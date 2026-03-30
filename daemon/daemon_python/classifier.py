"""
Classifies windows as safe or unsafe based on owner name and layer.

Default-deny: everything is unsafe unless explicitly listed as safe.
"""

# Safe apps — windows from these processes are revealed on stream.
# Everything else is masked.
SAFE_OWNERS = {
    "Code",
    "Google Chrome",
    "OBS",
    "OBS Studio",
    "RODE Connect",
    "Talon",
    "Terminal",
    "iTerm2",
    "Slack",
    "System Settings",
    "System Preferences",
    "Finder",
}

# Always mask windows from these processes, even if in SAFE_OWNERS.
ALWAYS_MASK_OWNERS = {
    "NotificationCenter",
    "SecurityAgent",
}


class Classifier:
    """Determines whether a window should be masked."""

    def __init__(self):
        self._safe_owners = SAFE_OWNERS
        self._always_mask = ALWAYS_MASK_OWNERS

    def is_unsafe(
        self,
        owner: str,
        bundle: str = "",
        title: str = "",
        layer: int = 0,
    ) -> bool:
        """
        Returns True if the window should be masked (unsafe).

        Rules evaluated in order:
        1. Negative layers → skip (desktop, wallpaper, backstop)
        2. Always-mask processes → unsafe
        3. Owner in safe list → safe (even overlays from safe apps)
        4. Layer > 0 from unsafe apps → unsafe
        5. Everything else → unsafe
        """
        # Rule 1: negative layers are below desktop — never mask
        if layer < 0:
            return False

        # Rule 2: always-mask processes
        if owner in self._always_mask:
            return True

        # Rule 3: safe list (safe apps' overlays are also safe)
        if owner in self._safe_owners:
            return False

        # Rule 4: overlays from unknown apps
        if layer > 0:
            return True

        # Rule 5: default deny
        return True
