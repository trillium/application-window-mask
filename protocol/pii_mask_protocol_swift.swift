///
/// pii_mask shared memory protocol — Swift implementation.
///
/// Mirrors the C struct layout in pii_mask_protocol_c.h exactly.
/// Can be used as either writer (daemon) or reader.
///

import Foundation

let PII_MASK_SHM_NAME = "/pii_mask"
let PII_MASK_MAGIC: UInt32 = 0x50494D53  // "PIMS"
let PII_MASK_VERSION: UInt32 = 1
let PII_MASK_MAX_RECTS = 32
let PII_MASK_SHM_SIZE = 808

let PII_MASK_FLAG_DAEMON_ALIVE: UInt32 = 1 << 0
let PII_MASK_FLAG_FULL_MASK: UInt32 = 1 << 1
let PII_RECT_FLAG_UNSAFE: UInt32 = 1 << 0

struct PiiMaskRect {
    var x: Float
    var y: Float
    var width: Float
    var height: Float
    var cornerRadius: Float
    var flags: UInt32
}

struct PiiMaskHeader {
    var magic: UInt32
    var version: UInt32
    var sequence: UInt32  // atomic via OSAtomicOr32Barrier or similar
    var rectCount: UInt32
    var timestampNs: UInt64
    var flags: UInt32
    var reserved: UInt32
}

// Verify sizes at compile time
#if swift(>=5.0)
    // Swift doesn't have _Static_assert but we can check at runtime in tests
#endif

class PiiMaskWriter {
    private var fd: Int32 = -1
    private var ptr: UnsafeMutableRawPointer?

    func open() throws {
        fd = shm_open(PII_MASK_SHM_NAME, O_CREAT | O_RDWR, 0o666)
        guard fd >= 0 else {
            throw NSError(domain: "PiiMask", code: Int(errno),
                          userInfo: [NSLocalizedDescriptionKey: "shm_open failed"])
        }

        ftruncate(fd, off_t(PII_MASK_SHM_SIZE))

        ptr = mmap(nil, PII_MASK_SHM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0)
        guard ptr != MAP_FAILED else {
            throw NSError(domain: "PiiMask", code: Int(errno),
                          userInfo: [NSLocalizedDescriptionKey: "mmap failed"])
        }

        // Initialize header
        let header = ptr!.bindMemory(to: PiiMaskHeader.self, capacity: 1)
        header.pointee.magic = PII_MASK_MAGIC
        header.pointee.version = PII_MASK_VERSION
        header.pointee.sequence = 0
        header.pointee.rectCount = 0
        header.pointee.timestampNs = UInt64(clock_gettime_nsec_np(CLOCK_REALTIME))
        header.pointee.flags = PII_MASK_FLAG_DAEMON_ALIVE
    }

    func writeRects(_ rects: [PiiMaskRect]) {
        guard let ptr = ptr else { return }

        let header = ptr.bindMemory(to: PiiMaskHeader.self, capacity: 1)
        let rectsPtr = (ptr + MemoryLayout<PiiMaskHeader>.size)
            .bindMemory(to: PiiMaskRect.self, capacity: PII_MASK_MAX_RECTS)

        let seq = header.pointee.sequence

        // Begin write (odd)
        OSAtomicIncrement32Barrier(
            UnsafeMutablePointer(&header.pointee.sequence)
                .withMemoryRebound(to: Int32.self, capacity: 1) { $0 }
        )

        let count = min(rects.count, PII_MASK_MAX_RECTS)
        header.pointee.rectCount = UInt32(count)
        header.pointee.timestampNs = UInt64(clock_gettime_nsec_np(CLOCK_REALTIME))
        header.pointee.flags = PII_MASK_FLAG_DAEMON_ALIVE

        for i in 0..<count {
            rectsPtr[i] = rects[i]
        }

        // End write (even)
        OSAtomicIncrement32Barrier(
            UnsafeMutablePointer(&header.pointee.sequence)
                .withMemoryRebound(to: Int32.self, capacity: 1) { $0 }
        )
    }

    func close() {
        if let ptr = ptr {
            let header = ptr.bindMemory(to: PiiMaskHeader.self, capacity: 1)
            header.pointee.flags = 0
            munmap(ptr, PII_MASK_SHM_SIZE)
        }
        if fd >= 0 {
            Darwin.close(fd)
            shm_unlink(PII_MASK_SHM_NAME)
        }
    }
}
