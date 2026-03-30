# Daemon — Window Detection

A background process that monitors macOS windows, classifies them as safe or unsafe,
and writes mask geometry to shared memory for the OBS plugin to read.

## Interface

- **Output:** Shared memory at `/pii_mask` (see `protocol/`)
- **Input (events):** Unix domain socket from Talon shim (optional)
- **Input (polling):** `CGWindowListCopyWindowInfo` at 10-30Hz adaptive

## Behavior

1. On startup, write full-mask state to shm (fail-safe)
2. Poll `CGWindowListCopyWindowInfo` for all on-screen windows
3. Receive low-latency events from Talon shim for focus changes
4. For each window, classify as safe/unsafe via classifier
5. Write updated rect array to shm via seqlock protocol
6. Mask overlays (layer > 0) unconditionally

## Implementations

| Directory | Language | Status |
|-----------|----------|--------|
| `daemon_python/` | Python + pyobjc | — |
| `daemon_swift/` | Swift (native CoreGraphics) | — |
| `daemon_rust/` | Rust + core-graphics crate | — |
