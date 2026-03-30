"""
Thin wrapper around protocol's PiiMaskWriter for the daemon.
"""

import sys
import os

# Add protocol to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "protocol"))

from pii_mask_protocol_python import PiiMaskWriter


class ShmWriter:
    """Manages shared memory lifecycle for the daemon."""

    def __init__(self):
        self._writer = PiiMaskWriter()

    def open(self):
        self._writer.open()

    def write_rects(self, rects: list[dict]):
        self._writer.write_rects(rects)

    def write_full_mask(self):
        self._writer.write_full_mask()

    def heartbeat(self):
        self._writer.heartbeat()

    def close(self):
        self._writer.close()
