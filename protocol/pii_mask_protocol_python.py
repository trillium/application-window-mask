"""
pii_mask shared memory protocol — daemon (writer) side.

Mirrors the C struct layout in pii_mask_protocol_c.h exactly.
"""

import ctypes
import mmap
import os
import struct
import time

SHM_NAME = "/pii_mask"
MAGIC = 0x50494D53  # "PIMS"
VERSION = 1
MAX_RECTS = 32
SHM_SIZE = 808

FLAG_DAEMON_ALIVE = 1 << 0
FLAG_FULL_MASK = 1 << 1

RECT_FLAG_UNSAFE = 1 << 0


class PiiMaskRect(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("width", ctypes.c_float),
        ("height", ctypes.c_float),
        ("corner_radius", ctypes.c_float),
        ("flags", ctypes.c_uint32),
    ]


class PiiMaskShm(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("sequence", ctypes.c_uint32),
        ("rect_count", ctypes.c_uint32),
        ("timestamp_ns", ctypes.c_uint64),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("rects", PiiMaskRect * MAX_RECTS),
    ]


assert ctypes.sizeof(PiiMaskRect) == 24, f"rect size: {ctypes.sizeof(PiiMaskRect)}"
assert ctypes.sizeof(PiiMaskShm) == 808, f"shm size: {ctypes.sizeof(PiiMaskShm)}"


class PiiMaskWriter:
    """Writes mask geometry to POSIX shared memory."""

    def __init__(self):
        self._fd = None
        self._mm = None
        self._shm = None

    def open(self):
        """Create or open the shared memory region."""
        # Use multiprocessing.shared_memory for cross-platform shm_open
        from multiprocessing import shared_memory

        try:
            self._sm = shared_memory.SharedMemory(
                name="pii_mask", create=True, size=SHM_SIZE
            )
        except FileExistsError:
            self._sm = shared_memory.SharedMemory(name="pii_mask", create=False)

        self._shm = PiiMaskShm.from_buffer(self._sm.buf)
        self._shm.magic = MAGIC
        self._shm.version = VERSION
        self._shm.sequence = 0
        self._shm.rect_count = 0
        self._shm.timestamp_ns = time.time_ns()
        self._shm.flags = FLAG_DAEMON_ALIVE

    def write_rects(self, rects: list[dict]):
        """
        Write a list of rects to shared memory using seqlock protocol.

        Each rect dict: {x, y, width, height, corner_radius, unsafe}
        """
        shm = self._shm

        # Begin write: increment sequence to odd
        seq = shm.sequence
        shm.sequence = seq + 1  # odd = writing

        # Write data
        count = min(len(rects), MAX_RECTS)
        shm.rect_count = count
        shm.timestamp_ns = time.time_ns()
        shm.flags = FLAG_DAEMON_ALIVE

        for i in range(count):
            r = rects[i]
            shm.rects[i].x = r["x"]
            shm.rects[i].y = r["y"]
            shm.rects[i].width = r["width"]
            shm.rects[i].height = r["height"]
            shm.rects[i].corner_radius = r.get("corner_radius", 10.0)
            shm.rects[i].flags = RECT_FLAG_UNSAFE if r.get("unsafe", True) else 0

        # End write: increment sequence to even
        shm.sequence = seq + 2  # even = ready

    def write_full_mask(self):
        """Signal that the entire screen should be masked."""
        shm = self._shm
        seq = shm.sequence
        shm.sequence = seq + 1
        shm.rect_count = 0
        shm.timestamp_ns = time.time_ns()
        shm.flags = FLAG_DAEMON_ALIVE | FLAG_FULL_MASK
        shm.sequence = seq + 2

    def close(self):
        """Clean up shared memory."""
        if self._shm:
            self._shm.flags = 0  # clear daemon alive
        if self._sm:
            self._sm.close()
            self._sm.unlink()
