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

            # Classify each window
            rects = []
            for w in windows:
                is_unsafe = classifier.is_unsafe(
                    owner=w["owner"],
                    bundle=w.get("bundle", ""),
                    title=w.get("title", ""),
                    layer=w["layer"],
                )
                if is_unsafe:
                    rects.append({
                        "x": w["x"],
                        "y": w["y"],
                        "width": w["width"],
                        "height": w["height"],
                        "corner_radius": 10.0,
                        "unsafe": True,
                    })

            # Update scene model — only write to shm if something changed
            if model.update(rects):
                writer.write_rects(rects)

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
