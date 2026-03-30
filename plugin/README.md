# Plugin — OBS Video Filter

An OBS Studio video filter plugin that reads mask geometry from shared memory and
applies blur/blackout to unsafe regions every frame.

## Interface

- **Input:** Shared memory at `/pii_mask` (see `protocol/`)
- **Output:** Modified video frames in the OBS render pipeline
- **Hooks:** `video_tick()` reads shm, `video_render()` applies mask
- **Fail-safe:** If shm is missing or stale, apply full-screen blur

## Behavior

1. Every frame, read rect array from shared memory via seqlock
2. If `FLAG_FULL_MASK` is set or data is stale: blur entire frame
3. Otherwise: for each rect with `RECT_FLAG_UNSAFE`, blur that region
4. Blur uses rounded rectangle shape matching `corner_radius`

## Implementations

| Directory | Language | Status |
|-----------|----------|--------|
| `plugin_c/` | C | — |
| `plugin_cpp/` | C++ | — |
| `plugin_rust/` | Rust (obs-rs) | — |
