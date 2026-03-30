# IPC Bridge Research: Python Daemon to OBS C Plugin on macOS

> Research date: 2026-03-29
> Context: PII mask system where a Python daemon detects unsafe window regions and an OBS C filter plugin reads mask geometry every frame in `video_tick()` to apply blur.

---

## Table of Contents

1. [Data Layout](#1-data-layout)
2. [IPC Transport Options (Comparison Matrix)](#2-ipc-transport-options)
3. [Serialization Options (Comparison Matrix)](#3-serialization-options)
4. [Lock-Free Synchronization](#4-lock-free-synchronization)
5. [Crash Detection and Staleness](#5-crash-detection-and-staleness)
6. [macOS Sandboxing Implications](#6-macos-sandboxing-implications)
7. [Maximum Practical Update Rates](#7-maximum-practical-update-rates)
8. [Recommendation](#8-recommendation)

---

## 1. Data Layout

### Per-rect fields

| Field | Type | Bytes | Notes |
|-------|------|-------|-------|
| `x` | `float` | 4 | Left edge, pixels |
| `y` | `float` | 4 | Top edge, pixels |
| `width` | `float` | 4 | Rect width, pixels |
| `height` | `float` | 4 | Rect height, pixels |
| `corner_radius` | `float` | 4 | Uniform radius (all 4 corners) |
| `flags` | `uint32_t` | 4 | Bit 0: blur vs blackout. Reserved bits for future use. |

**Total per rect: 24 bytes** (6 floats/uint32, naturally aligned)

If per-corner radii are needed later:

| Field | Type | Bytes |
|-------|------|-------|
| `corner_radius_tl` | `float` | 4 |
| `corner_radius_tr` | `float` | 4 |
| `corner_radius_bl` | `float` | 4 |
| `corner_radius_br` | `float` | 4 |

That would be 36 bytes per rect. Start with uniform radius (24 bytes) for simplicity.

### Header fields

| Field | Type | Bytes | Notes |
|-------|------|-------|-------|
| `magic` | `uint32_t` | 4 | `0x50494D53` ("PIMS") — validates shm is initialized |
| `version` | `uint32_t` | 4 | Protocol version (start at 1) |
| `sequence` | `uint64_t` | 8 | Atomic seqlock counter (even = stable, odd = writing) |
| `timestamp_ns` | `uint64_t` | 8 | Writer's `clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)` |
| `frame_width` | `uint32_t` | 4 | Source resolution width |
| `frame_height` | `uint32_t` | 4 | Source resolution height |
| `num_regions` | `uint32_t` | 4 | Number of active rects (0..MAX) |
| `_padding` | `uint32_t` | 4 | Align to 8-byte boundary |

**Header: 40 bytes**

### Limits and total size

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| `MAX_MASK_REGIONS` | 32 | Typical desktop: 5-15 windows. 32 handles pathological cases. |
| Per-region size | 24 bytes | 6 x 4-byte fields |
| Region array | 768 bytes | 32 x 24 |
| Header | 40 bytes | See above |
| **Total shm size** | **808 bytes** | Fits in a single cache line set. Tiny. |

Even with per-corner radii (36 bytes/rect): 40 + 32*36 = **1,192 bytes**. Still trivial.

### C struct definition

```c
#include <stdint.h>
#include <stdatomic.h>

#define PII_MASK_MAGIC    0x50494D53
#define PII_MASK_VERSION  1
#define MAX_MASK_REGIONS  32

struct pii_mask_region {
    float x;
    float y;
    float width;
    float height;
    float corner_radius;
    uint32_t flags;
};

struct pii_mask_shm {
    uint32_t magic;
    uint32_t version;
    _Atomic uint64_t sequence;      // seqlock counter
    uint64_t timestamp_ns;          // monotonic clock, nanoseconds
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t num_regions;
    uint32_t _padding;
    struct pii_mask_region regions[MAX_MASK_REGIONS];
};
```

### Python struct layout (using `struct` module)

```python
import struct

HEADER_FMT = '<IIQQIIIx4x'  # or use ctypes for exact match
REGION_FMT = '<fffffi'       # x, y, w, h, corner_radius, flags
```

Or better — use `ctypes.Structure` to guarantee identical layout to C.

---

## 2. IPC Transport Options

### Comparison Matrix

| # | Approach | Python Side | C Side | How It Works | Ease of Writing | Ease of Maint. | Type Safety | Perf/Latency | Mem Overhead | Reliability |
|---|----------|-------------|--------|--------------|:---:|:---:|:---:|:---:|:---:|:---:|
| 1 | **POSIX shm** (`shm_open` + `mmap`) | `multiprocessing.shared_memory` or `posix_ipc` + `mmap` | `shm_open()` + `mmap()` | Both processes map the same named kernel-managed memory object. Writer updates in-place, reader polls. | 3 | 4 | 2 | **5** | **5** | 4 |
| 2 | **Unix domain socket** | `asyncio` or `socket` | `socket()` + `connect()` | Daemon sends geometry updates over a stream/datagram socket. Plugin reads in `video_tick()`. | **5** | **5** | 3 | 3 | 4 | **5** |
| 3 | **Named pipe (FIFO)** | `os.mkfifo()` + `open()` | `open()` + `read()` | One-way byte stream from daemon to plugin. Writer pushes, reader pulls. | 4 | 3 | 2 | 3 | **5** | 2 |
| 4 | **Memory-mapped file** | `mmap` module on regular file | `open()` + `mmap()` | Same as POSIX shm but backed by a file on disk (e.g., `/tmp/pii_mask.bin`). | 4 | 4 | 2 | 4 | **5** | 4 |
| 5 | **Mach ports** | `pyobjc` (CFMessagePort) or raw Mach API | `mach_msg()` or CFMessagePort | macOS-native kernel IPC. Message-based, kernel-mediated. | 1 | 1 | 2 | 3 | 4 | 3 |
| 6 | **XPC** | `pyxpcconnection` or `pyobjc` NSXPCConnection | `xpc_connection_create()` | Apple's recommended IPC framework. Service-oriented, type-safe messages. | 2 | 2 | 4 | 2 | 3 | 4 |

### Detailed Analysis of Each Transport

---

### Option 1: POSIX Shared Memory (`shm_open` + `mmap`)

**How it works:** The daemon calls `shm_open("/pii_mask", O_CREAT|O_RDWR, 0666)` to create a named shared memory object, `ftruncate()` to set its size, and `mmap()` to get a writable pointer. The OBS plugin calls `shm_open("/pii_mask", O_RDONLY, 0)` and `mmap()` with `PROT_READ` to get a read-only view. Both processes see the same physical memory. Synchronization via atomic seqlock counter (see Section 4).

**Python side:**
- `multiprocessing.shared_memory.SharedMemory(name="pii_mask", create=True, size=808)` (stdlib, Python 3.8+)
- Or `posix_ipc.SharedMemory("/pii_mask", ...)` + `mmap.mmap()`
- Write via `ctypes.Structure` or `struct.pack_into()`

**C side:**
```c
int fd = shm_open("/pii_mask", O_RDONLY, 0);
struct pii_mask_shm *shm = mmap(NULL, sizeof(*shm), PROT_READ, MAP_SHARED, fd, 0);
// In video_tick(): read shm->sequence, memcpy if even, verify sequence unchanged
```

**Pros:**
- Zero-copy read (no kernel involvement after initial mmap)
- Lowest possible latency (~100ns for the read itself)
- Data is always "there" — no message passing, no buffering
- 808 bytes fits in L1 cache; hot reads are essentially free
- Kernel-managed lifetime: survives writer restart (stale data detectable via timestamp)
- `multiprocessing.shared_memory` is Python stdlib — no dependencies

**Cons:**
- Must manually ensure identical struct layout between Python and C (no schema enforcement)
- Seqlock adds complexity (but it's ~15 lines of code on each side)
- macOS: `shm_open` names must start with `/` and be <=30 chars total
- No notification mechanism — reader must poll (but `video_tick()` already polls every frame)
- If daemon dies, shm persists in kernel until `shm_unlink()` — need cleanup strategy

**Key references:**
- [Python multiprocessing.shared_memory docs](https://docs.python.org/3/library/multiprocessing.shared_memory.html)
- [POSIX shm_open spec](https://pubs.opengroup.org/onlinepubs/007904875/functions/shm_open.html)
- [posix-ipc PyPI](https://pypi.org/project/posix-ipc/)
- [shm_open+mmap example (Linux+macOS)](https://gist.github.com/pldubouilh/c007a311707798b42f31a8d1a09f1138)

---

### Option 2: Unix Domain Socket

**How it works:** The daemon listens on a Unix domain socket (e.g., `/tmp/pii_mask.sock`). The OBS plugin connects at startup. The daemon sends geometry updates as binary messages whenever the scene changes. The plugin reads available data in `video_tick()` using non-blocking recv.

**Python side:** `asyncio` server on a Unix domain socket, sends `struct.pack()` messages.

**C side:** `socket(AF_UNIX, SOCK_DGRAM, 0)` or `SOCK_STREAM`. Non-blocking `recv()` in `video_tick()`. Drain to latest message.

**Pros:**
- Familiar, well-understood API on both sides
- Built-in flow control and message ordering
- Daemon can push only when data changes (no wasted reads)
- Easy to add bidirectional communication (plugin -> daemon commands)
- Natural connection lifecycle: plugin detects daemon death via `EPIPE`/connection close
- SOCK_DGRAM gives message boundaries without framing

**Cons:**
- Kernel copy on every send/recv (~2us per message vs ~100ns for shm)
- At 60fps polling, non-blocking recv adds syscall overhead even when no data
- Must handle partial reads (SOCK_STREAM) or message size limits (SOCK_DGRAM)
- SOCK_DGRAM on macOS: max datagram ~2048 bytes (our 808 bytes fits, but barely)
- Slightly higher latency: writer must syscall to send, reader must syscall to recv

**Key references:**
- [Unix Domain Sockets — Beej's Guide](https://beej.us/guide/bgipc/html/multi/unixsock.html)
- [IPC Performance Comparison (Baeldung)](https://www.baeldung.com/linux/ipc-performance-comparison)

---

### Option 3: Named Pipe (FIFO)

**How it works:** `mkfifo("/tmp/pii_mask_pipe")`. Daemon writes geometry updates. Plugin opens for reading in non-blocking mode and reads in `video_tick()`.

**Python side:** `os.mkfifo()` + `os.open()` + `os.write()`

**C side:** `open()` + `read()` in non-blocking mode.

**Pros:**
- Simplest conceptual model — just a file-like byte stream
- No networking code, no socket setup
- One-way flow matches our data direction

**Cons:**
- **Stream-oriented with no message boundaries** — must implement framing (length prefix or delimiter)
- Reader must drain the pipe to get latest data (old data accumulates)
- If reader is slow, pipe buffer fills (64KB default on macOS) and writer blocks or gets `EAGAIN`
- **Writer blocks if no reader has opened the pipe** — daemon hangs if OBS hasn't started
- No random access — can't "peek at latest" like shm
- FIFO is removed on system reboot; must recreate
- Crash handling: if reader dies, writer gets `SIGPIPE`; must handle

**Key references:**
- [mkfifo(3) man page](https://man7.org/linux/man-pages/man3/mkfifo.3.html)

---

### Option 4: Memory-Mapped File

**How it works:** Same as POSIX shm (#1) but uses a regular file (e.g., `/tmp/pii_mask.bin`) instead of a kernel shm object. Both sides `open()` the file and `mmap()` with `MAP_SHARED`.

**Python side:** `open("/tmp/pii_mask.bin", "r+b")` + `mmap.mmap(fd, 808)`

**C side:** `open()` + `mmap()`

**Pros:**
- Almost identical to POSIX shm in performance (kernel caches in page cache; no disk I/O for small files that stay hot)
- Easier to debug: can `hexdump /tmp/pii_mask.bin` to inspect state
- No `shm_open`/`shm_unlink` lifecycle — just a file
- File persists across reboots (good or bad depending on perspective)
- Works in all environments; no special permissions

**Cons:**
- Slightly slower first access (page fault to load from disk, though subsequent accesses are memory-speed)
- File must exist before mmap — daemon must create it with correct size first
- Risk of filesystem sync: `msync()` needed to guarantee persistence, though we don't care about persistence
- `/tmp` is cleaned on reboot on macOS, so stale files are handled automatically
- Same struct-layout challenges as POSIX shm

**Key references:**
- [mmap(2) man page](https://man7.org/linux/man-pages/man2/mmap.2.html)
- [Playing with shared memory (Linux+macOS)](https://www.deepanseeralan.com/tech/playing-with-shared-memory/)

---

### Option 5: Mach Ports

**How it works:** macOS-native IPC primitive. Messages are sent via `mach_msg()` through kernel-managed port queues. Can also use the higher-level `CFMessagePort` wrapper.

**Python side:** `pyobjc` to access `CFMessagePortCreateLocal` / `CFMessagePortSendRequest`. Or raw Mach API via ctypes (painful).

**C side:** `CFMessagePortCreateRemote()` + `CFMessagePortSendRequest()`, or raw `mach_msg()`.

**Pros:**
- Native macOS IPC — "the right way" per Apple
- Kernel-mediated with proper access control
- Supports out-of-line memory transfer (large data without copy)
- Built-in port death notification (`MACH_NOTIFY_DEAD_NAME`) for crash detection

**Cons:**
- **Extremely poorly documented** outside Apple internals
- `mach_msg()` API is complex, low-level, error-prone
- Python bindings are thin/unmaintained; `pyobjc` CFMessagePort is the only practical option
- CFMessagePort is deprecated in favor of XPC
- Massive overkill for 808 bytes of data
- No mainstream OBS plugins use this pattern — we'd be pioneers (not in a good way)
- Debugging tools are limited

**Key references:**
- [Mach Messages on macOS (Dennis Babkin)](https://dennisbabkin.com/blog/?t=interprocess-communication-using-mach-messages-for-macos)
- [macOS IPC overview (HackTricks)](https://book.hacktricks.xyz/macos-hardening/macos-security-and-privilege-escalation/macos-proces-abuse/macos-ipc-inter-process-communication)
- [NSHipster IPC overview](https://nshipster.com/inter-process-communication/)

---

### Option 6: XPC

**How it works:** Apple's modern IPC framework. An XPC service is a separate binary that the system manages. Communication is via structured `xpc_dictionary` messages.

**Python side:** `pyxpcconnection` (third-party, limited maintenance) or `pyobjc` `NSXPCConnection`.

**C side:** `xpc_connection_create_mach_service()` + `xpc_connection_send_message()`.

**Pros:**
- Apple's recommended IPC mechanism
- Built-in service lifecycle management (launchd integration)
- Type-safe dictionaries
- Sandbox-friendly
- Automatic crash recovery (system restarts the XPC service)

**Cons:**
- **Designed for service-oriented architecture**, not shared-memory-style polling
- Requires either a launchd plist or embedding the service in an app bundle
- Python support is minimal and fragile (`pyxpcconnection` is abandoned-looking)
- Cannot easily integrate with OBS's `video_tick()` model (XPC is async/callback-based)
- Message passing has higher latency than shm (~10-50us per message)
- Massive over-engineering for our use case

**Key references:**
- [XPC Programming on macOS (Medium)](https://karol-mazurek.medium.com/xpc-programming-on-macos-7e1918573f6d)
- [pyxpcconnection (GitHub)](https://github.com/matthewelse/pyxpcconnection)
- [Apple XPC docs](https://developer.apple.com/documentation/xpc)

---

## 3. Serialization Options

The question of serialization is **orthogonal** to the transport. You could use any serialization over any transport. But practical pairings matter.

### Comparison Matrix

| # | Approach | Python Side | C Side | Ease of Writing | Ease of Maint. | Type Safety | Perf/Latency | Mem Overhead | Reliability |
|---|----------|-------------|--------|:---:|:---:|:---:|:---:|:---:|:---:|
| 7 | **Protocol Buffers** | `protobuf` pip package | `protobuf-c` or nanopb | 3 | 4 | **5** | 2 | 3 | **5** |
| 8 | **FlatBuffers** | `flatbuffers` pip package | `flatbuffers/flatcc` | 2 | 3 | **5** | 4 | 4 | **5** |
| 9 | **Raw struct layout** | `ctypes.Structure` or `struct` module | Direct struct access | 4 | 3 | 1 | **5** | **5** | 3 |
| 10 | **MessagePack** | `msgpack` pip package | `msgpack-c` or `cmp` | **5** | **5** | 3 | 3 | 4 | 4 |

### Detailed Analysis

---

### Option 7: Protocol Buffers

**How it works:** Define a `.proto` schema. `protoc` generates Python classes and C structs. Serialize on write, deserialize on read.

```protobuf
message MaskRegion {
  float x = 1;
  float y = 2;
  float width = 3;
  float height = 4;
  float corner_radius = 5;
  uint32 flags = 6;
}
message MaskData {
  uint32 frame_width = 1;
  uint32 frame_height = 2;
  repeated MaskRegion regions = 3;
}
```

**Pros:**
- Best type safety and schema evolution (add fields without breaking readers)
- Generated code on both sides — no manual struct matching
- Well-documented, battle-tested
- Schema serves as documentation

**Cons:**
- **Serialization/deserialization overhead**: ~1-5us for our tiny payload, but that's 1-5us we don't need to spend
- Protobuf-c adds a build dependency for the OBS plugin
- Varint encoding means wire format != memory layout — no zero-copy
- Overkill for a fixed, stable, 808-byte struct
- Schema changes require regenerating code on both sides

**Key references:**
- [protobuf-c](https://github.com/protobuf-c/protobuf-c)
- [nanopb (embedded-friendly)](https://github.com/nanopb/nanopb)

---

### Option 8: FlatBuffers

**How it works:** Define a `.fbs` schema. `flatc` generates accessors. Data is laid out in a format that can be read **without deserialization** (zero-copy).

**Pros:**
- Zero-copy reads — perfect for shm where the buffer IS the shared memory
- No deserialization step; reader accesses fields via generated offset functions
- Schema evolution (add fields without breaking)
- Faster than protobuf for reads

**Cons:**
- **Builder API is awkward**, especially in C (flatcc library)
- Overhead of vtables and offsets adds ~50-100 bytes to our tiny 808-byte payload
- C support via `flatcc` is less mature than the C++ implementation
- Extra build dependency
- For our data size, the zero-copy advantage is negligible — memcpy of 808 bytes takes ~50ns
- Learning curve is steeper than protobuf

**Key references:**
- [FlatBuffers docs](https://flatbuffers.dev/)
- [flatcc (C implementation)](https://github.com/dvidelabs/flatcc)
- [FlatBuffers benchmarks](https://flatbuffers.dev/benchmarks/)

---

### Option 9: Raw Struct Layout (RECOMMENDED)

**How it works:** Define an identical C struct on both sides. Python uses `ctypes.Structure` to write directly into shared memory with the exact same byte layout. C reads the struct directly via a casted pointer.

**Python writer:**
```python
import ctypes
from multiprocessing.shared_memory import SharedMemory

class MaskRegion(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("width", ctypes.c_float),
        ("height", ctypes.c_float),
        ("corner_radius", ctypes.c_float),
        ("flags", ctypes.c_uint32),
    ]

class MaskShm(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64),
        ("timestamp_ns", ctypes.c_uint64),
        ("frame_width", ctypes.c_uint32),
        ("frame_height", ctypes.c_uint32),
        ("num_regions", ctypes.c_uint32),
        ("_padding", ctypes.c_uint32),
        ("regions", MaskRegion * 32),
    ]
```

**C reader:** Direct cast: `struct pii_mask_shm *shm = (struct pii_mask_shm *)mmap_ptr;`

**Pros:**
- Absolute zero overhead — no serialization, no deserialization, no copies
- Both sides work with native struct access (Python via ctypes, C directly)
- Simplest code: ~20 lines on each side
- No external dependencies
- Perfect for fixed-layout, small-payload, high-frequency updates

**Cons:**
- **No schema evolution** — adding a field requires updating both sides simultaneously
- Must manually ensure struct packing/alignment matches (`_fields_` order, no padding surprises)
- Endianness assumption (both sides are same machine, so this is fine for IPC)
- No built-in validation beyond the magic number

**Mitigation for struct mismatch:** The `magic` and `version` fields catch incompatible layouts. If the plugin sees wrong magic or unexpected version, it refuses to read.

**Key references:**
- [Python ctypes docs](https://docs.python.org/3/library/ctypes.html)

---

### Option 10: MessagePack

**How it works:** Binary serialization format. Python `msgpack` library serializes dicts/lists to compact binary. C `msgpack-c` or `cmp` library deserializes.

**Pros:**
- Very simple API: `msgpack.packb(data)` / `msgpack_unpack()`
- Self-describing format — reader doesn't need compiled schema
- Compact encoding (our 808-byte struct would be ~200-400 bytes)
- Good for variable-length data

**Cons:**
- Serialization + deserialization overhead: ~5-10us for our payload
- Must unpack into local variables every frame (no zero-copy)
- Adds pip dependency (`msgpack`) and C library dependency
- Type mapping is loose (numbers can be int or float depending on value)
- No schema enforcement — easy to ship mismatched data

**Key references:**
- [msgpack.org](https://msgpack.org/)
- [msgpack-python](https://github.com/msgpack/msgpack-python)
- [cmp (minimal C msgpack)](https://github.com/camgunz/cmp)

---

## 4. Lock-Free Synchronization

### The Problem

The Python daemon writes mask data. The OBS plugin reads it in `video_tick()` on the graphics thread. These are different processes on different threads. We need to ensure the reader never sees a **torn write** (partially updated data).

### Approach: Seqlock (Sequence Lock)

A seqlock is the ideal primitive here because:
- **Single writer, single reader** (SWSR) — our exact pattern
- Writer never blocks
- Reader never blocks (just retries on conflict)
- Zero contention in the common case (writes are infrequent relative to reads)

#### How it works

```
Writer (Python daemon):
  1. sequence = atomic_load(&shm->sequence)
  2. atomic_store(&shm->sequence, sequence + 1)  // now ODD = "writing"
  3. atomic_thread_fence(memory_order_release)
  4. // ... write all fields ...
  5. atomic_thread_fence(memory_order_release)
  6. atomic_store(&shm->sequence, sequence + 2)  // now EVEN = "stable"

Reader (OBS C plugin, in video_tick):
  1. seq1 = atomic_load(&shm->sequence)
  2. if (seq1 & 1) goto retry;  // writer is mid-update
  3. atomic_thread_fence(memory_order_acquire)
  4. memcpy(&local_copy, shm, sizeof(*shm))  // copy to local
  5. atomic_thread_fence(memory_order_acquire)
  6. seq2 = atomic_load(&shm->sequence)
  7. if (seq1 != seq2) goto retry;  // write happened during our read
  8. // local_copy is consistent, use it
```

#### Implementation details

**C side (reader):**
```c
static bool read_mask_data(struct pii_mask_shm *shm,
                           struct pii_mask_shm *local)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        uint64_t seq1 = atomic_load_explicit(&shm->sequence,
                                              memory_order_acquire);
        if (seq1 & 1) continue;  // writer active

        memcpy(local, shm, sizeof(*shm));

        uint64_t seq2 = atomic_load_explicit(&shm->sequence,
                                              memory_order_acquire);
        if (seq1 == seq2) return true;  // consistent read
    }
    return false;  // failed after retries — use stale data
}
```

**Python side (writer):**
```python
import ctypes
import time

def write_mask_data(shm_buf, regions, frame_w, frame_h):
    shm = MaskShm.from_buffer(shm_buf)
    # Increment to odd (writing)
    seq = shm.sequence
    shm.sequence = seq + 1
    ctypes.memmove(ctypes.addressof(shm) + 8, b'', 0)  # compiler barrier

    # Write payload
    shm.timestamp_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    shm.frame_width = frame_w
    shm.frame_height = frame_h
    shm.num_regions = len(regions)
    for i, r in enumerate(regions):
        shm.regions[i].x = r.x
        shm.regions[i].y = r.y
        shm.regions[i].width = r.width
        shm.regions[i].height = r.height
        shm.regions[i].corner_radius = r.corner_radius
        shm.regions[i].flags = r.flags

    # Increment to even (stable)
    ctypes.memmove(ctypes.addressof(shm) + 8, b'', 0)  # compiler barrier
    shm.sequence = seq + 2
```

> **Note on atomics in Python:** `ctypes` doesn't provide `atomic_store`. On x86-64 (all current Macs), aligned 8-byte writes are naturally atomic. On ARM64 (Apple Silicon), 8-byte aligned stores are also atomic. So `shm.sequence = value` via ctypes is safe for our SWSR case. For extra safety, use the `atomics` PyPI package.

#### Alternative: Double Buffering

Instead of a seqlock on a single buffer, maintain two buffers (A and B):

```c
struct pii_mask_double_buf {
    _Atomic uint32_t active_index;  // 0 or 1
    struct pii_mask_shm buffers[2];
};
```

Writer writes to the inactive buffer, then atomically flips `active_index`. Reader always reads from `buffers[active_index]`.

**Pros:** Simpler than seqlock; reader never retries.
**Cons:** 2x memory (1,616 bytes — still trivial). Writer must finish the entire write before flipping.

**Recommendation:** Use seqlock. It's the standard pattern, well-understood, and 808 bytes copies in ~50ns — retry cost is negligible.

---

## 5. Crash Detection and Staleness

### Scenario: Python daemon crashes

The shm segment persists in the kernel (POSIX shm) or on disk (mmap file). The OBS plugin continues to read the last-written data.

**Detection methods:**

| Method | How | Latency | Complexity |
|--------|-----|---------|------------|
| **Timestamp age** | Plugin checks `shm->timestamp_ns` against current monotonic clock. If age > threshold (e.g., 2 seconds), consider stale. | 0 (checked every frame) | Trivial |
| **Sequence counter stall** | If `sequence` hasn't changed in N frames, daemon may be dead. | N frames | Trivial |
| **PID liveness check** | Store daemon PID in shm. Plugin calls `kill(pid, 0)` to check if process exists. | 1 syscall | Low |
| **Socket heartbeat** | Maintain a separate Unix domain socket connection. Detect disconnect. | Immediate on TCP RST | Medium |
| **File lock** | Daemon holds `flock()` on a sentinel file. Plugin tries to acquire — if it succeeds, daemon is dead. | 1 syscall | Low |

**Recommended approach:** Timestamp age check (primary) + PID liveness (secondary).

```c
// In video_tick():
uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
uint64_t age_ms = (now - local.timestamp_ns) / 1000000;
if (age_ms > 2000) {
    // Daemon is likely dead — apply full mask (fail-safe)
    filter->daemon_alive = false;
}
```

### Scenario: OBS plugin crashes

No impact. The daemon continues writing to shm. When OBS restarts and the plugin re-initializes, it picks up the latest data.

### Scenario: Both restart

Daemon creates/reopens shm on startup. Plugin opens shm on startup. Order doesn't matter — whichever starts second will find the shm (or create it and wait for data).

**Startup protocol:**
1. Daemon starts: `shm_open(O_CREAT|O_RDWR)`, writes initial data with `magic` and `version`
2. Plugin starts: `shm_open(O_RDONLY)`. If fails (daemon hasn't started), apply full mask (fail-safe). Retry every second.
3. Plugin verifies `magic == 0x50494D53` and `version == 1` before trusting data.

### Cleanup

POSIX shm objects persist until explicitly `shm_unlink()`'d or system reboot. The daemon should:
- Call `shm_unlink("/pii_mask")` in its shutdown handler
- Register `atexit()` handler for clean shutdown
- Use `signal()` handlers for SIGTERM/SIGINT

For the mmap-file approach, `/tmp/pii_mask.bin` is cleaned by macOS on reboot (macOS clears `/tmp` on restart).

---

## 6. macOS Sandboxing Implications

### OBS Studio on macOS

OBS Studio is **not sandboxed** on macOS. It is distributed as a `.dmg` / `.app` bundle outside the Mac App Store. It does not use App Sandbox entitlements.

### Python daemon

If running as a standalone Python process (not from the App Store), it is also **not sandboxed**.

### Implications for shm

Since neither process is sandboxed:
- `shm_open()` works without restrictions
- No App Group ID prefix required for the shm name
- Standard Unix file permissions apply (both processes run as the same user)
- No entitlements needed

### If sandboxing ever applies

If either process were sandboxed (e.g., distributed via App Store):
- POSIX shm names must be prefixed with the App Group ID: `/<group-id>/pii_mask`
- Both processes must share an App Group entitlement
- System V IPC (`shmget`) would NOT work (Apple blocks it in sandbox)
- Memory-mapped files in `/tmp` would work if the sandbox allows `/tmp` access
- XPC would be the Apple-blessed alternative

### Recommendation

Use POSIX shm without worrying about sandbox restrictions. If sandboxing becomes relevant (unlikely for OBS plugins), switch to memory-mapped file in a shared container directory.

---

## 7. Maximum Practical Update Rates

### Per-approach throughput estimates

All estimates for 808-byte payload on Apple Silicon Mac.

| Approach | Updates/sec (theoretical) | Updates/sec (practical) | Bottleneck |
|----------|:---:|:---:|---|
| **POSIX shm + seqlock** | >10,000,000 | **1,000,000+** | CPU cache coherency protocol between cores |
| **Memory-mapped file** | >10,000,000 | **1,000,000+** | Same as shm (page cache, no disk I/O) |
| **Unix domain socket (DGRAM)** | ~500,000 | **100,000-200,000** | Kernel socket buffer copies, syscall overhead |
| **Unix domain socket (STREAM)** | ~300,000 | **50,000-100,000** | Same + TCP-like buffering |
| **Named pipe** | ~200,000 | **50,000-100,000** | Kernel pipe buffer, syscall overhead |
| **Mach ports** | ~100,000 | **20,000-50,000** | Kernel message queuing, mach_msg syscall |
| **XPC** | ~50,000 | **10,000-20,000** | Serialization + Mach ports + dispatch queues |

### What we actually need

The daemon updates mask geometry when windows change — not every frame. Typical rates:
- **Steady state (no window changes):** 0 updates/sec
- **Active window switching:** 5-30 updates/sec
- **CG poller catch-all:** 30 updates/sec
- **Rapid cmd-tab spam:** 60-120 updates/sec (pathological)

**Even the slowest IPC mechanism handles 10,000+ updates/sec.** Performance is not the differentiator for our use case. The differentiator is **read latency in video_tick()** — how fast the OBS plugin can access the latest data.

| Approach | video_tick() read cost |
|----------|---|
| **POSIX shm / mmap file** | ~50-100ns (memcpy 808 bytes from cache) |
| **Unix domain socket** | ~2-5us (non-blocking recv syscall, even if empty) |
| **Named pipe** | ~2-5us (non-blocking read syscall) |

At 60fps, `video_tick()` runs every 16.7ms. A 5us read is 0.03% of the frame budget. A 100ns read is 0.0006%. Both are negligible. But shm avoids polluting the syscall path, which matters for jitter.

---

## 8. Recommendation

### Winner: POSIX shm + raw struct layout + seqlock

| Dimension | Score | Reasoning |
|-----------|:---:|---|
| Latency | 5/5 | ~100ns read in video_tick(). Nothing beats memory-mapped access. |
| Simplicity | 4/5 | ~50 lines of C (reader), ~60 lines of Python (writer). Seqlock adds minor complexity. |
| Reliability | 4/5 | Timestamp staleness detection. Magic/version validation. Fail-safe to full mask. |
| Maintenance | 4/5 | Struct layout is stable (rect geometry doesn't change often). Version field handles evolution. |
| Dependencies | 5/5 | Python stdlib (`multiprocessing.shared_memory`, `ctypes`). C stdlib (`shm_open`, `mmap`). |
| macOS compat | 5/5 | Both processes non-sandboxed. POSIX shm fully supported on macOS. |

### Why not the others?

| Ruled out | Reason |
|-----------|--------|
| Unix domain socket | Excellent choice for command/control channel, but unnecessary overhead for per-frame geometry reads. The plugin should not be doing recv() syscalls 60 times/sec for data that changes 5-30 times/sec. |
| Named pipe | Streaming semantics don't match our "read latest snapshot" model. Pipe accumulates stale messages. No random access. Writer blocks if reader dies. |
| Mach ports / XPC | macOS-specific, poorly documented (Mach) or over-architected (XPC). No advantage for our 808-byte fixed-layout payload. Would make the plugin non-portable. |
| Protocol Buffers / FlatBuffers | Schema safety is nice but overkill. Our struct is 6 fields per rect and a small header. Adding protobuf-c as a build dep and a .proto file is more maintenance than it saves. |
| MessagePack | Same argument — adds a dependency for serialization we don't need. |
| Memory-mapped file | Nearly identical to POSIX shm. Viable fallback. Slightly less clean (file on disk, must pre-create). Use this if shm_open causes issues. |
| Redis / similar | **Dismissed.** Adds a network hop, a daemon dependency, serialization overhead, and ~1ms latency for something that takes 100ns with shm. Appropriate for distributed systems, absurd for same-machine IPC of 808 bytes. |

### Hybrid architecture

Use **two IPC channels:**

1. **POSIX shm (data plane):** Mask geometry, read every frame in `video_tick()`
2. **Unix domain socket (control plane):** Plugin startup/shutdown handshake, daemon commands (reload config, etc.), heartbeat

This separates the hot path (geometry reads) from the cold path (lifecycle events), keeping `video_tick()` syscall-free.

### Implementation plan

```
Phase 1: POSIX shm with raw struct + seqlock
  - Python: multiprocessing.shared_memory + ctypes.Structure
  - C: shm_open + mmap + seqlock reader
  - Staleness: timestamp_ns age check in video_tick()
  - Fail-safe: if shm missing or stale, apply full mask

Phase 2: Unix domain socket control channel
  - Plugin connects to daemon on startup
  - Daemon sends "ready" signal
  - Plugin sends "shutdown" on destroy
  - Heartbeat via socket keepalive

Phase 3 (if needed): Double-buffer shm
  - If seqlock retries cause jitter (unlikely for 808 bytes)
  - Two buffers with atomic index flip
```

---

## Sources

### POSIX Shared Memory
- [Python multiprocessing.shared_memory docs](https://docs.python.org/3/library/multiprocessing.shared_memory.html)
- [POSIX shm_open spec](https://pubs.opengroup.org/onlinepubs/007904875/functions/shm_open.html)
- [posix-ipc PyPI package](https://pypi.org/project/posix-ipc/)
- [shm_open + mmap example (Linux + macOS)](https://gist.github.com/pldubouilh/c007a311707798b42f31a8d1a09f1138)
- [Playing with shared memory (Linux + macOS)](https://www.deepanseeralan.com/tech/playing-with-shared-memory/)
- [POSIX Shared Memory (GeeksforGeeks)](https://www.geeksforgeeks.org/posix-shared-memory-api/)

### Seqlocks and Lock-Free IPC
- [Simple lockfree IPC using shared memory and C11](https://yurovsky.github.io/2015/06/04/lockfree-ipc/)
- [Seqlock (Wikipedia)](https://en.wikipedia.org/wiki/Seqlock)
- [Linux kernel seqlock documentation](https://docs.kernel.org/locking/seqlock.html)
- [atomics PyPI package (lock-free atomics in Python)](https://github.com/doodspav/atomics)

### macOS Sandboxing
- [Apple Developer Forums: shm in sandboxed apps](https://developer.apple.com/forums/thread/719897)
- [Xojo Forum: Shared memory and App Sandbox](https://forum.xojo.com/t/shared-memory-app-sandbox/46405)
- [Mozilla bug: shm_open on Mac](https://bugzilla.mozilla.org/show_bug.cgi?id=1465669)

### IPC Benchmarks
- [unix-ipc-benchmarks (GitHub)](https://github.com/brylee10/unix-ipc-benchmarks)
- [ipc-bench (GitHub)](https://github.com/goldsborough/ipc-bench)
- [IPC Performance Comparison (Baeldung)](https://www.baeldung.com/linux/ipc-performance-comparison)

### Serialization
- [FlatBuffers docs and benchmarks](https://flatbuffers.dev/benchmarks/)
- [Binary Serialization Formats benchmark (2026)](https://medium.com/@shekhar.manna83/binary-serialization-formats-e2703f053010)
- [Optimizing API Performance: Protobuf, FlatBuffers, MessagePack](https://www.cloudthat.com/resources/blog/optimizing-api-performance-with-protocol-buffers-flatbuffers-messagepack-and-cbor)
- [protobuf-c](https://github.com/protobuf-c/protobuf-c)
- [flatcc (C FlatBuffers)](https://github.com/dvidelabs/flatcc)
- [msgpack.org](https://msgpack.org/)

### macOS-Specific IPC
- [Mach Messages on macOS (Dennis Babkin)](https://dennisbabkin.com/blog/?t=interprocess-communication-using-mach-messages-for-macos)
- [macOS IPC overview (HackTricks)](https://book.hacktricks.xyz/macos-hardening/macos-security-and-privilege-escalation/macos-proces-abuse/macos-ipc-inter-process-communication)
- [NSHipster: Inter-Process Communication](https://nshipster.com/inter-process-communication/)
- [XPC Programming on macOS (Medium)](https://karol-mazurek.medium.com/xpc-programming-on-macos-7e1918573f6d)
- [pyxpcconnection (GitHub)](https://github.com/matthewelse/pyxpcconnection)

### OBS Plugin References
- [obs-backgroundremoval (royshil)](https://github.com/royshil/obs-backgroundremoval)
- [obs-composite-blur (FiniteSingularity)](https://github.com/FiniteSingularity/obs-composite-blur)
- [obs-shm-image-source (watfordjc)](https://github.com/watfordjc/obs-shm-image-source)
