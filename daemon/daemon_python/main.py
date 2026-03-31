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
from config_loader import ConfigWatcher


def main():
    poll_hz = 30
    poll_interval = 1.0 / poll_hz

    config_watcher = ConfigWatcher()
    cfg = config_watcher.config
    classifier = Classifier(allow=cfg["allow"], always_mask=cfg["always_mask"])
    poller = WindowPoller()
    model = SceneModel()
    writer = ShmWriter()
    print(f"Config: {len(cfg['allow'])} allowed, "
          f"{len(cfg['always_mask'])} always-masked")

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

    # Benchmark accumulators
    bench_frame = 0
    bench_interval = 300  # log every 300 frames (~10s)
    bench_times = {"poll": [], "classify": [], "occlusion": [],
                   "clip": [], "write": [], "total": []}

    try:
        while running:
            loop_start = time.perf_counter()

            # Hot-reload config if file changed
            new_cfg = config_watcher.check()
            if new_cfg:
                classifier.update_lists(new_cfg["allow"], new_cfg["always_mask"])
                print(f"Config reloaded: {len(new_cfg['allow'])} allowed, "
                      f"{len(new_cfg['always_mask'])} always-masked")

            # Poll all on-screen windows
            t0 = time.perf_counter()
            windows = poller.poll()
            t_poll = time.perf_counter() - t0

            # Classify each window (preserving front-to-back z-order)
            # Skip overlay layers (layer != 0) from occlusion — they're
            # transparent and don't actually hide content beneath them.
            t0 = time.perf_counter()
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
            t_classify = time.perf_counter() - t0

            # Compute visible unsafe regions (z-order aware)
            t0 = time.perf_counter()
            visible_unsafe = compute_visible_unsafe(classified)
            t_occlusion = time.perf_counter() - t0

            # Clip to screen bounds and drop off-screen/tiny rects
            t0 = time.perf_counter()
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
            t_clip = time.perf_counter() - t0

            # Update scene model — write to shm if changed, or heartbeat
            t0 = time.perf_counter()
            if model.update(rects):
                writer.write_rects(rects)
            else:
                writer.heartbeat()
            t_write = time.perf_counter() - t0

            t_total = time.perf_counter() - loop_start

            # Accumulate benchmark data
            bench_times["poll"].append(t_poll)
            bench_times["classify"].append(t_classify)
            bench_times["occlusion"].append(t_occlusion)
            bench_times["clip"].append(t_clip)
            bench_times["write"].append(t_write)
            bench_times["total"].append(t_total)
            bench_frame += 1

            if bench_frame >= bench_interval:
                for key in bench_times:
                    vals = sorted(bench_times[key])
                    n = len(vals)
                    p50 = vals[n // 2] * 1000
                    p99 = vals[int(n * 0.99)] * 1000
                    avg = sum(vals) / n * 1000
                    print(f"  BENCH {key:>10s}: "
                          f"avg={avg:.3f}ms  "
                          f"p50={p50:.3f}ms  "
                          f"p99={p99:.3f}ms")
                print(f"  BENCH windows={len(windows)} "
                      f"classified={len(classified)} "
                      f"rects={len(rects)}")
                bench_times = {k: [] for k in bench_times}
                bench_frame = 0

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
