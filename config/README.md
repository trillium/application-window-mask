# Config — Shared Configuration

Configuration files used by the daemon and classifier.

## config.toml

```toml
[mask]
# Path to shared memory (default: /pii_mask)
shm_name = "/pii_mask"

# Default corner radius for macOS windows (points)
default_corner_radius = 10.0

# Polling rate for CGWindowListCopyWindowInfo (Hz)
poll_rate = 30

[safe_apps]
# Bundle IDs of apps that are safe to show on stream.
# Everything else is masked by default.
allow = [
    "com.microsoft.VSCode",
    "com.google.Chrome",
    "com.obsproject.obs-studio",
    "com.rode.RODE-Connect",
    "com.apple.Terminal",
    "com.tinyspeck.slackmacgap",
    "com.apple.systempreferences",
]

[safe_apps.title_overrides]
# Per-app window title patterns that override the app-level rule.
# Regex patterns matched against window title.
"com.google.Chrome" = [
    { pattern = "Gmail", action = "unsafe" },
    { pattern = "WhatsApp", action = "unsafe" },
    { pattern = "Facebook", action = "unsafe" },
]

[overlays]
# Always mask windows on these layers (system overlays, notifications)
mask_layers_above = 0

# Always mask windows from these processes regardless of layer
mask_processes = [
    "NotificationCenter",
    "SecurityAgent",
]
```
