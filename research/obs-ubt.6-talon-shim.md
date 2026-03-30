# OBS-UBT.6: Talon Voice Control Shim for PII Mask Daemon

> Research date: 2026-03-29
> Goal: Evaluate all viable approaches for a minimal Talon shim that forwards window
> events to the PII mask daemon without blocking Talon's main thread.

---

## Table of Contents

1. [Talon API Reference](#1-talon-api-reference)
2. [SIGABRT Root Cause Analysis](#2-sigabrt-root-cause-analysis)
3. [Existing Communication Patterns in trillium_obs](#3-existing-communication-patterns)
4. [Option Comparison Matrix](#4-option-comparison-matrix)
5. [Detailed Option Analysis](#5-detailed-option-analysis)
6. [Recommendation](#6-recommendation)

---

## 1. Talon API Reference

### ui.register() Events and Callback Signatures

From the Talon `.pyi` stubs (`/Applications/Talon.app/.../talon/mac/ui.pyi`):

```python
def register(topic: str, cb: Callable) -> None: ...
def unregister(topic: str, cb: Callable) -> None: ...
```

**Available event topics** (from `Mon` class which dispatches via `win_event`, `app_event`):

| Topic | Callback signature | When it fires |
|-------|-------------------|---------------|
| `"win_focus"` | `cb(window: ui.Window)` | Window gains focus |
| `"win_open"` | `cb(window: ui.Window)` | New window created |
| `"win_close"` | `cb(window: ui.Window)` | Window destroyed |
| `"win_title"` | `cb(window: ui.Window)` | Window title changes |
| `"app_launch"` | `cb(app: ui.App)` | New app launched |
| `"app_close"` | `cb(app: ui.App)` | App quit |
| `"app_activate"` | `cb(app: ui.App)` | App becomes frontmost |
| `"app_deactivate"` | `cb(app: ui.App)` | App loses frontmost |
| `"screen_change"` | `cb(...)` | Display configuration change |

### ui.Window Fields

From the Mac-specific `.pyi` stub (`talon/mac/ui.pyi`, class `Window(BaseWindow)`):

| Field | Type | Description |
|-------|------|-------------|
| `.id` | `int` | Unique window ID (stable for window lifetime) |
| `.title` | `str` | Window title |
| `.doc` | `str` | Document name |
| `.hidden` | `bool` | Whether window is hidden |
| `.enabled` | `bool` | Whether window accepts input |
| `.rect` | `Rect` | Position and size (`.x`, `.y`, `.width`, `.height`) |
| `.screen` | `Screen` | Which display the window is on |
| `.app` | `App` | Owning application |
| `.minimized` | `bool` | Property (get/set) |
| `.maximized` | `bool` | Property (get/set) |
| `.fullscreen` | `bool` | Property (get/set) |
| `.workspace` | `int` | Desktop/space number |
| `.element` | `Element` | Accessibility element (AX tree root) |
| `.children` | `Elements` | AX child elements |

### ui.App Fields

| Field | Type | Description |
|-------|------|-------------|
| `.pid` | `int` | Process ID |
| `.name` | `str` | Display name ("Messages", "Google Chrome") |
| `.bundle` | `str` | Bundle ID ("com.apple.MobileSMS") |
| `.path` | `str` | Application path |
| `.exe` | `str` | Executable path |
| `.background` | `bool` | Whether app is background-only |
| `.created_at` | `datetime` | Launch time |
| `.active_window` | `Window` | Currently focused window |
| `.windows()` | `Sequence[Window]` | All windows for this app |

### Rect Fields

| Field | Type |
|-------|------|
| `.x` | `float` |
| `.y` | `float` |
| `.width` | `float` |
| `.height` | `float` |

### Talon Threading Model

**Key facts from the `.pyi` stubs and observed behavior:**

1. **`ui.register()` callbacks run on Talon's main thread.** The `Mon` class uses
   `threading.Condition` for readiness signaling, but event dispatch is single-threaded.
   All `win_event`, `app_event` callbacks are dispatched sequentially.

2. **`cron.interval()` and `cron.after()` run on a dedicated cron thread** managed by the
   `Cron` class (which has its own `threading.Condition` and `thread()` method). Callbacks
   are invoked from this cron thread, not the main thread.

3. **Python `threading.Thread` works in Talon.** Confirmed by existing code:
   - `most_recent_command_playback/utils/play_flacs.py` uses `threading.Thread(target=..., daemon=True).start()`
   - `trillium/utils/_wav.py` imports `threading`
   - The `cron.pyi` itself imports `threading`

4. **`subprocess.run()` blocks the calling thread.** This is standard Python behavior.
   When called from a `ui.register()` callback, it blocks Talon's main event loop.

5. **`asyncio` is NOT used by Talon internally.** The cron system is thread-based, not
   asyncio-based. However, nothing prevents user scripts from creating their own asyncio
   event loops on background threads.

6. **Talon provides no built-in non-blocking I/O API.** There is no `talon.socket` or
   `talon.async_send`. You must use stdlib `socket`, `threading`, or `asyncio` yourself.

### cron API

```python
cron.after("100ms", callback)     # One-shot timer, runs callback once
cron.interval("33ms", callback)   # Repeating timer (returns Job for cancellation)
cron.cancel(job)                  # Cancel a job
```

The cron callbacks run on a cron thread, not the main thread. This means cron-based
polling does NOT block Talon's event dispatch. However, cron callbacks should still be
fast -- a slow callback delays subsequent cron jobs.

---

## 2. SIGABRT Root Cause Analysis

### The Problem

The existing code in `app_switcher.py` (line 70, 74) has `draw_rect_over_image_in_place()`
calls **disabled** with the comment:

```python
# DISABLED: Blocking ffmpeg call causing SIGABRT
# actions.user.draw_rect_over_image_in_place(color, rect)
```

### Root Cause

`draw_rect_over_image_in_place()` calls `subprocess.run(cmd, check=True)` which:

1. **Forks the process** via `posix_spawn` or `fork+exec`
2. **Blocks the calling thread** until ffmpeg completes (~20-100ms)

This is called from `switcher_focus()`, which is invoked as a Talon action. Talon actions
run on the main thread. The blocking `subprocess.run()` stalls Talon's event loop.

**The SIGABRT specifically occurs because:**

- macOS's Accessibility framework (which Talon uses for `ui.Window`, `ui.App`, etc.)
  has internal timeouts and thread-safety constraints
- When Talon's main thread is blocked in `subprocess.run()`, pending Accessibility
  callbacks queue up and eventually the framework sends SIGABRT
- Additionally, `fork()` in a multi-threaded process with active Objective-C runtime
  and Accessibility connections is inherently dangerous on macOS -- the forked child
  inherits locked mutexes from threads that don't exist in the child, causing deadlocks
  that trigger `SIGABRT`

### How to Avoid SIGABRT

1. **Never call `subprocess.run()` from a ui.register callback or Talon action** that
   runs on the main thread
2. **Never call `fork()`/`subprocess` at all** if possible -- prefer `socket.send()`,
   file writes, or other non-forking IPC
3. **If you must do slow work, use `threading.Thread(daemon=True)`** to offload it,
   or `cron.after()` to defer it
4. **Keep ui.register callbacks under ~1ms** -- do the absolute minimum, then hand off

### What the Shim Must Do Differently

The shim replaces `subprocess.run(ffmpeg ...)` with a lightweight message send. The
message goes to an external daemon (separate process, already running), which handles
all heavy lifting. The shim callback must complete in **microseconds**, not milliseconds.

---

## 3. Existing Communication Patterns in trillium_obs

### Pattern 1: Synchronous HTTP POST (urllib)

**Used by:** `obs_scene_change.py` -> `urllib_request.py` -> OBS Listener

```python
# In ui.register callback chain:
actions.user.obs_get_blurry()
  -> send_obs_request(data, method)
    -> send_urllib_request(url, data, method)
      -> urllib.request.urlopen(req)  # BLOCKS until HTTP response
```

**Latency:** ~5-50ms (localhost HTTP roundtrip)
**Problem:** Blocks Talon's main thread. Works because the OBS listener responds fast
(~5ms), but still risky. Any network hiccup or OBS listener delay causes the same
class of issues that led to SIGABRT with ffmpeg.

### Pattern 2: Synchronous HTTP POST (Home Assistant)

**Used by:** `trillium_hass/hass_api.py`

Same pattern as OBS -- `urllib.request.urlopen()` blocking on the calling thread. Works
because the HASS server is on LAN and responds quickly.

### Pattern 3: File-based IPC (Talon Deck)

**Used by:** `trillium_talon_deck/talon_deck_integrations.py`

```python
# Write state to JSON file
state_tmp_path.write_text(json.dumps(state))
os.replace(state_tmp_path, state_path)

# Poll for requests via cron
cron.interval("100ms", poll_requests)
```

Writes JSON to temp dir, uses `os.replace()` for atomicity. External process reads the
files. Polling-based, not event-driven.

### Pattern 4: Background Thread for Audio

**Used by:** `most_recent_command_playback/utils/play_flacs.py`

```python
threading.Thread(target=_play_flacs_thread, args=(...), daemon=True).start()
```

Daemon thread runs `subprocess.Popen()` in background. Confirmed: **threading.Thread
works in Talon and does not cause SIGABRT** as long as you don't block the main thread.

### Pattern 5: Passive Key Interception

**Used by:** `keyboard_blurr_on_window_change.talon`

```talon
key(cmd-tab:passive):
    user.obs_get_blurry()
```

Intercepts cmd-tab keystroke and synchronously calls HTTP POST to blur the stream.
This fires BEFORE the window actually switches, providing a safety net. But it blocks
on the HTTP call.

---

## 4. Option Comparison Matrix

| # | Approach | Ease of Writing | Ease of Maint. | Type Safety | Perf/Latency | Reliability | Rec? |
|---|----------|:-:|:-:|:-:|:-:|:-:|:-:|
| 1 | Sync Unix socket send | 5 | 4 | 3 | 3 | 3 | No |
| 2 | Async Unix socket + bg thread | 3 | 3 | 3 | 5 | 5 | **YES** |
| 3 | UDP datagram | 5 | 4 | 3 | 5 | 2 | Maybe |
| 4 | Talon cron polling | 4 | 5 | 4 | 1 | 4 | No |
| 5 | Named pipe (FIFO) | 3 | 3 | 3 | 4 | 3 | No |
| 6 | Shared memory direct write | 2 | 2 | 2 | 5 | 3 | No |
| 7 | HTTP POST to daemon | 4 | 4 | 3 | 2 | 3 | No |
| 8 | File-based signaling | 4 | 4 | 3 | 1 | 3 | No |
| 9 | No shim (daemon-only CGWindowList) | 5 | 5 | 4 | 2 | 4 | Partial |

---

## 5. Detailed Option Analysis

### Option 1: Synchronous Unix Socket Send

**How it works:** On each `ui.register` callback, open (or reuse) a Unix domain socket
connection and send a JSON message. Block until the kernel accepts the write.

```python
import socket, json

SOCK_PATH = "/tmp/pii_mask.sock"
_sock = None

def _get_sock():
    global _sock
    if _sock is None:
        _sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        _sock.connect(SOCK_PATH)
    return _sock

def on_win_focus(window):
    msg = json.dumps({"type": "focus", "app": window.app.name, "pid": window.app.pid})
    try:
        _get_sock().sendall(msg.encode() + b"\n")
    except:
        _sock = None  # reconnect next time
```

| Dimension | Score | Notes |
|-----------|:-----:|-------|
| Ease of writing | 5 | ~15 lines, stdlib only |
| Ease of maintenance | 4 | Simple, but reconnection logic needed |
| Type safety | 3 | JSON strings, no schema enforcement |
| Performance/latency | 3 | ~0.05-0.1ms per send (kernel buffer), but `connect()` on first call is ~0.5ms. Blocks main thread briefly. |
| Reliability | 3 | If daemon is down, `connect()` blocks/fails. If socket buffer is full, `sendall()` blocks. |

**Pros:**
- Simplest possible implementation
- Sub-millisecond for buffered writes (kernel copies to socket buffer immediately)
- No threading complexity
- Connection reuse amortizes setup cost

**Cons:**
- **Blocks Talon's main thread** on every send, even if briefly (~0.05ms)
- If daemon is slow to read, socket buffer fills and `sendall()` blocks indefinitely
- `connect()` failure path can block for the TCP timeout (even on Unix sockets, connect
  to a non-listening socket returns ECONNREFUSED immediately, but a stale socket file
  can cause brief hangs)
- Reconnection after daemon restart requires error handling on every send

**Key reference:** Python `socket` stdlib, `AF_UNIX` + `SOCK_STREAM`

---

### Option 2: Async Unix Socket with Background Thread (RECOMMENDED)

**How it works:** A daemon thread owns a Unix domain socket connection and reads from a
`queue.Queue`. The `ui.register` callbacks put messages on the queue (non-blocking,
bounded) and return immediately. The background thread drains the queue and sends.

```python
import socket, json, queue, threading

SOCK_PATH = "/tmp/pii_mask.sock"
_queue = queue.Queue(maxsize=64)

def _sender_thread():
    sock = None
    while True:
        msg = _queue.get()  # blocks until item available
        try:
            if sock is None:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(SOCK_PATH)
            sock.sendall(msg.encode() + b"\n")
        except Exception:
            sock = None  # will reconnect on next message

threading.Thread(target=_sender_thread, daemon=True).start()

def on_win_focus(window):
    msg = json.dumps({
        "type": "focus",
        "app": window.app.name,
        "pid": window.app.pid,
        "window_id": window.id,
        "title": window.title,
        "rect": {"x": window.rect.x, "y": window.rect.y,
                 "w": window.rect.width, "h": window.rect.height},
    })
    try:
        _queue.put_nowait(msg)
    except queue.Full:
        pass  # drop oldest or newest -- daemon will catch up via CG poller

ui.register("win_focus", on_win_focus)
ui.register("win_open", on_win_open)
ui.register("win_close", on_win_close)
ui.register("app_activate", on_app_activate)
```

| Dimension | Score | Notes |
|-----------|:-----:|-------|
| Ease of writing | 3 | ~40 lines, needs threading + queue |
| Ease of maintenance | 3 | Thread lifecycle, reconnection, queue overflow policy |
| Type safety | 3 | JSON strings |
| Performance/latency | 5 | `queue.put_nowait()` is ~1 microsecond. **Zero main-thread blocking.** |
| Reliability | 5 | Queue decouples sender from receiver. Daemon restart = seamless reconnect. Queue overflow = graceful drop (CG poller catches up). |

**Pros:**
- **Absolutely never blocks Talon's main thread** -- `queue.put_nowait()` is O(1)
- Automatic reconnection on daemon restart
- Queue absorbs bursts (rapid cmd-tab)
- Daemon thread handles all socket I/O, retries, reconnection
- Confirmed: `threading.Thread(daemon=True)` works in Talon (proven by play_flacs.py)
- Graceful degradation: if queue is full, message is dropped, CG poller provides backup

**Cons:**
- More code than Option 1 (~40 lines vs ~15)
- Thread lifecycle management (daemon=True handles cleanup on exit)
- Tiny added latency from queue transit (~microseconds, negligible)
- Need to decide queue overflow policy (drop newest vs drop oldest)

**Key reference:** Python `queue.Queue`, `threading.Thread`, `socket.AF_UNIX`

---

### Option 3: UDP Datagram

**How it works:** Send a UDP datagram for each event. No connection, no handshake,
fire-and-forget.

```python
import socket, json

_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
DAEMON_ADDR = "/tmp/pii_mask_dgram.sock"

def on_win_focus(window):
    msg = json.dumps({"type": "focus", "app": window.app.name})
    try:
        _sock.sendto(msg.encode(), DAEMON_ADDR)
    except:
        pass
```

| Dimension | Score | Notes |
|-----------|:-----:|-------|
| Ease of writing | 5 | ~10 lines, even simpler than TCP |
| Ease of maintenance | 4 | No connection state, no reconnection logic |
| Type safety | 3 | JSON strings |
| Performance/latency | 5 | `sendto()` copies to kernel buffer, returns immediately (~0.01ms) |
| Reliability | 2 | **No delivery guarantee.** Kernel can silently drop datagrams if receiver buffer is full. No way to know if daemon received the message. No backpressure. |

**Pros:**
- Simplest code of all socket options
- No connection setup, no reconnection logic
- Fire-and-forget semantics match the "fast path with CG poller backup" architecture
- `sendto()` is effectively non-blocking for small messages (fits in one kernel buffer)

**Cons:**
- **Silent message loss** -- kernel drops datagrams when receiver buffer is full
- No feedback if daemon is down (sends succeed to nowhere if socket file exists but
  nobody is listening -- actually `ECONNREFUSED` for Unix DGRAM, but the message is
  still lost)
- Max message size limited by `SO_SNDBUF` (default ~128KB, more than enough for JSON)
- Debugging harder -- no connection state to inspect
- Unix DGRAM sockets require the daemon to `bind()` first; if daemon restarts, the
  socket file must be recreated (race condition with shim)

**Key reference:** Python `socket.AF_UNIX` + `SOCK_DGRAM`

---

### Option 4: Talon Cron Job Polling

**How it works:** Instead of event-driven callbacks, use `cron.interval()` to poll
`ui.active_window()` on a timer and send state when it changes.

```python
from talon import cron, ui

_last_app = None
_last_window_id = None

def poll_active_window():
    global _last_app, _last_window_id
    w = ui.active_window()
    if w.id != _last_window_id or w.app.name != _last_app:
        _last_app = w.app.name
        _last_window_id = w.id
        send_to_daemon({"type": "focus", "app": w.app.name, ...})

cron.interval("33ms", poll_active_window)  # 30Hz
```

| Dimension | Score | Notes |
|-----------|:-----:|-------|
| Ease of writing | 4 | Simple, uses Talon's built-in cron |
| Ease of maintenance | 5 | No threading, no sockets in callback, trivial logic |
| Type safety | 4 | Direct access to typed ui.Window objects |
| Performance/latency | 1 | **33ms worst-case latency** (one full poll interval). Average 16.5ms. Plus the CG poller in the daemon already provides 30Hz polling. This is redundant. |
| Reliability | 4 | No message loss, no connection issues. But misses transient events (window opens and closes within one poll interval). |

**Pros:**
- No threading, no socket management in the callback
- Cron runs on its own thread, doesn't block main Talon thread
- Guaranteed to eventually detect any change
- Cannot crash or SIGABRT -- just reads `ui.active_window()`
- Trivial to implement and debug

**Cons:**
- **Redundant with the daemon's CG poller** which already polls at 30Hz
- 33ms worst-case latency is SLOWER than the CG poller (which has the same latency
  but gets more data -- all windows, z-order, layers)
- Misses rapid window open/close events that happen within one poll cycle
- Doesn't detect `win_open`, `win_close`, `win_title` -- only focus changes
- Wastes CPU calling `ui.active_window()` 30 times/sec even when nothing changes
- The whole point of the shim is sub-millisecond event delivery; polling defeats this

**Key reference:** `talon.cron.interval()`

---

### Option 5: Named Pipe (FIFO) Write

**How it works:** Create a FIFO at a known path. Daemon opens it for reading. Shim
opens it for writing and writes JSON on each event.

```python
import os, json

FIFO_PATH = "/tmp/pii_mask.fifo"

def on_win_focus(window):
    msg = json.dumps({"type": "focus", "app": window.app.name}) + "\n"
    fd = os.open(FIFO_PATH, os.O_WRONLY | os.O_NONBLOCK)
    os.write(fd, msg.encode())
    os.close(fd)
```

| Dimension | Score | Notes |
|-----------|:-----:|-------|
| Ease of writing | 3 | Must handle FIFO lifecycle, O_NONBLOCK, EAGAIN |
| Ease of maintenance | 3 | FIFO has tricky edge cases (no reader = ENXIO, buffer full = EAGAIN) |
| Type safety | 3 | JSON strings |
| Performance/latency | 4 | `write()` to FIFO with O_NONBLOCK is fast (~0.01ms) when buffer has space |
| Reliability | 3 | If daemon hasn't opened FIFO yet, `open(O_WRONLY)` fails with ENXIO. If pipe buffer is full (64KB default on macOS), writes fail with EAGAIN. No automatic reconnection semantics. |

**Pros:**
- Simple unidirectional channel
- `O_NONBLOCK` prevents main thread blocking
- Standard POSIX, well-understood semantics
- No connection/reconnection logic (unlike TCP sockets)
- Filesystem-visible for debugging (`cat /tmp/pii_mask.fifo`)

**Cons:**
- **FIFO open/close on every event** is wasteful (or keep fd open, but then must handle
  broken pipe when daemon restarts)
- **ENXIO if no reader** -- must handle daemon-not-running case gracefully
- **EAGAIN if buffer full** -- messages lost under burst load
- Unidirectional only -- no acknowledgment path
- FIFO cleanup on crash is messy (stale file remains)
- More edge cases than Unix sockets for the same functionality

**Key reference:** `os.open()` with `O_WRONLY | O_NONBLOCK`, `mkfifo()`

---

### Option 6: Shared Memory Direct Write

**How it works:** The Talon shim writes window event data directly to the POSIX shared
memory segment that the OBS plugin reads. Skips the daemon entirely for the "fast path."

```python
import mmap, struct, os

fd = os.open("/dev/shm/pii_mask_regions", os.O_RDWR)  # or shm_open equivalent
shm = mmap.mmap(fd, 4096, access=mmap.ACCESS_WRITE)

def on_win_focus(window):
    # Write directly to shared memory
    # Must match the struct layout the OBS plugin expects
    ...
```

| Dimension | Score | Notes |
|-----------|:-----:|-------|
| Ease of writing | 2 | Must manage struct layout, atomic counters, memory mapping. macOS doesn't have `/dev/shm` -- need `shm_open` via ctypes or pyobjc. |
| Ease of maintenance | 2 | Binary format coupling between Talon (Python) and OBS plugin (C). Any struct change requires coordinated updates. |
| Type safety | 2 | Raw bytes, manual struct packing, easy to corrupt |
| Performance/latency | 5 | Zero-copy, sub-microsecond. Fastest possible IPC. |
| Reliability | 3 | No delivery notification. Race conditions between Talon writing and OBS reading unless sequence counter is implemented correctly. Talon can only write focus events -- still needs daemon for CG poller, classification, compositing. |

**Pros:**
- Absolute minimum latency (memory write, no kernel transition for data transfer)
- No daemon in the critical path for simple focus changes
- OBS reads on next `video_tick()` -- 0-16.7ms to screen

**Cons:**
- **Does not eliminate the daemon** -- still need it for CG polling, classification,
  multi-window compositing, notification detection, config management
- Talon shim becomes coupled to the OBS plugin's binary struct layout
- macOS shared memory via Python requires ctypes/cffi for `shm_open()`
  (no `/dev/shm` on macOS)
- Talon would need to replicate window classification logic (safe/unsafe) to know
  what to write
- Two writers (Talon shim + daemon) to the same shared memory = complex synchronization
- Significantly harder to debug than message-based IPC
- Premature optimization -- the daemon adds ~0.5ms, well within the 16ms frame budget

**Key reference:** `shm_open(3)`, `mmap.mmap()`, POSIX shared memory

---

### Option 7: HTTP POST to Daemon

**How it works:** Daemon runs a small HTTP server (e.g., `http.server` or `aiohttp`).
Shim sends HTTP POST with JSON body on each event.

```python
import urllib.request, json

def on_win_focus(window):
    data = json.dumps({"type": "focus", "app": window.app.name}).encode()
    req = urllib.request.Request("http://127.0.0.1:9876/event",
                                 data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req)
```

| Dimension | Score | Notes |
|-----------|:-----:|-------|
| Ease of writing | 4 | Reuses existing `urllib_request.py` pattern from trillium_obs |
| Ease of maintenance | 4 | Well-understood HTTP semantics, easy to debug with curl |
| Type safety | 3 | JSON over HTTP |
| Performance/latency | 2 | **5-20ms per request** (TCP handshake + HTTP overhead + response wait). Blocks main thread. Even with keep-alive, HTTP framing adds overhead. |
| Reliability | 3 | Connection refused if daemon is down. Timeouts if daemon is slow. Same blocking issues as current OBS listener pattern. |

**Pros:**
- Familiar pattern -- already used for OBS listener communication
- Easy to test with `curl`
- Can add endpoints for config, status, health checks
- Works across network boundaries (not just local)

**Cons:**
- **Blocks Talon's main thread** for 5-20ms per event (TCP + HTTP overhead)
- This is EXACTLY the pattern that's already problematic in `obs_scene_change.py`
- HTTP is massive overhead for a 100-byte JSON message on localhost
- TCP connection per request (or keep-alive complexity)
- urllib doesn't support connection pooling well
- Would need background thread to avoid blocking, at which point use a Unix socket instead

**Key reference:** `urllib.request`, `http.server`

---

### Option 8: File-Based Signaling

**How it works:** Shim writes event JSON to a file. Daemon watches the directory with
`fsevents` (macOS) or polls with `os.stat()`.

```python
import json, os, tempfile, time

EVENT_DIR = "/tmp/pii_mask_events"

def on_win_focus(window):
    event = {"type": "focus", "app": window.app.name, "ts": time.time()}
    path = os.path.join(EVENT_DIR, f"{time.monotonic_ns()}.json")
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(event, f)
    os.replace(tmp, path)
```

| Dimension | Score | Notes |
|-----------|:-----:|-------|
| Ease of writing | 4 | Simple file writes, atomic rename |
| Ease of maintenance | 4 | Easy to inspect events on disk, simple cleanup |
| Type safety | 3 | JSON files |
| Performance/latency | 1 | **File I/O is 0.5-5ms** (SSD write + fsync). fsevents delivery has ~100-500ms latency. Even with polling at 30Hz, minimum 33ms latency. |
| Reliability | 3 | Atomic rename prevents partial reads. But file cleanup is needed. Disk full = failure. fsevents can coalesce events. |

**Pros:**
- Extremely debuggable -- events are JSON files on disk
- No network/socket setup
- Survives daemon restarts (events persist on disk)
- Atomic rename prevents partial reads
- Talon Deck already uses this pattern successfully

**Cons:**
- **Slowest option** -- file I/O + fsevents latency = 100-500ms typical
- Even with polling, 33ms minimum latency
- File cleanup required (events accumulate on disk)
- Disk I/O on every event (SSD wear, though minimal)
- Not suitable for the "sub-millisecond event delivery" goal
- Talon Deck uses this for non-latency-sensitive state sync -- different use case

**Key reference:** `os.replace()`, macOS FSEvents

---

### Option 9: No Shim (Daemon-Only CGWindowList Polling)

**How it works:** Don't use Talon at all for event delivery. The daemon polls
`CGWindowListCopyWindowInfo` at 30Hz and detects all window changes itself.

```python
# In the daemon (not Talon):
import time
from Quartz import CGWindowListCopyWindowInfo, kCGWindowListOptionOnScreenOnly, kCGNullWindowID

while True:
    windows = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID)
    # Compare to previous state, detect changes, update mask
    time.sleep(1/30)
```

| Dimension | Score | Notes |
|-----------|:-----:|-------|
| Ease of writing | 5 | No shim code at all. Daemon already has this for the CG poller. |
| Ease of maintenance | 5 | Zero Talon-side code to maintain. Daemon is self-contained. |
| Type safety | 4 | Native CGWindowList returns typed dictionaries |
| Performance/latency | 2 | **33ms worst-case** for 30Hz polling. Cannot beat event-driven delivery. |
| Reliability | 4 | Very reliable -- CGWindowList always works, no IPC failure modes. But misses rapid changes within one poll interval. |

**Pros:**
- **Zero Talon code** -- no shim, no thread, no socket, nothing to break
- Daemon is fully self-contained and independently testable
- CGWindowList provides MORE data than Talon events (z-order, layers, all windows)
- No IPC failure modes (daemon talks directly to macOS)
- No permissions beyond what the daemon already needs (Screen Recording)
- If Talon crashes, mask system continues working

**Cons:**
- **33ms worst-case latency** is too slow for the "mask before OBS renders next frame"
  requirement at 60fps (16.7ms frame budget)
- Cannot detect events faster than the poll rate
- Misses transient windows that open and close within one poll cycle
- At 60Hz polling, CPU cost doubles (~6-9% of one core at 30Hz, ~12-18% at 60Hz)
- Loses the "fast path" optimization from the architecture doc (event -> 5ms mask)

**Key reference:** `CGWindowListCopyWindowInfo`, `pyobjc-framework-Quartz`

**Verdict:** This should be the FALLBACK, not the primary path. The daemon should
always have the CG poller running as a catch-all. The Talon shim provides the fast
path (sub-millisecond event delivery) that closes the latency gap from 33ms to <1ms.
The system works without the shim, just with higher latency.

---

## 6. Recommendation

### Primary: Option 2 (Async Unix Socket + Background Thread)

**Rationale:**

1. **Never blocks Talon's main thread** -- the only operation on the main thread is
   `queue.put_nowait()`, which is O(1) and takes ~1 microsecond
2. **Proven pattern** -- `threading.Thread(daemon=True)` is already used successfully
   in the Talon codebase (play_flacs.py)
3. **Graceful degradation** -- if daemon is down, messages queue up and the background
   thread retries connection. If queue overflows, messages are dropped and the CG poller
   catches up.
4. **Clean separation** -- the shim is ~40 lines, all IPC complexity is in the background
   thread, callbacks are trivial
5. **Matches the architecture doc** -- the daemon receives events via Unix socket, exactly
   as specified in ARCHITECTURE.md

### Fallback: Option 9 (No Shim) is Already Built In

The CG poller in the daemon provides complete functionality without the shim. The shim
is a latency optimization, not a correctness requirement. Ship Option 9 first (it's
free -- the daemon needs the CG poller anyway), then add Option 2 for faster response.

### What NOT to Do

- **Option 1 (sync socket)** -- blocks main thread, same class of risk as the current
  urllib calls
- **Option 4 (cron polling)** -- redundant with the daemon's CG poller, adds no value
- **Option 6 (shared memory)** -- premature optimization, couples Talon to OBS plugin
  binary format, doesn't eliminate the daemon
- **Option 7 (HTTP POST)** -- repeats the existing problematic pattern from obs_scene_change.py
- **Option 8 (file-based)** -- too slow for sub-frame latency

### Consider: Option 3 (UDP) as a Simpler Alternative

If the background thread complexity of Option 2 feels like overkill for a ~30-line shim,
Option 3 (UDP datagram) is worth considering. `sendto()` on a Unix DGRAM socket is
effectively non-blocking for small messages and takes ~10 microseconds. The trade-off is
silent message loss under load, but the CG poller catches anything dropped. The risk is
low for this use case (window events are infrequent -- a few per second at most).

**Decision point:** If message loss is acceptable (CG poller is the safety net), use
Option 3 for simplicity. If guaranteed delivery matters, use Option 2.

### Proposed Shim Implementation (Option 2)

```
talon_shim/
  pii_mask_shim.py    # ~40 lines: register events, queue messages, bg thread sends
```

**Events to forward:**
- `win_focus` -> `{type: "focus", app, pid, window_id, title, rect}`
- `win_open` -> `{type: "open", app, pid, window_id, title, rect}`
- `win_close` -> `{type: "close", window_id}`
- `app_activate` -> `{type: "activate", app, pid}`

**Protocol:** Newline-delimited JSON over Unix domain socket (`SOCK_STREAM`).

**Socket path:** `/tmp/pii_mask.sock` (or configurable via environment variable).

---

## Sources

- [Talon 0.4.0 documentation](https://talonvoice.com/docs/)
- [Talon Community Wiki -- Tips and Tricks](https://talon.wiki/Customization/misc-tips/)
- [Talon Community Wiki -- Framework Overview](https://talon.wiki/Customization/Talon%20Framework/talon-framework-overview/)
- [Python asyncio development docs](https://docs.python.org/3/library/asyncio-dev.html)
- [Talonvoice changelog](https://talonvoice.com/dl/latest/changelog.html)
- Talon `.pyi` stubs: `/Applications/Talon.app/Contents/Resources/python/lib/python3.13/site-packages/talon/`
  - `ui.pyi` -- BaseWindow, BaseApp, register()
  - `mac/ui.pyi` -- Mac-specific Window, App, Mon, Element classes
  - `cron.pyi` -- Cron, Job, interval(), after()
- Existing codebase:
  - `~/.talon/user/trillium_obs/` -- current OBS integration (HTTP-based)
  - `~/.talon/user/trillium/window_history/window_history.py` -- ui.register() pattern
  - `~/.talon/user/talon-axkit/notification.py` -- NotificationMonitor with win_open/win_close
  - `~/.talon/user/most_recent_command_playback/utils/play_flacs.py` -- threading.Thread in Talon
  - `~/.talon/user/trillium_talon_deck/talon_deck_integrations.py` -- file-based IPC + cron polling
