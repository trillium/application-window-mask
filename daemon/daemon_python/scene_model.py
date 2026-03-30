"""
Tracks the current set of unsafe window rects and detects changes.

Only triggers a shared memory write when the scene actually changed,
avoiding unnecessary writes when nothing moved.
"""


class SceneModel:
    """Maintains window state and dirty detection."""

    def __init__(self):
        self._last_rects = []

    def update(self, rects: list[dict]) -> bool:
        """
        Update the scene model with new rects.

        Returns True if the rects changed (dirty), False if unchanged.
        """
        if self._rects_equal(self._last_rects, rects):
            return False

        self._last_rects = [dict(r) for r in rects]
        return True

    def _rects_equal(self, a: list[dict], b: list[dict]) -> bool:
        """Compare two rect lists for equality."""
        if len(a) != len(b):
            return False

        for ra, rb in zip(a, b):
            if (ra["x"] != rb["x"] or
                ra["y"] != rb["y"] or
                ra["width"] != rb["width"] or
                ra["height"] != rb["height"]):
                return False

        return True
