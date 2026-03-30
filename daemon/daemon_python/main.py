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

    writer.open()

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
            classified = []
            for w in windows:
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

            rects = [
                {
                    "x": r[0],
                    "y": r[1],
                    "width": r[2],
                    "height": r[3],
                    "corner_radius": 0.0,
                    "unsafe": True,
                }
                for r in visible_unsafe
            ]

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
