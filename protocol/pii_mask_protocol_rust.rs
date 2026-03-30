///
/// pii_mask shared memory protocol — Rust implementation.
///
/// Mirrors the C struct layout in pii_mask_protocol_c.h exactly.
///

use std::sync::atomic::{AtomicU32, Ordering};

pub const SHM_NAME: &str = "/pii_mask";
pub const MAGIC: u32 = 0x50494D53; // "PIMS"
pub const VERSION: u32 = 1;
pub const MAX_RECTS: usize = 32;
pub const SHM_SIZE: usize = 808;

pub const FLAG_DAEMON_ALIVE: u32 = 1 << 0;
pub const FLAG_FULL_MASK: u32 = 1 << 1;
pub const RECT_FLAG_UNSAFE: u32 = 1 << 0;

pub const STALE_TIMEOUT_NS: u64 = 5_000_000_000; // 5 seconds

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct PiiMaskRect {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub corner_radius: f32,
    pub flags: u32,
}

#[repr(C)]
pub struct PiiMaskShm {
    pub magic: u32,
    pub version: u32,
    pub sequence: AtomicU32,
    pub rect_count: u32,
    pub timestamp_ns: u64,
    pub flags: u32,
    pub reserved: u32,
    pub rects: [PiiMaskRect; MAX_RECTS],
}

// Compile-time size checks
const _: () = assert!(std::mem::size_of::<PiiMaskRect>() == 24);
const _: () = assert!(std::mem::size_of::<PiiMaskShm>() == 808);

impl PiiMaskShm {
    /// Reader: begin a seqlock read. Returns None if writer is mid-update.
    pub fn read_begin(&self) -> Option<u32> {
        let seq = self.sequence.load(Ordering::Acquire);
        if seq & 1 == 0 {
            Some(seq)
        } else {
            None // writer is mid-update
        }
    }

    /// Reader: validate that data wasn't torn during read.
    pub fn read_valid(&self, seq: u32) -> bool {
        std::sync::atomic::fence(Ordering::Acquire);
        self.sequence.load(Ordering::Relaxed) == seq
    }

    /// Check if the data is stale (daemon may have crashed).
    pub fn is_stale(&self) -> bool {
        let now_ns = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos() as u64;
        (now_ns - self.timestamp_ns) > STALE_TIMEOUT_NS
    }
}
