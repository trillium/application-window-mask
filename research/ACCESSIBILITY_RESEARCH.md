# macOS Accessibility APIs for Window Detection

Research into how pii_mask can detect window positions, foreground state, and transient
popups (notifications, Spotlight, etc.) that may contain PII.

## Two Window Enumeration APIs

### CGWindowListCopyWindowInfo (Quartz / CoreGraphics)

The **only** way to get a true z-ordered list of all windows across all processes.

**Returns per window:**
- `kCGWindowBounds` -- x, y, width, height
- `kCGWindowLayer` -- integer level (0 = normal, 20+ = overlay/popup)
- `kCGWindowOwnerName` -- process name ("Messages", "Finder", etc.)
- `kCGWindowOwnerPID` -- process ID
- `kCGWindowName` -- window title (may be empty)
- `kCGWindowNumber` -- unique window ID
- `kCGWindowIsOnscreen` -- boolean
- `kCGWindowAlpha` -- opacity

**Z-order:** Returned in front-to-back order. First entry = frontmost.

**Permission:** Screen Recording (System Settings > Privacy > Screen Recording).

**Performance:** ~1-3ms per call. Can poll at 30+ Hz easily.

**Python access:**
```python
from Quartz import CGWindowListCopyWindowInfo, kCGWindowListOptionOnScreenOnly, kCGNullWindowID
windows = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID)
```

### AXUIElement (Accessibility Framework)

Per-application querying. Can read AND write window attributes.

**Key attributes:**
- `AXPosition`, `AXSize`, `AXFrame` -- geometry
- `AXTitle` -- window title
- `AXRole` / `AXSubrole` -- "AXWindow" / "AXStandardWindow", "AXDialog", etc.
- `AXMinimized`, `AXFullScreen`

**Permission:** Accessibility (System Settings > Privacy > Accessibility).

**Limitation:** No global z-order. Must query per-app. Slower (5-50ms for tree walks).

## Detecting Notifications and Popups

**Critical finding:** Notification banners are windows owned by `com.apple.notificationcenterui`
and appear on window layer ~20 (above normal windows at layer 0).

| UI Element | Owner Process | Window Layer | Detectable? |
|---|---|---|---|
| Notification banners | NotificationCenter | ~20 | YES |
| Spotlight search | com.apple.Spotlight | ~20+ | YES |
| Siri | com.apple.siri* | ~20+ | YES |
| System dialogs/alerts | owning app | 0 (AXSubrole="AXDialog") | YES via AX |
| Control Center dropdowns | com.apple.controlcenter | ~20+ | YES |
| Dock hover previews | com.apple.dock | ~20+ | YES |
| Password prompts | SecurityAgent | ~25+ | YES |

**Key insight:** `kCGWindowLayer > 0` catches all overlay/popup/transient UI. This is the
single best heuristic for "things that appeared that the user didn't explicitly ask for."

## What Talon Already Provides

Talon's `ui` module wraps most of this:

**Objects:** `ui.Window` (.rect, .title, .app, .id, .focused), `ui.App` (.name, .bundle,
.pid, .windows()), `ui.Screen` (.rect, .visible_rect)

**Events (via `ui.register()`):**
- `"win_focus"`, `"win_open"`, `"win_close"`, `"win_title"`
- `"app_launch"`, `"app_close"`, `"app_activate"`, `"app_deactivate"`
- `"screen_change"`

**What Talon does NOT provide:**
- True z-order of all windows (only tracks active window)
- Window layer info (needed for popup detection via CG)
- Direct access to `CGWindowListCopyWindowInfo`

## Existing Code We Already Have

| File | What it does |
|------|------|
| `~/.talon/user/trillium/core/app_switcher/window_utils.py` | `windows_in_viewport()` -- enumerates visible windows with rects, inferred z-order via focus history |
| `~/.talon/user/trillium/window_history/window_history.py` | Tracks window focus history via `win_focus` events |
| `~/.talon/user/talon-axkit/notification.py` | Full notification banner detection via `com.apple.notificationcenterui` AX monitoring |
| `~/.talon/user/trillium_obs/streaming/keyboard_blurr_on_window_change.talon` | Blurs OBS on cmd-tab (reactive safety net) |

The **talon-axkit notification.py** is especially relevant -- it already monitors for
notification banners, reads their content, and tracks their lifecycle (appear/dismiss).

## Recommended Approach

### For pii_mask (Talon-integrated):

1. **App focus changes** -- `ui.register("win_focus", ...)`. Already works.
2. **Notification banners** -- Monitor `com.apple.notificationcenterui` via `win_open`/`win_close`.
   talon-axkit already does this. Get the banner rect from AX and add it to the mask.
3. **Window geometry** -- `window.rect` on `ui.Window`. Already used in `window_utils.py`.
4. **Overlay/popup detection** -- Small Swift helper (~20 lines) that calls
   `CGWindowListCopyWindowInfo` and returns JSON. Gives true z-order + window layers.
   This catches Spotlight, Siri, Control Center, and any other transient UI.

### Swift helper concept:

A tiny CLI that outputs window info as JSON, called by pii_mask when it needs a full
window snapshot. Would give us:
- True z-order (not available via Talon's `ui`)
- Window layers (to detect popups vs normal windows)
- All windows including those Talon's `ui` may miss

### Permissions

Talon already requires both Screen Recording and Accessibility permissions, so if pii_mask
runs inside Talon, **no additional permissions are needed**.

If running standalone: need both permissions granted to the process.

## iMessage Specifically

Two defense layers:

1. **Disable the daemon** -- The existing `scripts/imessage` script does `launchctl bootout`
   on imagent. This prevents notifications entirely. Nuclear option.

2. **Mask the banner** -- Detect the notification window via
   `com.apple.notificationcenterui`, get its rect, add a white region to the mask PNG.
   The banner disappears after ~5s, at which point remove the mask region. This preserves
   iMessage functionality while hiding PII from stream.

Option 2 is the better path for pii_mask -- it handles ALL notification sources, not just
iMessage.
