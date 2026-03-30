# Protocol

The shared memory layout that connects the daemon to the OBS plugin. This is the
contract — both sides must agree on the exact byte layout.

## Interface

- **Transport:** POSIX shared memory (`/pii_mask` via `shm_open`)
- **Direction:** Daemon writes, plugin reads
- **Synchronization:** Seqlock (atomic sequence counter)
- **Total size:** 808 bytes (header + 32 rects)

## Shared Memory Layout

```
Offset  Size   Field
─────────────────────────────────
Header (40 bytes):
0x00    4      magic (0x50494D53 = "PIMS")
0x04    4      version (1)
0x08    4      sequence counter (atomic, odd = writing, even = ready)
0x0C    4      rect_count (0-32)
0x10    8      timestamp_ns (unix nanoseconds of last update)
0x18    4      flags (bit 0: daemon alive, bit 1: full mask override)
0x1C    4      reserved

Rect Array (32 entries x 24 bytes = 768 bytes):
0x28    4      x (float, logical points)
0x2C    4      y (float, logical points)
0x30    4      width (float)
0x34    4      height (float)
0x38    4      corner_radius (float, 0 = sharp corners)
0x3C    4      flags (bit 0: 0=reveal/safe, 1=mask/unsafe)
... repeats 32 times
```

## Seqlock Protocol

**Writer (daemon):**
1. Increment sequence to odd (signals "writing")
2. Write rect data
3. Memory barrier
4. Increment sequence to even (signals "ready")

**Reader (plugin, every frame in video_tick):**
1. Read sequence → if odd, skip (writer is mid-update, use last good data)
2. Memory barrier
3. Read rect data
4. Memory barrier
5. Read sequence again → if changed, data was torn, discard and retry

## Staleness Detection

Plugin checks `timestamp_ns` every frame. If older than 5 seconds, the daemon is
presumed dead and the plugin falls back to full-mask mode.

## Implementations

Each implementation must produce/consume this exact byte layout:

| Implementation | Role | Notes |
|---|---|---|
| `pii_mask_protocol_c.h` | Plugin reads | C struct + seqlock macros |
| `pii_mask_protocol_python.py` | Daemon writes | ctypes struct + mmap |
| `pii_mask_protocol_swift.swift` | Daemon writes (alt) | UnsafeRawPointer |
| `pii_mask_protocol_rust.rs` | Either side | #[repr(C)] struct |
