"""
Loads and hot-reloads app classification config from ~/.config/pii-mask/apps.toml.

Config format:
    allow = ["Code", "Google Chrome", "com.microsoft.VSCode"]
    always_mask = ["NotificationCenter", "SecurityAgent"]

Matching is by process name OR bundle ID — both go in the same list.
"""

import os
import tomllib
from pathlib import Path

CONFIG_DIR = Path.home() / ".config" / "pii-mask"
CONFIG_PATH = CONFIG_DIR / "apps.toml"

# Defaults match the original hardcoded SAFE_OWNERS / ALWAYS_MASK_OWNERS.
DEFAULT_ALLOW = [
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
]

DEFAULT_ALWAYS_MASK = [
    "Notification Center",
    "NotificationCenter",
    "SecurityAgent",
]


def _write_toml(allow: list[str], always_mask: list[str], path: Path) -> None:
    """Write config as TOML (tomllib is read-only, so we format manually)."""
    lines = ["# PII mask — app classification config", "#"]
    lines.append("# Process names or bundle IDs allowed on stream (default-deny)")
    lines.append("allow = [")
    for name in sorted(allow):
        lines.append(f'    "{name}",')
    lines.append("]")
    lines.append("")
    lines.append("# Always masked, even if in allow list")
    lines.append("always_mask = [")
    for name in sorted(always_mask):
        lines.append(f'    "{name}",')
    lines.append("]")
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))


def load_config(path: Path = CONFIG_PATH) -> dict:
    """Load config from TOML. Creates default config if missing."""
    if not path.exists():
        _write_toml(DEFAULT_ALLOW, DEFAULT_ALWAYS_MASK, path)
        return {
            "allow": set(DEFAULT_ALLOW),
            "always_mask": set(DEFAULT_ALWAYS_MASK),
        }

    with open(path, "rb") as f:
        data = tomllib.load(f)

    return {
        "allow": set(data.get("allow", DEFAULT_ALLOW)),
        "always_mask": set(data.get("always_mask", DEFAULT_ALWAYS_MASK)),
    }


def save_config(allow: list[str], always_mask: list[str],
                path: Path = CONFIG_PATH) -> None:
    """Save config back to TOML."""
    _write_toml(allow, always_mask, path)


class ConfigWatcher:
    """Checks config file mtime for hot-reload."""

    def __init__(self, path: Path = CONFIG_PATH):
        self._path = path
        self._last_mtime = 0.0
        self._config = None
        self.reload()

    def reload(self) -> dict:
        """Force reload config from disk."""
        self._config = load_config(self._path)
        try:
            self._last_mtime = os.path.getmtime(self._path)
        except OSError:
            self._last_mtime = 0.0
        return self._config

    def check(self) -> dict | None:
        """Return new config if file changed, else None."""
        try:
            mtime = os.path.getmtime(self._path)
        except OSError:
            return None
        if mtime != self._last_mtime:
            return self.reload()
        return None

    @property
    def config(self) -> dict:
        return self._config
