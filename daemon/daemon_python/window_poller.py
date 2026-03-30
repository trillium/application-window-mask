"""
Polls macOS for all on-screen windows via CGWindowListCopyWindowInfo.

Returns a list of window dicts with position, size, owner, layer.
"""

from Quartz import (
    CGWindowListCopyWindowInfo,
    kCGWindowListOptionOnScreenOnly,
    kCGNullWindowID,
    kCGWindowBounds,
    kCGWindowLayer,
    kCGWindowOwnerName,
    kCGWindowOwnerPID,
    kCGWindowName,
    kCGWindowNumber,
)


class WindowPoller:
    """Polls CGWindowListCopyWindowInfo for on-screen windows."""

    def poll(self) -> list[dict]:
        """
        Return all on-screen windows with their metadata.

        Each dict contains:
            owner: str  — process name
            title: str  — window title (may be empty)
            pid: int    — process ID
            layer: int  — window layer (0 = normal, >0 = overlay)
            x, y, width, height: float — window bounds in logical points
            window_id: int — unique window number
        """
        window_list = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly, kCGNullWindowID
        )

        if not window_list:
            return []

        windows = []
        for w in window_list:
            bounds = w.get(kCGWindowBounds)
            if not bounds:
                continue

            owner = w.get(kCGWindowOwnerName, "")
            if not owner:
                continue

            windows.append({
                "owner": owner,
                "title": w.get(kCGWindowName, "") or "",
                "pid": w.get(kCGWindowOwnerPID, 0),
                "layer": w.get(kCGWindowLayer, 0),
                "x": bounds.get("X", 0),
                "y": bounds.get("Y", 0),
                "width": bounds.get("Width", 0),
                "height": bounds.get("Height", 0),
                "window_id": w.get(kCGWindowNumber, 0),
            })

        return windows
