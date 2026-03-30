# pii_mask Architecture

Greenfield design for a privacy masking system for OBS live streaming. This replaces the
current Talon/ffmpeg-based mask generation with a standalone daemon optimized for speed
and reliability.

## 1. Requirements

### Core Functional Requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| R1 | Mask generation must complete within one frame period (16ms at 60fps, 33ms at 30fps) | P0 |
| R2 | Any window belonging to an app NOT on the safe list must be masked (white) | P0 |
| R3 | Notification banners from any app must be masked regardless of safe list | P0 |
| R4 | System must detect app focus changes and update the mask before OBS renders the next frame | P0 |
| R5 | If the mask service crashes or is unreachable, OBS must show a fully masked (white) screen | P0 |
| R6 | Overlays, popups, Spotlight, Siri, Control Center, and password prompts (layer > 0) must be masked | P1 |
| R7 | Multiple overlapping windows from different safety classifications must be composited correctly (unsafe on top of safe means that region is masked) | P1 |
| R8 | Multi-monitor support with independent mask files per display | P2 |
| R9 | Safe/unsafe app list must be configurable at runtime without restart | P2 |
| R10 | Per-window-title granularity (e.g., Chrome tab "Gmail" is unsafe, Chrome tab "Docs" is safe) | P3 |

### Non-Functional Requirements

| ID | Requirement |
|----|-------------|
| NF1 | macOS Sonoma/Sequoia only |
| NF2 | Must not block Talon's main thread (the SIGABRT problem) |
| NF3 | Must not require additional macOS permissions beyond what Talon already has (Screen Recording + Accessibility) |
| NF4 | CPU usage under 5% sustained during steady state (no window changes) |
| NF5 | Mask file must be a valid PNG that OBS's `mask_filter_v2` can read |
| NF6 | System must handle the streamer switching apps at any speed (rapid cmd-tab) |

## 2. Architecture

### Component Diagram

```
+-------------------------------------------------------------------+
|  EVENT SOURCES                                                     |
|                                                                    |
|  +------------------+    +------------------+    +--------------+  |
|  | Talon ui.register|    | CG Window Poller |    | Notification |  |
|  | (win_focus,      |    | (30Hz fallback)  |    | Monitor      |  |
|  |  win_open, etc.) |    |                  |    | (layer > 0)  |  |
|  +--------+---------+    +--------+---------+    +------+-------+  |
|           |                       |                      |         |
+-----------+-----------------------+----------------------+---------+
            |                       |                      |
            v                       v                      v
+-------------------------------------------------------------------+
|  MASK DAEMON  (standalone Python process, Unix domain socket API)  |
|                                                                    |
|  +------------------+     +------------------+                     |
|  | Window Classifier |     | Mask Compositor  |                    |
|  | - safe app list  |     | - numpy array    |                    |
|  | - layer rules    |     | - rect painting  |                    |
|  +--------+---------+     +--------+---------+                    |
|           |                        |                               |
|           v                        v                               |
|  +------------------+     +------------------+                     |
|  | Scene Model      |     | File Writer      |                    |
|  | - all windows    |     | - PIL save       |                    |
|  | - z-order        |     | - atomic rename  |                    |
|  | - dirty flag     |     | - double buffer  |                    |
|  +------------------+     +--------+---------+                    |
|                                    |                               |
+------------------------------------+-------------------------------+
                                     |
                                     v
                        +---------------------------+
                        | ~/Downloads/left_mask.png |
                        | (atomic os.replace)       |
                        +-------------+-------------+
                                      |
                                      v
                        +---------------------------+
                        | OBS Studio                |
                        | mask_filter_v2 on         |
                        | Display Capture LEFT BLUR |
                        +---------------------------+
```

### Component Descriptions

**Talon Shim** — A minimal Talon user script (~30 lines) that registers for `win_focus`,
`win_open`, `win_close`, and `app_activate` events. On each event, it sends a message over
a Unix domain socket to the mask daemon. Intentionally thin to avoid blocking Talon.

**CG Window Poller** — A background thread inside the mask daemon that calls
`CGWindowListCopyWindowInfo` at 30Hz. Catches everything Talon events miss: overlays on
non-standard layers, transient popups, window moves/resizes without focus changes. At
1-3ms per call, this costs ~6-9% of a single core at 30Hz.

**Notification Monitor** — Detects `com.apple.notificationcenterui` windows via the CG
poller's layer detection (layer > 0). Only needs the bounding rect, which CGWindowList
provides.

**Window Classifier** — Determines whether each window rect should be masked. Inputs:
owner name, window layer, optional title. Outputs: mask color (black=reveal, white=hide).
Rules loaded from TOML config, reloadable at runtime via SIGHUP or socket command.

**Scene Model** — Maintains the current set of on-screen windows with rects, z-order, and
classifications. Sets a dirty flag on change. Compositor only runs when dirty.

**Mask Compositor** — Takes the scene model and produces a mask image. Starts with a white
(fully masked) canvas. Paints black rectangles for safe windows in z-order (back to front),
then white for any unsafe windows/overlays in front. Uses numpy for array operations.

**File Writer** — Writes mask PNG to a temp file, then does atomic `os.replace()` to the
target path. OBS never reads a half-written file.

## 3. Data Flow: Window Change to Mask Ready

```
T=0.0ms  Talon fires win_focus event
         Shim sends {type:"focus", app:"Messages", pid:1234} over Unix socket

T=0.1ms  Mask daemon receives event on asyncio event loop

T=0.3ms  Window Classifier checks "Messages" against safe list --> UNSAFE
         Scene Model marks dirty

T=0.5ms  CG Window Poller may also fire (if polling cycle aligns),
         confirming window rects. Not required for the fast path.

T=0.8ms  Compositor runs:
         1. Create numpy ones * 255 --> all white (SAFE DEFAULT)
         2. For each SAFE window in z-order (back to front):
            Paint black rect via numpy slice assignment
         3. For each UNSAFE overlay (layer > 0) in z-order:
            Paint white rect via numpy slice assignment
         Cost: ~0.3ms for array creation, ~0.01ms per rect

T=1.5ms  PIL Image.fromarray() + save to /tmp/mask_staging.png
         Cost: ~2-4ms for 1728x1117 grayscale PNG (compression level 1)

T=5.0ms  os.replace("/tmp/mask_staging.png", "~/Downloads/left_mask.png")
         Cost: ~0.1ms (atomic rename, same filesystem)

T=5.1ms  Mask is ready on disk for OBS to read on its next render cycle
```

**Total latency: ~5ms from event to mask-on-disk.** Well within the 16ms frame budget.

## 4. Performance Budget

| Phase | Time | Notes |
|-------|------|-------|
| Event delivery (Talon to daemon) | 0.1ms | Unix domain socket, same machine |
| Classification + scene model update | 0.3ms | Dict lookup, flag set |
| Numpy array creation (1728x1117 uint8) | 0.3ms | `np.full()` |
| Rect painting (typical 5-10 rects) | 0.1ms | Numpy slice assignment |
| PIL Image.fromarray + PNG save | 2-4ms | Grayscale, compression level 1 |
| Atomic file rename | 0.1ms | `os.replace()` |
| **Total** | **~3-5ms** | |
| CG poller per cycle (background) | 1-3ms | `CGWindowListCopyWindowInfo` |
| CG poller budget at 30Hz | ~60-90ms/s | ~6-9% of one core |

### Comparison to Current System

| Operation | Current (ffmpeg) | Proposed (numpy+PIL) |
|-----------|-----------------|---------------------|
| Full mask generation | ~100-200ms | ~3-5ms |
| Single rect update | ~20ms | ~3-5ms (full recomposite) |
| Process overhead | Fork+exec per operation | None (in-process) |
| Talon thread blocking | YES (SIGABRT) | NO (separate process) |

## 5. Tech Stack

### Primary: Python standalone daemon

**Justification:**
- **numpy** for mask array operations is 100x faster than ffmpeg subprocess, no forking
- **Pillow (PIL)** for PNG encoding is fast enough (2-4ms at low compression)
- Python has native access to `CGWindowListCopyWindowInfo` via `pyobjc`
- Unix domain sockets via `asyncio` give sub-millisecond IPC with Talon
- Talon's Python runtime already has `pyobjc`

**Why NOT Swift/Rust:**
- `pyobjc` provides `CGWindowListCopyWindowInfo` with identical performance (1-3ms)
- numpy is fast enough for rect painting (sub-millisecond). Bottleneck is PNG encoding,
  where Rust would save maybe 1-2ms — not worth the IPC complexity.
- If PNG encoding becomes the bottleneck, the answer is an OBS plugin (see Phase 4), not
  a different language.

### Dependencies

| Package | Purpose |
|---------|---------|
| numpy | Mask array operations |
| Pillow | PNG encoding |
| pyobjc-framework-Quartz | CGWindowListCopyWindowInfo |
| tomllib (stdlib 3.11+) | Config parsing |

### File Layout

```
pii_mask/
  pyproject.toml
  config.toml              # Safe app list, mask paths, polling rate
  src/
    pii_mask/
      __init__.py
      daemon.py            # Main entry point, asyncio event loop
      compositor.py        # Numpy mask generation
      classifier.py        # Window safety classification
      scene_model.py       # Window state tracking, dirty detection
      cg_poller.py         # CGWindowListCopyWindowInfo polling thread
      ipc.py               # Unix domain socket server
      file_writer.py       # Atomic PNG write
      config.py            # Config loading from TOML
  talon_shim/
    pii_mask_shim.py       # Minimal Talon script, sends events to daemon
  tests/
    test_compositor.py
    test_classifier.py
    test_scene_model.py
```

## 6. Fail-Safe Mechanisms

### Default-White Canvas (Mask-First)

The compositor starts with a **fully white** (masked) canvas and only reveals (paints
black) regions that are explicitly classified as safe. Unknown apps are masked by default.
If classification fails, the region stays masked. If a window rect is missing, nothing is
revealed.

### Pre-Mask on Switch

When Talon fires `win_focus` for an unsafe app:
1. Immediately write a fully-white mask (5ms)
2. OBS picks it up on the next frame — entire screen is hidden
3. Then composite the proper mask with safe windows revealed
4. Write the composited mask — OBS picks it up, showing safe regions

One frame of full-mask before the proper mask appears. Imperceptible.

### Crash Recovery

**Mask daemon crashes:** Last-written mask PNG remains on disk. Talon shim detects daemon
unreachable (socket connect fails) and triggers `obs_get_blurry()` to switch OBS to
full-blur scene (existing safety net).

**Talon crashes:** CG poller continues running independently. Daemon does not depend on
Talon for correctness, only for lower-latency event delivery.

**OBS crashes:** Nothing to do. When it restarts, it reads the current mask file.

### Atomic File Writes

`os.replace()` is atomic on macOS (POSIX rename). OBS sees either the old complete mask
or the new complete mask, never a partial write.

### Watchdog

If the compositor has not run in 5 seconds AND there are on-screen windows, force a
recomposite. Catches edge cases where an event was lost.

### Startup Behavior

On daemon startup, before any events are processed:
1. Write a fully-white mask to the mask path
2. Run a full CG window enumeration and composite the proper mask

Safe even if daemon starts after OBS.

## 7. Event Architecture

### Hybrid: Events + Polling

```
FAST PATH (event-driven, <1ms latency):
  Talon win_focus/win_open/win_close/app_activate
    --> Unix socket --> daemon --> immediate recomposite

CATCH-ALL (polling, 33ms worst-case latency):
  CG Window Poller at 30Hz
    --> Compares current window list to scene model
    --> If different: update scene model, mark dirty, recomposite
```

**Why both?** Talon events are fast but incomplete. They don't fire for:
- Notification banners appearing (no `win_focus` for a notification)
- Spotlight/Siri/Control Center overlays
- Window resize/move without focus change
- Apps that open windows without taking focus

The CG poller catches all of these. At 30Hz, worst case is 33ms — one frame at 30fps.

## 8. Migration Path

### Phase 1: Standalone Daemon (replaces ffmpeg)

1. Build mask daemon with compositor, classifier, scene model, file writer
2. Build Talon shim that sends events over Unix socket
3. Test side-by-side: daemon writes to a different file path, compare to current masks
4. Swap mask path in OBS filter config
5. Disable ffmpeg calls in `trillium_obs/streaming/create_safety_mask.py`
6. Keep `keyboard_blurr_on_window_change.talon` safety net

### Phase 2: Remove OBS Listener Dependency for Blur

The mask already hides unsafe content per-frame, so blur-on-switch becomes unnecessary.
No need to switch to a full-blur scene during transitions. OBS listener remains for other
coordination needs.

### Phase 3: CG Poller Integration

1. Add CG poller thread for overlays and notifications
2. Remove dependency on Talon for notification detection
3. Talon shim becomes optional — daemon can run fully standalone

### Phase 4: OBS Plugin (optional, v2)

If file I/O becomes a bottleneck, build a minimal OBS C plugin that reads mask data from
shared memory or a socket. Eliminates PNG encode/decode entirely.

## 9. Open Questions

1. **How frequently does OBS re-read the mask PNG?** The `mask_filter_v2` likely checks
   file mtime each frame. If it caches aggressively, may need to toggle the filter via
   obs-websocket. Needs empirical testing.

2. **Retina scaling.** Current mask is 1728x1117 (logical). Display capture may be 2x
   (3456x2234). Need to verify which resolution OBS expects. If 2x, ~1ms more for PNG.

3. **PNG compression tradeoff.** Lower compression = faster write, larger file. Start with
   compression level 1 (fastest).

4. **Mission Control / Expose.** Windows animate to thumbnails. CG poller sees rapidly
   changing rects. Should debounce or detect Mission Control state and hold current mask.
