# Talon Shim — Event Forwarder

A minimal Talon user script that forwards window events to the mask daemon
over a Unix domain socket. Never blocks Talon's main thread.

## Interface

- **Input:** Talon `ui.register()` events (win_focus, win_open, win_close, app_activate)
- **Output:** JSON messages over Unix domain socket to daemon
- **Fallback:** If daemon unreachable, triggers `obs_get_blurry()` as safety net

## Message Format

```json
{"type": "focus", "app": "Messages", "bundle": "com.apple.MobileSMS", "pid": 1234, "rect": [100, 200, 800, 600]}
{"type": "open", "app": "Spotlight", "bundle": "com.apple.Spotlight", "pid": 5678, "rect": [400, 100, 600, 400]}
{"type": "close", "app": "Spotlight", "bundle": "com.apple.Spotlight", "pid": 5678}
```

## Implementation

Only Python is viable (Talon's runtime is Python).

| Directory | Language | Status |
|-----------|----------|--------|
| `shim_python/` | Python | — |
