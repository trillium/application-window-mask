"""
pii_mask daemon — entry point.

Polls macOS for window positions, classifies them, and writes
mask geometry to shared memory for the OBS plugin to read.
"""

import signal
import sys
import time

from shm_writer import ShmWriter
from window_poller import WindowPoller
from classifier import Classifier
from scene_model import SceneModel
from occlusion import compute_visible_unsafe


def main():
    poll_hz = 30
    poll_interval = 1.0 / poll_hz

    classifier = Classifier()
    poller = WindowPoller()
    model = SceneModel()
    writer = ShmWriter()

    # Get screen dimensions for coordinate scaling
    from Quartz import CGMainDisplayID, CGDisplayPixelsWide, CGDisplayPixelsHigh
    display_id = CGMainDisplayID()
    screen_w = CGDisplayPixelsWide(display_id)
    screen_h = CGDisplayPixelsHigh(display_id)

    writer.open(screen_width=screen_w, screen_height=screen_h)
    print(f"Screen: {screen_w}x{screen_h}")

    # Fail-safe: start fully masked
    writer.write_full_mask()

    running = True

    def handle_signal(sig, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print(f"pii_mask daemon started (polling at {poll_hz}Hz)")

    try:
        while running:
            loop_start = time.monotonic()

            # Poll all on-screen windows
            windows = poller.poll()

            # Classify each window (preserving front-to-back z-order)
            # Skip overlay layers (layer != 0) from occlusion — they're
            # transparent and don't actually hide content beneath them.
            classified = []
            for w in windows:
                if w["layer"] != 0:
                    continue
                classified.append({
                    "x": w["x"],
                    "y": w["y"],
                    "width": w["width"],
                    "height": w["height"],
                    "unsafe": classifier.is_unsafe(
                        owner=w["owner"],
                        bundle=w.get("bundle", ""),
                        title=w.get("title", ""),
                        layer=w["layer"],
                    ),
                })

            # Compute visible unsafe regions (z-order aware)
            visible_unsafe = compute_visible_unsafe(classified)

            # Clip to screen bounds and drop off-screen/tiny rects
            rects = []
            for r in visible_unsafe:
                x, y, w, h = r
                # Clip to screen (0,0)
                if x < 0:
                    w += x
                    x = 0
                if y < 0:
                    h += y
                    y = 0
                if w <= 1 or h <= 1:
                    continue
                rects.append({
                    "x": x,
                    "y": y,
                    "width": w,
                    "height": h,
                    "corner_radius": 0.0,
                    "unsafe": True,
                })

            # Update scene model — write to shm if changed, or heartbeat
            if model.update(rects):
                writer.write_rects(rects)
            else:
                writer.heartbeat()

            # Sleep for remainder of poll interval
            elapsed = time.monotonic() - loop_start
            sleep_time = poll_interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        pass
    finally:
        print("pii_mask daemon shutting down")
        writer.close()


if __name__ == "__main__":
    main()
