"""
Classifies windows as safe or unsafe based on owner name and layer.

Default-deny: everything is unsafe unless explicitly listed as safe.
App lists loaded from ~/.config/pii-mask/apps.toml (hot-reloadable).
"""


class Classifier:
    """Determines whether a window should be masked."""

    def __init__(self, allow: set[str] | None = None,
                 always_mask: set[str] | None = None):
        from config_loader import DEFAULT_ALLOW, DEFAULT_ALWAYS_MASK
        self._safe_owners = allow if allow is not None else set(DEFAULT_ALLOW)
        self._always_mask = (always_mask if always_mask is not None
                             else set(DEFAULT_ALWAYS_MASK))

    def update_lists(self, allow: set[str], always_mask: set[str]) -> None:
        """Hot-reload: swap in new app lists."""
        self._safe_owners = allow
        self._always_mask = always_mask

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
