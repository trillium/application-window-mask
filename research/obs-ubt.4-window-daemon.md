# Window Daemon Implementation Research

Research for pii_mask's window detection daemon -- a standalone process that monitors
all on-screen windows via `CGWindowListCopyWindowInfo`, receives events from Talon,
maintains a scene model, classifies windows as safe/unsafe, and writes mask geometry
to shared memory (or file).

---

## 1. Implementation Options Comparison

### Option 1: Python + pyobjc

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/runtime** | Python 3.11+ | |
| **How it works** | `pyobjc-framework-Quartz` wraps `CGWindowListCopyWindowInfo` directly. Daemon runs asyncio event loop, polls CG at 30Hz, receives Talon events over Unix socket. numpy for mask composition, PIL for PNG output. |
| **Ease of writing** | 5 | Minimal boilerplate. `from Quartz import CGWindowListCopyWindowInfo` just works. Full stdlib access for sockets, asyncio, signal handling. |
| **Ease of maintenance** | 4 | Python is readable. pyobjc tracks macOS releases (12.1 released Nov 2025). Risk: pyobjc API churn when Apple changes frameworks. |
| **Type safety** | 2 | Dynamic typing. CFDictionary results come back as Python dicts with string keys. No compile-time checks on key names or value types. mypy helps but doesn't cover ObjC bridge. |
| **Performance** | 3 | CGWindowListCopyWindowInfo: ~1-3ms (same as native -- pyobjc is a thin bridge). numpy mask ops: sub-ms. PIL PNG encode: 2-4ms. GIL limits true parallelism but asyncio + thread pool works. |
| **Memory overhead** | 2 | Python interpreter: ~30-50MB RSS. numpy + PIL add ~20MB. Total ~50-70MB for the daemon. |
| **macOS API access** | 4 | pyobjc wraps nearly all macOS frameworks. CGWindowList, NSWorkspace, AXUIElement all available. Missing: some newer Swift-only APIs. |
| **Pros** | | Fastest time to working prototype. Talon ecosystem is Python -- shared knowledge, debugging tools. numpy mask composition is already proven in ARCHITECTURE.md. Hot-reloadable config. |
| **Cons** | | GIL limits CPU parallelism (not a real issue for this workload). Memory footprint. No compile-time safety for ObjC bridge calls. |
| **Key reference** | | [pyobjc docs](https://pyobjc.readthedocs.io/), [pyobjc-framework-Quartz on PyPI](https://pypi.org/project/pyobjc-framework-Quartz/) |

### Option 2: Python + subprocess to Swift helper

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/runtime** | Python 3.11+ orchestrator, Swift CLI helper | |
| **How it works** | Python daemon handles event loop, classification, mask writing. Calls a small compiled Swift binary (~20 lines) that runs `CGWindowListCopyWindowInfo` and prints JSON to stdout. Python parses JSON output. |
| **Ease of writing** | 4 | Python side is same as Option 1. Swift helper is trivial (~20 lines). Build step adds friction. |
| **Ease of maintenance** | 3 | Two languages, two build systems. Swift helper must be recompiled for new macOS SDKs. JSON serialization/parsing adds a contract to maintain. |
| **Type safety** | 2 | Python side same as Option 1. Swift helper is type-safe internally but the JSON boundary erases types. |
| **Performance** | 2 | subprocess fork+exec per poll cycle: ~5-15ms overhead. At 30Hz that's 150-450ms/s of process creation. Could mitigate with long-running helper + pipe, but then you're building IPC. |
| **Memory overhead** | 3 | Python ~50MB + Swift helper transient (or ~10MB if long-running). Slightly better than pure Python if helper is transient. |
| **macOS API access** | 5 | Swift helper has full native access. Python side uses pyobjc for anything else it needs. Best of both worlds for API coverage. |
| **Pros** | | Swift helper can use APIs not exposed through pyobjc. Clean separation of concerns. Swift helper is independently testable. |
| **Cons** | | Subprocess overhead kills the latency budget if called per poll cycle. Long-running helper with pipe IPC adds complexity that approaches "just write it in Swift." Two-language debugging. |
| **Key reference** | | [Apple CGWindowListCopyWindowInfo docs](https://developer.apple.com/documentation/coregraphics/cgwindowlistcopywindowinfo(_:_:)) |

### Option 3: Pure Swift daemon

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/runtime** | Swift 5.9+ / macOS native | |
| **How it works** | Standalone Swift binary. Uses `CGWindowListCopyWindowInfo` directly. Runs a `DispatchSource` or Combine-based event loop. Receives Talon events via Unix socket. Writes mask as raw bytes to shared memory or PNG to disk. |
| **Ease of writing** | 3 | Swift's ObjC interop is seamless for CG/NS APIs. Socket programming and event loops are more boilerplate than Python. No numpy equivalent -- need vImage or manual pixel buffer manipulation. |
| **Ease of maintenance** | 4 | Single language. Apple's first-class language for macOS. Xcode tooling. ABI stability since Swift 5.1. |
| **Type safety** | 5 | Full compile-time type safety. CGWindowList returns typed dictionaries. Enums for window layers. |
| **Performance** | 5 | Zero bridging overhead for CG calls. vImage/Accelerate for mask composition is SIMD-optimized. PNG encoding via ImageIO is native and fast (~1-2ms). Shared memory writes are zero-copy. |
| **Memory overhead** | 5 | ~5-10MB RSS for a minimal Swift daemon. No interpreter, no runtime bloat. |
| **macOS API access** | 5 | First-class access to all macOS frameworks. CoreGraphics, AppKit, Accessibility, NSWorkspace notifications, AXObserver -- all native. Can use newest APIs immediately on release. |
| **Pros** | | Best performance. Lowest memory. Best type safety. Apple's recommended language. Can use `NSWorkspace.didActivateApplicationNotification` for zero-latency app switch detection. vImage for SIMD mask composition. launchd integration is trivial. |
| **Cons** | | Slower to prototype than Python. No numpy -- need to learn vImage/Accelerate or CoreGraphics pixel buffer APIs. Talon ecosystem is Python, so debugging integration requires crossing language boundaries. Compilation step. |
| **Key reference** | | [Swindler - Swift window management library](https://github.com/tmandry/Swindler), [Apple CoreGraphics docs](https://developer.apple.com/documentation/coregraphics) |

### Option 4: Rust + core-graphics crate

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/runtime** | Rust with `core-graphics` and `core-foundation` crates | |
| **How it works** | Uses `core-graphics::display::CGWindowListCopyWindowInfo` (unsafe FFI). Tokio or mio for async event loop. Unix socket for Talon IPC. Raw pixel buffer for mask, `image` crate for PNG encoding. |
| **Ease of writing** | 2 | CGWindowList bindings exist but are thin wrappers around unsafe C. Must manually manage CFArray/CFDictionary iteration with `CFRelease`. Borrow checker fights with ObjC reference semantics. |
| **Ease of maintenance** | 3 | Rust is maintainable once written. But `core-graphics` crate (servo project) has inconsistent maintenance. `objc2-core-graphics` is newer alternative. Ecosystem fragmentation. |
| **Type safety** | 5 | Rust's type system is the strongest here. But CGWindowList returns `CFArrayRef` which must be manually typed -- the safety is in your code, not the bindings. |
| **Performance** | 5 | Equivalent to Swift for CG calls. Zero-cost abstractions. Excellent for mask buffer manipulation. |
| **Memory overhead** | 5 | ~3-8MB RSS. No runtime, no GC. Smallest footprint of all options. |
| **macOS API access** | 3 | CGWindowList and basic CG functions are wrapped. NSWorkspace notifications require `objc` crate FFI or `objc2` -- significantly more boilerplate than Swift. AXObserver bindings are sparse. |
| **Pros** | | Memory safety guarantees beyond Swift. Excellent concurrency story with tokio. If you already know Rust, the daemon will be rock-solid. |
| **Cons** | | Highest development time. macOS API bindings are incomplete and fragmented across crates. NSWorkspace/AXObserver require manual ObjC FFI. No first-class Apple support. `core-graphics` crate maintainers are slow to update for new macOS releases. |
| **Key reference** | | [core-graphics crate](https://crates.io/crates/core-graphics), [objc2-core-graphics](https://docs.rs/objc2-core-graphics/latest/objc2_core_graphics/) |

### Option 5: Go + cgo

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/runtime** | Go 1.21+ with cgo for CoreGraphics | |
| **How it works** | Uses cgo to call `CGWindowListCopyWindowInfo` via C bindings. Go handles concurrency via goroutines. Unix socket server in pure Go. Mask as `[]byte` buffer. |
| **Ease of writing** | 2 | cgo requires writing C shim code. CoreFoundation type conversion (CFArray -> Go slice) is tedious and error-prone. Go's lack of generics (pre-1.18) made this worse; generics help but CF types are still manual. |
| **Ease of maintenance** | 3 | Go code is maintainable. cgo shim layer is fragile -- must be updated when CG headers change. Cross-compilation with cgo is painful (Darwin-only). |
| **Type safety** | 3 | Go is statically typed but cgo boundary is unsafe. `C.CFArrayRef` is an opaque pointer. Must cast manually. |
| **Performance** | 4 | Go goroutines are efficient. cgo calls have ~100ns overhead per call (negligible). Mask buffer ops are fast but no SIMD without assembly. |
| **Memory overhead** | 4 | Go runtime: ~10-15MB RSS. Goroutine stack: 8KB each. Reasonable. |
| **macOS API access** | 2 | Only raw C APIs available through cgo. No ObjC bridge without additional libraries (like `progrium/macdriver`). NSWorkspace, AXObserver require separate ObjC shims. Very limited ecosystem for macOS development in Go. |
| **Pros** | | Excellent concurrency model. Single static binary. Fast compilation. |
| **Cons** | | cgo is the weak link -- complex, slow to compile, hard to debug. macOS API surface through cgo is minimal. Community support for macOS Go development is thin. Not the right tool for this job. |
| **Key reference** | | [atomical/coregraphics Go bindings](https://github.com/atomical/coregraphics), [progrium/macdriver](https://github.com/nicholasgasior/gopher-apple) |

### Option 6: Python + ctypes to CoreGraphics

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/runtime** | Python 3.11+ with ctypes (stdlib) | |
| **How it works** | Load `/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics` via `ctypes.cdll`. Manually define function signatures, call `CGWindowListCopyWindowInfo`, parse returned `CFArrayRef` by manually calling CF functions. |
| **Ease of writing** | 1 | Must manually define every function signature, handle CFRetain/CFRelease, convert CFString to Python str, iterate CFArray manually. Hundreds of lines of boilerplate for what pyobjc does in one import. |
| **Ease of maintenance** | 1 | Extremely fragile. No automatic updates when macOS changes framework layout. Memory management bugs (segfaults from premature CFRelease) are the #1 reported issue with this approach. |
| **Type safety** | 1 | ctypes provides zero type safety. Wrong function signature = segfault, not error. |
| **Performance** | 3 | Same CG call performance as pyobjc (~1-3ms). But CF type conversion overhead in Python is higher than pyobjc's C bridge. |
| **Memory overhead** | 3 | Avoids pyobjc's ~5MB overhead, but still has Python interpreter cost. Marginal savings. |
| **macOS API access** | 2 | Can technically call any C function, but ObjC methods (NSWorkspace, AXObserver) are nearly impossible through ctypes alone. Limited to C-level CoreGraphics functions. |
| **Pros** | | No external dependencies beyond stdlib. Works with any Python, including Talon's bundled Python. |
| **Cons** | | Extremely error-prone. Segfaults from memory management are well-documented. Massive boilerplate. No ObjC API access. This is reinventing pyobjc badly. |
| **Key reference** | | [comp.lang.python discussion on CG via ctypes](https://comp.lang.python.narkive.com/uniuqhjY/using-mac-os-x-coregraphics-via-ctypes), [PyGetWindow macOS backend](https://github.com/asweigart/PyGetWindow/blob/master/src/pygetwindow/_pygetwindow_macos.py) |

### Option 7: Objective-C daemon

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/runtime** | Objective-C with Foundation/CoreGraphics | |
| **How it works** | Traditional macOS daemon. `CGWindowListCopyWindowInfo` is a C function, called directly. `NSRunLoop` + `NSWorkspace` notifications for events. `CFSocket` or `NSFileHandle` for Unix socket IPC. CoreGraphics for mask buffer, ImageIO for PNG encoding. |
| **Ease of writing** | 2 | Verbose syntax. Manual memory management (ARC helps but CoreFoundation objects still need CFRelease). Socket programming is low-level. No modern concurrency (pre-Swift concurrency). |
| **Ease of maintenance** | 3 | Stable language -- ObjC hasn't changed meaningfully in years. Apple still supports it. But finding ObjC developers is increasingly difficult. |
| **Type safety** | 3 | Better than Python (static types, compiler warnings) but ObjC's `id` type and message passing are inherently dynamic. Dictionary access is untyped (`objectForKey:` returns `id`). |
| **Performance** | 5 | Identical to Swift -- both compile to native code with the same runtime. CG calls are direct C calls. |
| **Memory overhead** | 5 | ~5-10MB RSS, same as Swift. |
| **macOS API access** | 5 | Full access to everything. ObjC is the original macOS language. All frameworks, all APIs, no bridging needed. |
| **Pros** | | Proven, stable, fast. Every macOS framework was originally written for ObjC. Excellent debugger support. ARC handles most memory management. |
| **Cons** | | Archaic syntax. No modern concurrency (async/await). Increasingly rare skillset. Swift is strictly better for new macOS projects unless there's a specific legacy reason. |
| **Key reference** | | [Apple Creating Launch Daemons and Agents](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html) |

### Option 8: Node.js + native addon

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/runtime** | Node.js 20+ with N-API C++ addon | |
| **How it works** | C++ addon (compiled via node-gyp) wraps `CGWindowListCopyWindowInfo` and exposes it as a JS function. Node.js handles event loop, socket server, classification logic. Mask written as Buffer. |
| **Ease of writing** | 2 | N-API boilerplate for C++ addon is significant. Must convert between CF types and N-API values. JS side is easy but the addon is the hard part. |
| **Ease of maintenance** | 2 | N-API is ABI-stable across Node versions, which helps. But node-gyp builds are notoriously fragile. Two-language debugging. Node.js version management. |
| **Type safety** | 2 | TypeScript helps on JS side. C++ addon is type-safe internally. N-API boundary loses types (napi_value is opaque). |
| **Performance** | 3 | CG calls through addon are native speed. But V8 <-> C++ marshaling has overhead. Event loop is single-threaded. Mask buffer ops in JS are slower than numpy. |
| **Memory overhead** | 1 | V8 engine: ~50-100MB RSS. Heaviest option. |
| **macOS API access** | 3 | C++ addon can call any C/ObjC API via Objective-C++. But each API must be manually wrapped. |
| **Pros** | | Large ecosystem. Easy socket handling. If the team knows JS, the orchestration code is quick to write. |
| **Cons** | | Heaviest memory footprint. node-gyp build issues are legendary. No advantage over Python for this use case -- Python has better macOS bindings AND is the Talon ecosystem language. Adds a runtime dependency (Node.js) with no offsetting benefit. |
| **Key reference** | | [Node-API docs](https://nodejs.org/api/n-api.html), [napi-rs](https://napi.rs/) |

---

## 2. Summary Matrix

| Option | Write | Maintain | Types | Perf | Memory | macOS API | **Total** |
|--------|-------|----------|-------|------|--------|-----------|-----------|
| 1. Python + pyobjc | 5 | 4 | 2 | 3 | 2 | 4 | **20** |
| 2. Python + Swift helper | 4 | 3 | 2 | 2 | 3 | 5 | **19** |
| 3. Pure Swift | 3 | 4 | 5 | 5 | 5 | 5 | **27** |
| 4. Rust + core-graphics | 2 | 3 | 5 | 5 | 5 | 3 | **23** |
| 5. Go + cgo | 2 | 3 | 3 | 4 | 4 | 2 | **18** |
| 6. Python + ctypes | 1 | 1 | 1 | 3 | 3 | 2 | **11** |
| 7. Objective-C | 2 | 3 | 3 | 5 | 5 | 5 | **23** |
| 8. Node.js + addon | 2 | 2 | 2 | 3 | 1 | 3 | **13** |

**Top tier:** Pure Swift (27), Rust (23), Objective-C (23), Python+pyobjc (20)

---

## 3. Event Loop Architecture

### asyncio vs threading vs multiprocessing

| Approach | How it works | Fit for daemon | Verdict |
|----------|-------------|----------------|---------|
| **asyncio** | Single-threaded cooperative multitasking. `await` on socket reads, timers for polling. | Excellent. The daemon is I/O-bound (socket events, CG polling, file writes). asyncio handles all of these naturally. | **Best for Python option.** CG polling runs in `loop.run_in_executor()` (thread pool) to avoid blocking the event loop. |
| **threading** | OS threads with GIL. Can run CG poller on a background thread while main thread handles socket events. | Good. Simpler mental model than asyncio. But GIL means CG polling and event handling can't truly run in parallel. For this workload, that's fine -- neither is CPU-bound. | Viable but asyncio is cleaner for the socket server. |
| **multiprocessing** | Separate OS processes. Each has its own GIL. | Overkill. The daemon doesn't need CPU parallelism. IPC between processes adds latency and complexity. The only use case would be isolating the CG poller from a crash, but the poller is a single C call, not crash-prone. | Not recommended. |
| **GCD (Swift)** | `DispatchQueue` with `DispatchSource` for socket events and timers. | Native and optimal for Swift. `DispatchSource.makeTimerSource` for polling, `DispatchSource.makeReadSource` for socket. Zero overhead. | **Best for Swift option.** |
| **Swift Concurrency** | `async`/`await` with structured concurrency. `AsyncStream` for events. | Modern and clean. But CGWindowList is synchronous C API -- must be called from a detached task or actor to avoid blocking. | Good alternative to GCD for Swift. Slightly less control over thread placement. |

**Recommendation for Python:** asyncio event loop with `run_in_executor` for the CG polling thread.
**Recommendation for Swift:** GCD with DispatchSource, or Swift Concurrency actors.

---

## 4. CGWindowListCopyWindowInfo Polling Rate vs CPU Cost

### Empirical data (from ARCHITECTURE.md and research)

| Poll rate | Per-call cost | CPU/s (one core) | Worst-case latency | Notes |
|-----------|--------------|-------------------|---------------------|-------|
| 10 Hz | 1-3ms | 1-3% | 100ms | Too slow for overlays/notifications |
| 20 Hz | 1-3ms | 2-6% | 50ms | Acceptable. Misses sub-50ms transients. |
| **30 Hz** | **1-3ms** | **3-9%** | **33ms** | **Sweet spot.** Matches 30fps OBS. One frame worst-case. |
| 60 Hz | 1-3ms | 6-18% | 16ms | Matches 60fps. Higher CPU. Diminishing returns -- most window changes are > 33ms. |
| 120 Hz | 1-3ms | 12-36% | 8ms | Wasteful. No visible benefit over 60Hz. |

### Adaptive polling strategy

Instead of fixed-rate polling:
1. **Idle rate:** 10 Hz when no window changes detected in last 2 seconds
2. **Active rate:** 30 Hz when a window change was recently detected (Talon event or CG diff)
3. **Burst rate:** 60 Hz for 500ms after app switch (catches rapid window animations)

This cuts average CPU from ~6% to ~2% during steady state while maintaining responsiveness.

### Key finding: CGWindowListCopyWindowInfo cost scales with window count

The 1-3ms figure is for typical desktop usage (10-30 windows). With 100+ windows
(many browser tabs, IDE panes), cost can rise to 5-8ms. The daemon should monitor
call duration and back off polling rate if individual calls exceed a threshold.

---

## 5. Event Sources: NSWorkspace vs AXObserver vs Polling

### NSWorkspace Notifications

**What it catches:**
- `didActivateApplicationNotification` -- app gained focus
- `didDeactivateApplicationNotification` -- app lost focus
- `didLaunchApplicationNotification` -- app launched
- `didTerminateApplicationNotification` -- app quit
- `activeSpaceDidChangeNotification` -- space/desktop switch
- `didHideApplicationNotification` / `didUnhideApplicationNotification`

**What it misses:**
- Individual window open/close/move/resize within an app
- Overlay/popup appearance (Spotlight, notifications, Control Center)
- Window z-order changes without focus change
- Window title changes

**Latency:** Near-zero. Delivered via NSNotificationCenter on the run loop.

**Permission:** None (public API, no TCC).

**Reliability:** 5/5 for what it covers. These are first-class system notifications.

**Verdict:** Excellent for app-level lifecycle. Insufficient alone for window-level tracking.

### AXObserver

**What it catches:**
- `kAXWindowCreatedNotification` -- new window in observed app
- `kAXUIElementDestroyedNotification` -- window closed
- `kAXWindowMovedNotification` / `kAXWindowResizedNotification`
- `kAXFocusedWindowChangedNotification`
- `kAXTitleChangedNotification`
- `kAXWindowMiniaturizedNotification` / `kAXWindowDeminiaturizedNotification`

**What it misses:**
- Windows from apps you haven't registered an observer for
- System UI elements (Notification Center, Spotlight, Control Center) -- these
  don't expose AX observers reliably
- Z-order changes without move/resize

**Latency:** Near-zero for observed apps.

**Permission:** Accessibility (TCC).

**Reliability:** 3/5. Documentation is poor. Some events are delivered out of order or
not at all for certain apps. Swindler (Swift library) exists specifically to paper over
these gaps. Per-app observer registration means you must track all running apps and
register/deregister observers dynamically.

**Verdict:** Powerful but complex and incomplete. Best as a supplement, not primary source.

### CGWindowListCopyWindowInfo Polling

**What it catches:** Everything visible on screen. All windows from all apps, all layers,
including system UI, overlays, notifications. True z-order. Rects, PIDs, names, layers.

**What it misses:** Nothing that's on-screen. But: no events -- only snapshots. Must diff
consecutive snapshots to detect changes.

**Latency:** Polling interval (33ms at 30Hz worst case).

**Permission:** Screen Recording (TCC). Without it, window names are empty and bounds
may be missing for non-owned windows.

**Reliability:** 5/5. It's a snapshot of truth. No events to miss, no observers to register.

**Verdict:** The only complete source. Must be used for correctness. Polling cost is the tradeoff.

### Recommended hybrid approach

```
Layer 1: NSWorkspace notifications (app activate/deactivate)
  --> Instant app-switch detection, triggers immediate CG snapshot
  --> Zero-latency for the most common case (cmd-tab)

Layer 2: Talon events (win_focus, win_open, win_close)
  --> Redundant with NSWorkspace but catches window-level events
  --> Arrives via Unix socket from Talon shim

Layer 3: CGWindowListCopyWindowInfo polling (adaptive 10-30Hz)
  --> Ground truth. Catches everything layers 1-2 miss.
  --> Diff against scene model to detect changes.
  --> Only triggers recomposite when diff is non-empty.
```

**Why not AXObserver?** It adds complexity (per-app observer management) for coverage
that CGWindowList polling already provides. The latency advantage over polling (~0ms vs
~33ms) is only relevant for window move/resize, which are cosmetic -- a 33ms-stale mask
during a window drag is invisible to viewers.

Exception: If per-window-title granularity (R10 in ARCHITECTURE.md) becomes important,
AXObserver's `kAXTitleChangedNotification` is the only way to get real-time title
updates without polling.

---

## 6. Retina Scaling: Logical vs Physical Coordinates

### How macOS coordinate systems work

- **Logical points:** The coordinate system used by all macOS APIs (NSWindow, CGWindowList,
  AXUIElement). On a Retina display, 1 point = 2 physical pixels (2x scaling).
- **Physical pixels (backing pixels):** The actual framebuffer resolution.
- **Display points (scaled resolution):** What the user selected in System Settings >
  Displays. May differ from both logical and physical.

### CGWindowListCopyWindowInfo returns logical points

`kCGWindowBounds` values are in the **global display coordinate space** using **logical
points** (not physical pixels). On a MacBook Pro 14" (3024x1964 physical, 1512x982
logical at default scaling), a full-screen window reports bounds of
`{x: 0, y: 0, width: 1512, height: 982}`.

### Display capture resolution

OBS Display Capture captures at the **physical pixel resolution** (the actual framebuffer).
So on the same MacBook Pro, OBS captures a 3024x1964 image.

### The mask must match OBS's capture resolution

If the mask PNG is 1512x982 (logical) but OBS is rendering at 3024x1964 (physical),
the mask won't align. The mask must be generated at the **physical pixel resolution**.

### How to get the scaling factor

**Python (pyobjc):**
```python
from Quartz import CGDisplayScreenSize, CGMainDisplayID
from AppKit import NSScreen

screen = NSScreen.mainScreen()
backing_scale = screen.backingScaleFactor()  # 2.0 for Retina
frame = screen.frame()  # logical points
# Physical pixels = logical points * backing_scale
```

**Swift:**
```swift
let screen = NSScreen.main!
let scale = screen.backingScaleFactor  // 2.0 for Retina
let physicalWidth = Int(screen.frame.width * scale)
let physicalHeight = Int(screen.frame.height * scale)
```

### Coordinate conversion for mask generation

```
mask_x = (window_bounds.x - screen_origin.x) * backing_scale_factor
mask_y = (window_bounds.y - screen_origin.y) * backing_scale_factor
mask_w = window_bounds.width * backing_scale_factor
mask_h = window_bounds.height * backing_scale_factor
```

### Multi-monitor complication

Each monitor can have a different backing scale factor. A Retina laptop (2x) with an
external 4K monitor (may be 2x or 1x depending on resolution setting) requires per-display
scale factor lookup.

CGWindowListCopyWindowInfo returns bounds in a unified global coordinate space where
the primary display's origin is (0,0). Secondary displays have offsets based on arrangement.
Each display's bounds must be converted independently using its own scale factor.

### Recommendation

1. Query `NSScreen.screens()` at startup and on `NSApplicationDidChangeScreenParametersNotification`
2. Store per-screen: logical frame, backing scale factor, physical dimensions
3. When compositing mask, multiply all CG bounds by the target screen's scale factor
4. Generate mask at physical pixel resolution matching OBS display capture

---

## 7. Daemon Lifecycle Management

### Option A: launchd plist (LaunchAgent)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.trillium.pii-mask-daemon</string>
    <key>ProgramArguments</key>
    <array>
        <string>/path/to/pii_mask_daemon</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/tmp/pii-mask-daemon.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/pii-mask-daemon.err</string>
</dict>
</plist>
```

Installed to `~/Library/LaunchAgents/com.trillium.pii-mask-daemon.plist`.

| Aspect | Detail |
|--------|--------|
| **Start** | Automatic at login (`RunAtLoad`) |
| **Restart** | Automatic via `KeepAlive` (restarts on crash) |
| **Stop** | `launchctl bootout gui/$(id -u) com.trillium.pii-mask-daemon` |
| **Permissions** | Inherits user's TCC grants. Screen Recording and Accessibility must be granted to the daemon binary (or its parent -- e.g., Terminal, Python). |
| **Pros** | OS-managed lifecycle. Auto-restart on crash. Survives logout/login. Standard macOS pattern. |
| **Cons** | Must grant Screen Recording to the specific binary. If using Python, must grant to python3 binary (which may be Homebrew, system, or Talon's). Permission changes require re-granting after binary updates. |

**Critical TCC note:** LaunchAgents (user-level) can receive TCC permissions. LaunchDaemons
(system-level, run as root before login) **cannot** present permission dialogs and have
limited TCC access. The daemon **must** be a LaunchAgent, not a LaunchDaemon.

### Option B: Manual start (CLI)

```bash
# Start
/path/to/pii_mask_daemon &

# Or with nohup
nohup /path/to/pii_mask_daemon > /tmp/pii-mask.log 2>&1 &
```

| Aspect | Detail |
|--------|--------|
| **Start** | Manual, or from a shell profile / Talon script |
| **Restart** | Manual. No auto-restart on crash. |
| **Permissions** | Inherits from terminal/shell. |
| **Pros** | Simple. Easy to debug (run in foreground, see stdout). |
| **Cons** | No crash recovery. Easy to forget to start. Not suitable for production. |

### Option C: Talon-managed

Talon starts the daemon as a subprocess from a Talon script:

```python
# In Talon user script
import subprocess
daemon_proc = subprocess.Popen(["/path/to/pii_mask_daemon"])
```

| Aspect | Detail |
|--------|--------|
| **Start** | When Talon loads the script |
| **Restart** | Talon script can monitor and restart |
| **Permissions** | Inherits Talon's TCC grants (Screen Recording + Accessibility already granted to Talon) |
| **Pros** | **Inherits Talon's permissions automatically.** No separate TCC grant needed. Lifecycle tied to Talon -- if Talon is running, daemon is running. |
| **Cons** | If Talon crashes, daemon dies too (but Talon crash = streaming should stop anyway). Talon's subprocess management is basic. Must be careful not to block Talon's main thread during daemon start. |

### Recommendation

**Phase 1 (development):** Talon-managed (Option C). Inherits Talon's TCC permissions,
which solves the Screen Recording grant problem. Simple to iterate.

**Phase 2 (production):** LaunchAgent (Option A) with `KeepAlive`. Grant Screen Recording
to the daemon binary directly. Use Talon shim to send events but don't depend on Talon
for daemon lifecycle.

**Hybrid:** LaunchAgent for the daemon, but if the daemon isn't running when Talon starts,
the Talon shim launches it as a subprocess as a fallback.

---

## 8. Permissions Requirements

### Screen Recording (TCC: kTCCServiceScreenCapture)

**Required for:** `CGWindowListCopyWindowInfo` to return complete window information.

**Without it:** The function still returns results, but:
- `kCGWindowName` (window title) is empty for non-owned windows
- `kCGWindowBounds` may be missing or incorrect for some windows
- Window content capture (CGWindowListCreateImage) returns blank images

**Granted to:** The specific binary that calls the API. If Python, the python3 binary.
If Swift daemon, the compiled binary.

**Talon inheritance:** If the daemon is a child process of Talon, and Talon has Screen
Recording permission, the daemon **inherits** this permission. This is the key advantage
of Talon-managed lifecycle.

### Accessibility (TCC: kTCCServiceAccessibility)

**Required for:** AXUIElement API (AXObserver, reading window titles/attributes via AX).

**NOT required for:** `CGWindowListCopyWindowInfo` (this is CoreGraphics, not Accessibility).

**When needed:** Only if the daemon uses AXObserver for event-driven window tracking
(not recommended as primary approach -- see Section 5). Also needed if the daemon
wants to manipulate windows (move, resize, close) -- not a requirement for pii_mask.

### Automation (TCC: kTCCServiceAppleEvents)

**Required for:** Sending AppleEvents to other apps (e.g., `osascript` to control OBS).

**NOT required for:** Window detection, mask generation, or IPC with Talon.

### Summary by implementation option

| Option | Screen Recording | Accessibility | Automation | Notes |
|--------|-----------------|---------------|------------|-------|
| Python + pyobjc | YES | Only if using AX | No | Grant to python3 binary |
| Python + Swift helper | YES (Swift binary) | Only if using AX | No | Grant to compiled Swift helper |
| Pure Swift daemon | YES | Only if using AX | No | Grant to compiled binary |
| Rust daemon | YES | Only if using AX | No | Grant to compiled binary |
| Talon-managed (any) | Inherited from Talon | Inherited from Talon | Inherited | Simplest permission story |

---

## 9. Shared Memory vs File I/O for Mask Data

### Current approach: PNG file with atomic rename

- Write PNG to temp file, `os.replace()` to target path
- OBS reads file via `mask_filter_v2`
- Latency: 2-4ms for PNG encode + <0.1ms for rename
- Reliability: Atomic. OBS never sees partial writes.

### Alternative: POSIX shared memory (shm_open + mmap)

```
Daemon writes raw mask bytes to shared memory segment.
OBS plugin reads from same shared memory.
No file I/O, no PNG encode/decode.
```

| Dimension | File (PNG) | Shared Memory |
|-----------|-----------|---------------|
| Write latency | 2-4ms (PNG encode) | <0.1ms (memcpy) |
| Read latency | 1-2ms (PNG decode in OBS) | <0.1ms (direct memory read) |
| Total round-trip | 3-6ms | <0.2ms |
| Requires OBS plugin | No (uses existing mask_filter_v2) | **Yes** (custom plugin to read shm) |
| Synchronization | Atomic rename (free) | Need mutex or double-buffer protocol |
| macOS shm limits | N/A | Default 4MB limit (sufficient for 1 mask) |
| Complexity | Low | Medium (OBS plugin + shm protocol) |

**Mask size calculation:**
- 1728 x 1117 x 1 byte (grayscale) = ~1.9MB (logical resolution)
- 3456 x 2234 x 1 byte (Retina 2x) = ~7.7MB (exceeds default 4MB shm limit; can increase via sysctl)

### Verdict

Shared memory is Phase 4 optimization (per ARCHITECTURE.md). PNG file I/O is fast enough
for Phase 1-3 (~5ms total including encode). Only pursue shared memory when/if an OBS
plugin is built to read it. The bottleneck is PNG encode/decode, not filesystem I/O.

---

## 10. Recommendation

### For Phase 1: Python + pyobjc (Option 1)

**Rationale:**
- Fastest path to working daemon (days, not weeks)
- Already proven in ARCHITECTURE.md design
- Talon ecosystem alignment (shared language, debugging tools)
- pyobjc provides complete CGWindowList access with native performance
- Talon-managed lifecycle inherits TCC permissions
- numpy + PIL mask composition meets 16ms frame budget easily

### For Phase 4 (if needed): Pure Swift daemon (Option 3)

**When to consider Swift rewrite:**
- PNG encode/decode becomes the bottleneck and shared memory is needed
- NSWorkspace notifications needed for zero-latency app switch detection
  (avoiding even the Unix socket hop from Talon)
- Memory budget tightens (Python's 50-70MB vs Swift's 5-10MB matters)
- Type safety becomes critical as the rule engine grows complex

### Options to eliminate

- **Python + ctypes (Option 6):** Strictly worse than pyobjc in every dimension. Do not use.
- **Node.js + addon (Option 8):** No advantage over Python, higher memory, worse macOS ecosystem.
- **Go + cgo (Option 5):** Poor macOS API access. Wrong tool for the job.
- **Python + Swift helper (Option 2):** Subprocess overhead defeats the purpose unless long-running,
  at which point just write the whole thing in Swift.

### Options to keep in reserve

- **Rust (Option 4):** If the daemon needs to be embedded in another Rust system later.
- **Objective-C (Option 7):** Only if inheriting an existing ObjC codebase.
