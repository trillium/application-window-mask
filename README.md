# PII Mask

Real-time privacy masking for OBS Studio. Automatically blurs, pixelates, or
blacks out windows from apps you haven't explicitly marked as safe — so you can
stream freely without leaking email, messages, banking, or anything else.

**Default-deny:** only apps on your allow list are visible. Everything else is
masked. If the daemon crashes, the entire screen masks (fail-closed).

## How it works

```
┌─────────────────────┐         ┌──────────────────────┐
│  pii-mask daemon    │   shm   │  OBS plugin          │
│  (Swift)            │────────>│  (C, runs in OBS)    │
│                     │ 800B    │                      │
│  polls windows      │ seqlock │  reads mask rects    │
│  classifies safe/   │         │  composites clear +  │
│   unsafe            │         │   obfuscated frames  │
│  computes occlusion │         │  via SDF shader      │
│  writes mask rects  │         │                      │
└─────────────────────┘         └──────────────────────┘
        ▲                                ▲
        │ hot-reload                     │ filter on
        │                                │ Display Capture
~/.config/pii-mask/apps.toml         OBS Studio
```

**Daemon** monitors all on-screen windows via CoreGraphics, classifies them
against your allow list, computes z-order occlusion (only masks visible unsafe
regions), and writes up to 32 mask rectangles to POSIX shared memory.

**Plugin** reads the shared memory every frame via a lock-free seqlock, then
composites the clear capture with an obfuscated version using an SDF rounded-
rectangle shader. No image files, no HTTP, no ffmpeg — pure GPU pipeline.

## System requirements

- **macOS 13 (Ventura)** or later (CoreGraphics + Swift concurrency APIs)
- **OBS Studio 30+** (tested on 30.x, uses `gs_texrender` + effect API)
- **Screen Recording permission** for the daemon binary (System Settings →
  Privacy & Security → Screen Recording)
- **Accessibility permission** for event-driven window monitoring (AXObserver;
  System Settings → Privacy & Security → Accessibility)
- **Swift 5.9+** toolchain (Xcode 15+ or standalone; for building the daemon)
- **CMake 3.20+** (for building the OBS plugin; needs OBS source headers)
- **Apple Silicon or Intel Mac** (universal binary, both architectures supported)

### Runtime dependencies

| Component | Runs as | CPU cost |
|-----------|---------|----------|
| Daemon | Background process (launchd) | ~31ms/s at 5Hz polling |
| Plugin | OBS filter on Display Capture | ~260µs/frame GPU |

No Python runtime needed. No external services. No network calls.

## Quick start

### 1. Build the daemon

```bash
cd pii_mask/daemon/daemon_swift
swift build -c release
```

### 2. Build the OBS plugin

```bash
cd pii_mask/plugin/plugin_c
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
# Installs to ~/Library/Application Support/obs-studio/plugins/
make install
```

### 3. Start the daemon

```bash
.build/release/PiiMaskDaemon
```

Or install as a launchd user agent for auto-start:

```bash
cp install/com.trillium.pii-mask-daemon.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.trillium.pii-mask-daemon.plist
```

### 4. Add the filter in OBS

1. Right-click your Display Capture source → **Filters**
2. Click **+** → **PII Mask**
3. Choose obfuscation method: **Blur**, **Pixelate**, or **Solid black**
4. Status should show "Connected — N mask rects"

## Managing safe apps

The system is **default-deny**: every application is masked unless you
explicitly add it to the allow list. The `pii` CLI manages the list.

### Seeing what's on screen

```bash
pii show
```

Shows every visible application with its current classification:

```
STATUS   APP                            WINDOWS  NOTE
----------------------------------------------------------
● MASK   Activity Monitor                   1
○ SHOW   Code                              10
· SYS    Control Center                    16  (non-zero layer, ignored)
○ SHOW   Google Chrome                      1
● MASK   Messages                           2
○ SHOW   Terminal                           5
```

- `○ SHOW` — app is on the allow list, visible to viewers
- `● MASK` — app is masked (blurred/pixelated/blacked out)
- `· SYS` — system-level window (non-zero layer), ignored by the daemon

### Adding an app to the safe list

```bash
pii allow Discord
```

The app is immediately visible on stream. The daemon picks up the change
within one frame (~200ms at 5Hz). No restart needed.

You can use either the **process name** (as shown by `pii show`) or a
**bundle ID** (e.g. `com.tinyspeck.slackmacgap`):

```bash
pii allow "Google Chrome"    # process name (use quotes if spaces)
pii allow com.google.Chrome  # bundle ID also works
```

### Removing an app from the safe list

```bash
pii deny Slack
```

The app is immediately masked. `deny` also adds the app to the **always-mask**
list, which takes priority over the allow list — so even if someone adds it
back to allow, it stays masked until explicitly re-allowed.

To re-allow a previously denied app:

```bash
pii allow Slack    # removes from always-mask and adds to allow
```

### Viewing the current config

```bash
pii list
```

Shows both lists:

```
ALLOWED (shown on stream):
  + Code
  + Google Chrome
  + Terminal

ALWAYS MASKED:
  - NotificationCenter
  - SecurityAgent

Everything else: masked (default-deny)
```

### Config file

Config lives at `~/.config/pii-mask/apps.toml`:

```toml
allow = [
    "Code",
    "Google Chrome",
    "Terminal",
]

always_mask = [
    "NotificationCenter",
    "SecurityAgent",
]
```

You can edit this file directly — the daemon hot-reloads on save. You can also
send `SIGHUP` to force a reload: `kill -HUP $(pgrep PiiMaskDaemon)`

### Default allow list

```
Code, Google Chrome, OBS, OBS Studio, RODE Connect, Talon,
Terminal, iTerm2, Slack, System Settings, System Preferences, Finder
```

Everything else is masked. `Notification Center` and `SecurityAgent` are always
masked regardless of the allow list.

### Talon voice integration

Shell out to `pii` from Talon voice commands:

```talon
stream allow discord: user.run_shell("pii allow Discord")
stream deny slack:    user.run_shell("pii deny Slack")
stream show apps:     user.run_shell("pii show")
```

## Architecture

### Daemon pipeline (per frame)

1. **Poll** — `CGWindowListCopyWindowInfo` enumerates all on-screen windows
2. **Classify** — check each window's owner against allow/always-mask sets
3. **Occlude** — walk windows front-to-back, subtract safe coverage from unsafe
   regions, emit only the visible unsafe portions
4. **Clip** — clip rects to screen bounds, drop sub-pixel fragments
5. **Write** — if scene changed, write rects to shm via seqlock; otherwise
   heartbeat (timestamp only)

### Event-driven monitoring

The daemon doesn't just poll at 30Hz. It uses macOS event sources for low-
latency response:

- **NSWorkspace notifications** — app activation, launch, termination
- **AXObserver** — window move, resize, create, destroy, minimize
- **5Hz fallback timer** — catches overlays, notifications, Spotlight

Events are coalesced within a 16ms window to prevent floods during window drags.

### Shared memory protocol

800-byte POSIX shared memory segment (`/pii_mask`):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Magic (`0x50494D53` = "PIMS") |
| 4 | 4 | Version (1) |
| 8 | 4 | Sequence (seqlock counter, atomic) |
| 12 | 4 | Rect count (0–32) |
| 16 | 8 | Timestamp (ns, CLOCK_REALTIME) |
| 24 | 4 | Flags (bit 0: alive, bit 1: full mask) |
| 28 | 2+2 | Screen width + height (uint16) |
| 32 | 768 | 32 × 24-byte rects (x/y/w/h/radius as float + flags as uint32) |

**Seqlock protocol:** writer increments sequence to odd before writing, even
after. Reader skips if odd (mid-write) and retries if sequence changed (torn
read). Lock-free, wait-free on the reader side.

**Staleness:** plugin checks timestamp every frame. If older than 5 seconds,
daemon is presumed dead and the plugin falls back to full-screen masking
(fail-closed).

### GPU pipeline (plugin)

1. **Capture** — render source to `gs_texrender_t` (clear frame)
2. **Obfuscate** — apply selected method to clear frame:
   - Dual Kawase blur (1–6 levels, downsample/upsample pyramid)
   - Pixelation (configurable block size 2–64px)
   - Solid black
3. **Composite** — SDF shader blends clear + obfuscated per-pixel:
   `output = lerp(clear, obfuscated, sdf_mask_alpha)`
   with smoothstep anti-aliasing at rect edges

Rect data is uploaded as a 1D RGBA32F texture (2 texels per rect). Coordinates
are scaled from daemon screen space to OBS output resolution.

## Project structure

```
pii_mask/
├── daemon/
│   ├── daemon_swift/        ← Swift daemon (current)
│   │   ├── Package.swift
│   │   └── Sources/
│   │       ├── PiiMaskDaemon/   main, poller, classifier, occlusion,
│   │       │                    scene model, shm writer, config, events
│   │       └── CSeqlock/        C interop for atomic seqlock ops
│   └── daemon_python/       ← Python daemon (original, deprecated)
├── plugin/
│   └── plugin_c/            ← OBS plugin (C)
│       ├── pii_mask_filter.c    main filter: tick, render, properties
│       ├── shm_reader.c         shared memory reader + seqlock
│       ├── mask_renderer.c      SDF composite shader driver
│       ├── rect_texture.c       rect data → GPU texture upload
│       ├── blur_kawase.c        dual Kawase blur pipeline
│       └── data/*.effect        GLSL/HLSL shader effects
├── protocol/
│   ├── pii_mask_protocol_c.h       canonical struct layout (800 bytes)
│   ├── pii_mask_protocol_python.py  Python reader/writer
│   └── pii_mask_protocol_swift.swift  Swift reader/writer (needs update)
├── cli/
│   └── pii                  ← CLI tool for managing safe apps
├── config/
│   └── config.toml          ← reference config (design artifact)
├── talon_shim/              ← Talon voice command integration
└── failsafe/                ← fail-closed safety logic
```

## Performance

Benchmarked on M3 MacBook Pro, ~55 on-screen windows:

| Metric | Python daemon (30Hz) | Swift daemon (5Hz + events) |
|--------|---------------------|-----------------------------|
| Poll avg | 11.8ms | 6.1ms |
| Poll p99 | 41ms | 16ms |
| CPU per second | 354ms/s | 31ms/s |
| Memory | ~42MB (Python RSS) | ~5MB |
| Plugin GPU/frame | 260µs | 260µs (unchanged) |

The bottleneck is `CGWindowListCopyWindowInfo` itself — irreducible at ~6ms for
55 windows. The Swift rewrite eliminated PyObjC bridging overhead (1.9x) and
the event-driven architecture reduced poll rate from 30Hz to 5Hz (6x), for a
combined **11.4x reduction** in CPU cost.

## Safety invariants

1. **Default-deny** — unlisted apps are masked
2. **Fail-closed** — daemon crash → full screen mask (5s staleness timeout)
3. **Startup mask** — daemon writes full mask before first poll
4. **Always-mask list** — some processes (notifications, auth dialogs) are
   masked regardless of allow list
5. **No shm_unlink on shutdown** — plugin mmap stays valid across daemon
   restarts

## License

MIT — see [LICENSE](LICENSE).
