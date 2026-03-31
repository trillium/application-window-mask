/// Shared memory writer using the pii_mask protocol.
///
/// Writes mask rects to /pii_mask POSIX shared memory using a seqlock
/// for lock-free synchronization with the OBS plugin reader.
///
/// Layout matches pii_mask_protocol_c.h exactly (800 bytes).

import Darwin
import CSeqlock

// Protocol constants
let shmName = "/pii_mask"
let shmMagic: UInt32 = 0x50494D53  // "PIMS"
let shmVersion: UInt32 = 1
let maxRects = 32
let shmSize = 800

let flagDaemonAlive: UInt32 = 1 << 0
let flagFullMask: UInt32 = 1 << 1
let rectFlagUnsafe: UInt32 = 1 << 0

// Byte offsets matching the C struct layout
private let offMagic       = 0
private let offVersion     = 4
private let offSequence    = 8
private let offRectCount   = 12
private let offTimestamp   = 16
private let offFlags       = 24
private let offScreenW     = 28
private let offScreenH     = 30
private let offRects       = 32
private let rectStride     = 24  // 5 floats + 1 uint32

struct MaskRect {
    var x: Float
    var y: Float
    var width: Float
    var height: Float
    var cornerRadius: Float
    var flags: UInt32
}

final class ShmWriter {
    private var fd: Int32 = -1
    private var ptr: UnsafeMutableRawPointer?

    func open(screenWidth: UInt16, screenHeight: UInt16) throws {
        fd = shm_open_wrapper(shmName, O_CREAT | O_RDWR, 0o666)
        guard fd >= 0 else {
            throw ShmError.openFailed(errno)
        }
        ftruncate(fd, off_t(shmSize))

        ptr = mmap(nil, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
        guard ptr != MAP_FAILED else {
            Darwin.close(fd)
            throw ShmError.mmapFailed(errno)
        }

        // Initialize header
        ptr!.storeBytes(of: shmMagic, toByteOffset: offMagic, as: UInt32.self)
        ptr!.storeBytes(of: shmVersion, toByteOffset: offVersion, as: UInt32.self)
        ptr!.storeBytes(of: UInt32(0), toByteOffset: offSequence, as: UInt32.self)
        ptr!.storeBytes(of: UInt32(0), toByteOffset: offRectCount, as: UInt32.self)
        ptr!.storeBytes(of: currentTimestampNs(), toByteOffset: offTimestamp, as: UInt64.self)
        ptr!.storeBytes(of: flagDaemonAlive, toByteOffset: offFlags, as: UInt32.self)
        ptr!.storeBytes(of: screenWidth, toByteOffset: offScreenW, as: UInt16.self)
        ptr!.storeBytes(of: screenHeight, toByteOffset: offScreenH, as: UInt16.self)
    }

    func writeRects(_ rects: [MaskRect]) {
        guard let ptr else { return }
        let count = min(rects.count, maxRects)
        let seqPtr = ptr + offSequence

        seqlock_begin_write(seqPtr)

        ptr.storeBytes(of: UInt32(count), toByteOffset: offRectCount, as: UInt32.self)
        ptr.storeBytes(of: currentTimestampNs(), toByteOffset: offTimestamp, as: UInt64.self)
        ptr.storeBytes(of: flagDaemonAlive, toByteOffset: offFlags, as: UInt32.self)

        for i in 0..<count {
            let base = offRects + i * rectStride
            ptr.storeBytes(of: rects[i].x, toByteOffset: base, as: Float.self)
            ptr.storeBytes(of: rects[i].y, toByteOffset: base + 4, as: Float.self)
            ptr.storeBytes(of: rects[i].width, toByteOffset: base + 8, as: Float.self)
            ptr.storeBytes(of: rects[i].height, toByteOffset: base + 12, as: Float.self)
            ptr.storeBytes(of: rects[i].cornerRadius, toByteOffset: base + 16, as: Float.self)
            ptr.storeBytes(of: rects[i].flags, toByteOffset: base + 20, as: UInt32.self)
        }

        seqlock_end_write(seqPtr)
    }

    func writeFullMask() {
        guard let ptr else { return }
        let seqPtr = ptr + offSequence

        seqlock_begin_write(seqPtr)
        ptr.storeBytes(of: UInt32(0), toByteOffset: offRectCount, as: UInt32.self)
        ptr.storeBytes(of: currentTimestampNs(), toByteOffset: offTimestamp, as: UInt64.self)
        ptr.storeBytes(of: flagDaemonAlive | flagFullMask, toByteOffset: offFlags, as: UInt32.self)
        seqlock_end_write(seqPtr)
    }

    func heartbeat() {
        guard let ptr else { return }
        let seqPtr = ptr + offSequence

        seqlock_begin_write(seqPtr)
        ptr.storeBytes(of: currentTimestampNs(), toByteOffset: offTimestamp, as: UInt64.self)
        seqlock_end_write(seqPtr)
    }

    func markNotAlive() {
        guard let ptr else { return }
        ptr.storeBytes(of: UInt32(0), toByteOffset: offFlags, as: UInt32.self)
    }

    func close() {
        markNotAlive()
        if let ptr {
            munmap(ptr, shmSize)
            self.ptr = nil
        }
        if fd >= 0 {
            Darwin.close(fd)
            fd = -1
            // Do NOT shm_unlink — keep segment alive so plugin mmap stays valid
        }
    }

    private func currentTimestampNs() -> UInt64 {
        UInt64(clock_gettime_nsec_np(CLOCK_REALTIME))
    }
}

enum ShmError: Error {
    case openFailed(Int32)
    case mmapFailed(Int32)
}
