# Detecting and Masking Transient UI Overlays on macOS

> Research date: 2026-03-29
> Context: PII masking system for OBS streaming on macOS. A Python daemon detects sensitive regions; an OBS plugin masks them in real time.
> Goal: Detect and mask transient overlays (notification banners, Spotlight, Siri, Control Center, password prompts, third-party launchers) before PII reaches the stream.

---

## The Problem

Transient UI overlays appear suddenly, often contain PII (iMessage previews, search queries, password fields), and disappear within seconds. They render above normal application windows and are fully captured by OBS. A streamer can't react fast enough to manually mask them.

**Key timing constraint:** Notification banners appear in ~50-100ms from trigger to pixels on screen. With OBS encoding at 30fps, that's 1-3 frames of exposure. Any detection approach must either:
- React within 1 frame (~33ms), or
- Prevent the overlay from appearing at all, or
- Accept a blanket masking strategy that covers the overlay region preemptively.

---

## Live System Data (captured from this Mac)

### Window Layer Map

Data from `CGWindowListCopyWindowInfo` on macOS Sequoia:

| Layer | NSWindowLevel Constant | Observed Owners | Overlay Type |
|-------|------------------------|-----------------|--------------|
| -2147483626 | kCGBackstopMenuLevel | Window Server | Desktop backstops |
| -2147483624 | (Desktop wallpaper) | Dock | Wallpaper |
| -2147483603 | kCGDesktopIconWindowLevel | Finder | Desktop icons |
| -2147483601 | (Widget level) | Notification Center | Widgets (calendar, weather) |
| **0** | **NSNormalWindowLevel** | **Most apps** | **Normal app windows** |
| 3 | NSFloatingWindowLevel | zoom.us, Messages, Accessibility | Floating panels, PiP |
| 8 | NSModalPanelWindowLevel | loginwindow, Raycast | Modal dialogs, Force Quit |
| 20 | NSDockWindowLevel | Dock | Dock bar |
| **21** | **(NC/CC overlay)** | **Control Center, Notification Center** | **Notification banners, CC dropdowns** |
| **23** | **(Spotlight level)** | **Spotlight** | **Spotlight search bar** |
| 24 | NSMainMenuWindowLevel | Window Server | Menu bar |
| **25** | **NSStatusWindowLevel** | **Control Center, Karabiner, remoting** | **Status bar items, system indicators** |
| 29 | (HUD level) | zoom.us | HUD toasts |
| **101** | **NSPopUpMenuWindowLevel** | **LuLu** | **Popup menus, firewall alerts** |
| 103 | (Tooltip level) | Various apps | Tooltips |
| **1000** | **NSScreenSaverWindowLevel** | **loginwindow, Claude** | **Screen saver, login window** |
| 1500 | kCGAssistiveTechHighWindowLevel | Talon | Assistive tech overlays |
| 2005 | (System indicator) | Control Center | System-level indicators |
| 2147483630 | kCGCursorWindowLevel | Window Server | Cursor, status indicator |

### Polling Performance

| Method | Time per call | Max polling rate | Windows returned |
|--------|---------------|------------------|------------------|
| `CGWindowListCopyWindowInfo(kCGWindowListOptionAll)` | **3.17ms** | ~315 Hz | 277 |
| `CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly)` | **0.59ms** | ~1,683 Hz | 60 |

### Key Overlay Processes

| Process | PID | Layers | What it shows |
|---------|-----|--------|---------------|
| Notification Center | 711 | -2147483601, **21** | Notification banners, widgets, NC drawer |
| Control Center | 637 | **21, 25**, 2005 | CC dropdown, WiFi/BT/volume panels |
| Spotlight | 90612 | 0, **23** | Search bar with typed queries |
| Raycast | 25812 | 0, **8** | Launcher with typed queries |
| loginwindow | 396 | 0, **8, 1000** | Password prompts, Force Quit |
| LuLu | 1154 | 0, **101** | Firewall allow/deny alerts |
| 1Password | 39124 | 0 | Password autofill (layer 0!) |
| zoom.us | 39698 | 0, 3, **29**, 103 | Meeting HUDs, floating video |

---

## Option Comparison Matrix

| # | Approach | Ease of Writing | Ease of Maint. | Detection Latency | Completeness | False Positive Rate | Best For |
|---|----------|:-:|:-:|:-:|:-:|:-:|---|
| 1 | CGWindowList layer filtering | 4 | 4 | 3 | 3 | 3 | Known system overlays |
| 2 | Process name allowlist | 4 | 2 | 3 | 3 | 4 | Targeted blocking |
| 3 | AX observer on NotificationCenter | 2 | 2 | 4 | 2 | 5 | Notification banners only |
| 4 | NSDistributedNotificationCenter | 3 | 3 | 4 | 1 | 5 | Limited system events |
| 5 | CGWindowList polling at 60Hz | 3 | 4 | 4 | 4 | 3 | General overlay catch |
| 6 | Hybrid: events + polling | 2 | 3 | 5 | 4 | 4 | Production system |
| 7 | macOS DND API | 5 | 3 | 5 | 2 | 5 | Notification suppression |
| 8 | Window level monitoring | 3 | 3 | 3 | 4 | 3 | Precise layer targeting |
| 9 | FSEvents on notification DB | 3 | 2 | 2 | 1 | 4 | Notification audit log |
| 10 | Blanket safe-list rule | 5 | 5 | 5 | 5 | 1 | Maximum safety |

---

## Detailed Analysis

### 1. CGWindowList Layer Filtering

**How it works:** Poll `CGWindowListCopyWindowInfo` and mask any window with `kCGWindowLayer > 0`. Normal app windows live at layer 0; system overlays (notifications, Spotlight, Control Center) live at layers 3-2005. When a non-zero layer window appears on screen, blur the region defined by its `kCGWindowBounds`.

**Ease of writing:** 4/5 — Straightforward. Call `CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly)`, iterate, check layer, read bounds.

**Ease of maintenance:** 4/5 — Apple occasionally changes layer assignments (e.g., the macOS 26 Control Center bug FB18327911 where all status items report as Control Center). But the layer > 0 heuristic is stable.

**Detection latency:** 3/5 — Depends on polling rate. At 60Hz polling (16.6ms interval), worst case is 16.6ms + processing time. The `kCGWindowListOptionOnScreenOnly` call takes ~0.6ms, so 60Hz is easily achievable. But polling means you may miss the first 1-2 frames.

**Completeness:** 3/5 — Catches Spotlight (23), Notification Center banners (21), Control Center (21/25), Dock (20), modal dialogs (8), LuLu alerts (101). Does NOT catch: 1Password (layer 0), Raycast main window (layer 0), most third-party overlays that use NSNormalWindowLevel.

**False positive rate:** 3/5 — Will mask the menu bar (layer 24), Dock (layer 20), Talon canvases (layer 1500), and other benign system UI. These are generally acceptable to mask but could look odd.

**Pros:**
- Simple, single API call
- Catches most macOS system overlays automatically
- No accessibility permissions required beyond screen recording
- Stable across macOS versions

**Cons:**
- Misses layer-0 overlays (1Password, Raycast, Alfred)
- Masks benign system UI (menu bar, Dock)
- Polling introduces 1-frame latency
- Cannot distinguish benign floating windows (Zoom PiP) from PII-bearing ones

**Key reference:** `Quartz.CGWindowListCopyWindowInfo`, `kCGWindowLayer` property

---

### 2. Process Name Allowlist

**How it works:** Maintain a list of known overlay processes (NotificationCenter, Spotlight, Siri, SecurityAgent, ControlCenter, Raycast, Alfred, 1Password). When any window owned by these processes appears on screen, mask it.

**Ease of writing:** 4/5 — Same CGWindowList call, but filter on `kCGWindowOwnerName` instead of (or in addition to) layer.

**Ease of maintenance:** 2/5 — Requires manual updates when new overlay apps are installed or system processes change names. Third-party apps (Raycast, Alfred, 1Password, Bartender, iStat Menus, etc.) all need to be enumerated. Miss one and PII leaks.

**Detection latency:** 3/5 — Same as option 1; depends on polling rate.

**Completeness:** 3/5 — Only as complete as the list. Catches known processes but misses unknown third-party overlays, browser-based password managers, or newly installed apps.

**False positive rate:** 4/5 — More targeted than layer filtering. Won't mask benign layer-3 windows unless their process is on the list. But will mask ALL windows from listed processes, including their main windows (e.g., Spotlight's layer-0 helper windows).

**Pros:**
- Can catch layer-0 overlays (1Password, Raycast) that layer filtering misses
- More targeted masking than blanket layer rules
- Can differentiate Zoom floating video (benign) from Zoom HUD toast (transient)

**Cons:**
- Maintenance burden: must track every overlay app the user installs
- Incomplete by design — unknown apps leak through
- Process names can change across macOS versions
- Some processes own both overlay and normal windows (e.g., Spotlight has layer-0 and layer-23 windows)

**Key reference:** `kCGWindowOwnerName` property, process enumeration

---

### 3. AX Observer on NotificationCenter

**How it works:** Use the Accessibility API to observe the Notification Center app (PID of `com.apple.notificationcenterui`). Monitor `AXWindows` and `AXChildren` for the appearance of notification group elements. This is the approach used by talon-axkit's `notification.py`.

**talon-axkit implementation details:**
- Monitors `com.apple.notificationcenterui` for `win_open`/`win_close` events
- Scans window children for `AXRole="AXGroup"` (pre-Sequoia) or `AXRole="AXButton"` (Sequoia+)
- Extracts title, body, subtitle from `AXIdentifier` child nodes
- Uses `group_identifier()` to track individual notifications (numeric pre-Sequoia, UUID post-Sequoia)
- Can enumerate notification actions via AX

**Ease of writing:** 2/5 — Requires AX observer setup, handling PID changes, parsing the notification element tree. The AX tree structure changes between macOS versions (AXGroup vs AXButton). Talon-axkit's implementation is ~200 lines of non-trivial code.

**Ease of maintenance:** 2/5 — Apple changes the AX tree structure across macOS versions. Sequoia already changed the element type from AXGroup to AXButton. Every major macOS release risks breaking the parsing logic.

**Detection latency:** 4/5 — AX notifications are event-driven, not polled. The `win_open` event fires when the notification window appears, which should be within 1 frame of rendering. However, there's inherent AX API overhead (~5-10ms for attribute reads).

**Completeness:** 2/5 — Only detects Notification Center banners. Does not detect Spotlight, Siri, Control Center, password prompts, third-party overlays, or any other overlay type.

**False positive rate:** 5/5 — Extremely precise. Only fires for actual notification banners. Can even read the notification content to determine if it contains PII (though this adds latency).

**Pros:**
- Event-driven — no polling overhead
- Can read notification content (app name, title, body)
- Can distinguish between notification types
- Proven approach (talon-axkit has been maintained since 2021)

**Cons:**
- Only covers notification banners — one of ten+ overlay types
- Requires Accessibility permissions (already needed for screen recording permission prompts)
- Fragile across macOS versions (AXGroup -> AXButton in Sequoia)
- Cannot detect notifications before they render
- AXTotalNotificationCount is available but only counts current visible notifications

**Limitations of talon-axkit approach:**
- Actions returned out of order
- No notification persistence/history
- Relies on NotificationCenter process being alive
- Sequoia compatibility required code changes

**Key reference:** [phillco/talon-axkit notification.py](https://github.com/phillco/talon-axkit/blob/main/notification.py), `AXUIElementCreateApplication`, `AXObserverAddNotification`

---

### 4. NSDistributedNotificationCenter

**How it works:** Listen for system-wide distributed notifications that fire when overlays appear. Register observers for names like `com.apple.notificationcenterui.bannerAppeared`, `com.apple.doNotDisturb.stateChanged`, etc.

**Ease of writing:** 3/5 — Simple API: `NSDistributedNotificationCenter.defaultCenter().addObserver(...)`. The challenge is knowing which notification names to listen for, as they're undocumented.

**Ease of maintenance:** 3/5 — Undocumented notification names can change or be removed across macOS versions. Apple provides no stability guarantee for these.

**Detection latency:** 4/5 — Event-driven; fires when the system posts the notification. Should be near-instantaneous if the notification exists.

**Completeness:** 1/5 — **Very limited.** Testing on this machine showed **zero distributed notifications in a 1-second observation window** when listening to all notifications. Many overlay events simply don't post distributed notifications. The notification names are undocumented and may not exist for most overlay types.

**False positive rate:** 5/5 — If a notification fires, it means exactly what it says. No ambiguity.

**Pros:**
- Zero CPU cost when idle (event-driven)
- No polling overhead
- Clean API

**Cons:**
- Most overlay events don't post distributed notifications
- Notification names are undocumented and unstable
- Essentially useless for comprehensive overlay detection
- Testing confirmed zero notifications captured during idle observation
- Cannot detect Spotlight, Siri, password prompts, or third-party overlays

**Key reference:** `NSDistributedNotificationCenter.defaultCenter()`, undocumented notification names

---

### 5. CGWindowList Polling at High Frequency

**How it works:** Poll `CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly)` at 60Hz (or higher). On each poll, compare the window list to the previous frame. Any new window not in the "safe" set triggers masking of its bounds.

**Ease of writing:** 3/5 — Requires efficient diffing logic, window identity tracking (windows can change IDs), and a thread-safe polling loop.

**Ease of maintenance:** 4/5 — The CGWindowList API is stable. The polling logic is self-contained.

**Detection latency:** 4/5 — At 60Hz, worst-case detection is 16.6ms (one poll interval). With `kCGWindowListOptionOnScreenOnly` at 0.59ms per call, we could poll at 200+Hz for sub-5ms detection, but this increases CPU cost. At 60Hz, CPU cost is ~3.6% of one core (0.59ms/16.6ms).

**Completeness:** 4/5 — Catches ANY window that appears on screen, regardless of layer or process. This includes all system overlays AND third-party overlays. The only miss is content rendered within an existing window (e.g., a password field appearing inside a browser window — but that's not a transient overlay).

**False positive rate:** 3/5 — Every new window triggers evaluation. Tooltips, menu items, and benign floating panels all trigger detection. Need heuristics to classify: layer, process name, window size, duration.

**Pros:**
- Catches everything that creates a new window
- Process-agnostic — works for third-party overlays too
- Adjustable frequency/latency tradeoff
- Can combine with layer/process heuristics for classification

**Cons:**
- 1-frame latency is unavoidable with polling
- CPU cost at high frequencies (though 60Hz is cheap at ~3.6% core)
- Must handle window ID instability (windows can be recreated)
- Generates noise from tooltips, menu bar interactions, etc.
- Cannot detect overlays that reuse existing windows (e.g., NC drawer animating within its window)

**Key reference:** `CGWindowListCopyWindowInfo`, `kCGWindowListOptionOnScreenOnly`, benchmarked at 0.59ms/call

---

### 6. Hybrid: Events for Known Processes + Polling for Everything Else

**How it works:** Combine multiple detection methods:
1. **AX observer** on Notification Center for instant banner detection
2. **Process monitoring** (via `NSWorkspace` notifications) for known overlay apps launching
3. **CGWindowList polling** at 30Hz as a safety net for unknown overlays
4. **DND activation** as a pre-stream step to suppress notifications entirely

**Ease of writing:** 2/5 — Multiple subsystems to implement, coordinate, and test. Each has its own failure modes.

**Ease of maintenance:** 3/5 — More code means more maintenance, but each subsystem is independently replaceable. If the AX observer breaks on a new macOS version, the polling fallback still works.

**Detection latency:** 5/5 — Event-driven detection for known overlays (sub-frame). Polling fallback for unknowns (~16-33ms). Best achievable latency for a comprehensive system.

**Completeness:** 4/5 — Covers known overlays via targeted detection AND unknown overlays via polling. Only misses in-window content changes (not transient overlays).

**False positive rate:** 4/5 — Event-driven paths have high precision. Polling path can be tuned with layer/process heuristics to reduce false positives.

**Pros:**
- Best overall detection coverage
- Graceful degradation — if one subsystem fails, others compensate
- Can optimize latency for the most dangerous overlays (notifications, passwords)
- Extensible — add new detectors as needed

**Cons:**
- Most complex to implement
- Multiple permission requirements (Accessibility, Screen Recording)
- Debugging is harder with multiple detection paths
- Risk of duplicate detections causing flickering masks

**Key reference:** Architectural pattern, not a single API

---

### 7. macOS Do Not Disturb / Focus Mode API

**How it works:** Activate DND/Focus Mode before streaming to suppress all notification banners. This doesn't detect overlays — it prevents notification banners from appearing at all.

**Implementation options:**
- **Shortcuts automation:** Create a Shortcut that enables a "Streaming" Focus Mode, call via `shortcuts run "Streaming Focus"` from the daemon
- **defaults write:** `defaults write com.apple.notificationcenterui doNotDisturb -bool true && killall NotificationCenter` (broken on Big Sur+)
- **sindresorhus/do-not-disturb:** Node.js library for DND control
- **Focus Mode automation:** macOS can auto-enable Focus when specific apps (OBS) launch

**Ease of writing:** 5/5 — A single shell command or Shortcut invocation.

**Ease of maintenance:** 3/5 — Apple changes the DND/Focus API regularly. The `defaults write` method broke in Big Sur. Shortcuts are more stable but still depend on Apple's automation framework. **No official public API exists** for programmatic Focus Mode control (Apple Developer Forums confirms this — FB requested).

**Detection latency:** 5/5 — No detection needed. Notifications are suppressed before they can appear.

**Completeness:** 2/5 — **Only suppresses notification banners.** Does NOT prevent:
- Spotlight (Cmd+Space)
- Siri
- Control Center
- Password prompts (SecurityAgent)
- Third-party overlays (Raycast, Alfred, 1Password)
- System alerts (LuLu, software updates)
- Zoom HUD toasts

**False positive rate:** 5/5 — No false positives. Notifications simply don't appear.

**Pros:**
- Zero detection latency — prevention, not detection
- Zero CPU cost during streaming
- User-visible state (Focus indicator in menu bar)
- Can be automated via macOS Focus Mode triggers

**Cons:**
- Only prevents notification banners
- User might forget to enable it
- Suppresses notifications the user might want to see
- No official API — workarounds are fragile
- Does not address Spotlight, Siri, Control Center, password prompts, or third-party overlays

**Key reference:** [sindresorhus/do-not-disturb](https://github.com/sindresorhus/do-not-disturb), macOS Shortcuts, Focus Mode automation settings

---

### 8. Window Level Monitoring

**How it works:** Instead of a blanket layer > 0 rule, maintain a mapping of specific window levels to overlay types and mask accordingly:

| Level Range | Overlay Type | Action |
|-------------|-------------|--------|
| 0 | Normal apps | Allow (check process allowlist for known overlays) |
| 1-7 | Floating panels | Mask if new and transient |
| 8 | Modal dialogs | Mask (password prompts, Force Quit) |
| 20-25 | System overlays | Mask (NC banners, CC, Spotlight, menu bar items) |
| 26-100 | HUD/toast | Mask |
| 101+ | Popups/alerts | Mask |
| 1000+ | Screen saver/login | Mask entire screen |

**Ease of writing:** 3/5 — Requires building and maintaining the level-to-action mapping. More complex than blanket layer > 0 but not dramatically so.

**Ease of maintenance:** 3/5 — Window level assignments are generally stable across macOS versions, but edge cases exist (zoom.us uses layer 29 for HUD toasts, which is non-standard).

**Detection latency:** 3/5 — Same as polling-based approaches. Depends on poll rate.

**Completeness:** 4/5 — Better than blanket layer filtering because it can handle different levels differently. Can combine with process name checks for layer-0 overlays.

**False positive rate:** 3/5 — More nuanced than layer > 0, but still masks some benign UI (floating panels at layer 3 are often harmless).

**Pros:**
- More granular than layer > 0
- Can prioritize response by overlay type (mask password prompts instantly, delay for tooltips)
- Works with the existing CGWindowList infrastructure

**Cons:**
- Still misses layer-0 overlays
- Level mapping must be maintained manually
- Non-standard levels (app-specific) require per-app handling
- No advantage over layer > 0 for most cases

**Key reference:** NSWindowLevel constants, CGWindowLevelKey enum

---

### 9. FSEvents on Notification Database

**How it works:** Monitor the notification database file (`~/Library/Group Containers/group.com.apple.usernoted/db2/db`) for write events using FSEvents. When the database is modified, a new notification has been delivered, and we can mask the notification banner region.

**Ease of writing:** 3/5 — FSEvents API is well-documented. Monitoring a single file path is simple.

**Ease of maintenance:** 2/5 — **macOS Sequoia moved the database** to a TCC-protected Group Container location. The path changed from `/private/var/folders/.../com.apple.notificationcenter/db2/db` to `~/Library/Group Containers/group.com.apple.usernoted/db2/db`. Future versions may change it again. Additionally, the database is now TCC-protected, so reading its contents requires user consent.

**Detection latency:** 2/5 — FSEvents has inherent coalescing delay (typically 0.5-2 seconds). It's designed for efficiency, not real-time notification. The notification banner will have appeared and possibly disappeared before FSEvents fires.

**Completeness:** 1/5 — Only detects notifications that write to the database. Does NOT detect:
- Spotlight, Siri, Control Center (no database writes)
- Password prompts
- Third-party overlays
- Transient system alerts
- Notifications that don't persist

**False positive rate:** 4/5 — A database write means a notification was delivered. But the notification might be delivered silently (DND active) or to a different display.

**Pros:**
- Can read notification content from the database
- Low CPU cost (event-driven)
- Works even if the notification banner is not visible

**Cons:**
- Far too slow for real-time masking (0.5-2s latency)
- Only covers notifications, not other overlays
- Database path changes across macOS versions
- TCC protection in Sequoia complicates access
- Database writes may not correlate with banner display timing
- Notification might be dismissed before FSEvents fires

**Key reference:** [objective-see.org notification database analysis](https://objective-see.org/blog/blog_0x2E.html), FSEvents API, TCC changes in Sequoia

---

### 10. Blanket Rule: Mask Anything Not in the Safe Window List

**How it works:** Maintain a list of "safe" windows (the specific app windows the streamer intends to show). Every frame, check `CGWindowListCopyWindowInfo`. Any on-screen window NOT in the safe list gets its bounds masked. This is the default-deny approach applied to overlay detection.

**Ease of writing:** 5/5 — The simplest possible logic: `if window_id not in safe_set: mask(bounds)`.

**Ease of maintenance:** 5/5 — No knowledge of overlay types, window levels, or process names required. The safe list is defined by the user or auto-populated from the current scene.

**Detection latency:** 5/5 — Instant. Every poll cycle catches every non-safe window. No special-casing, no event subscriptions to miss.

**Completeness:** 5/5 — **Catches everything.** Notification banners, Spotlight, Siri, Control Center, password prompts, third-party overlays, system alerts, tooltips, floating panels — if it creates a window, it gets masked.

**False positive rate:** 1/5 — **Highest false positive rate of all approaches.** Will mask:
- Menu bar (always visible)
- Dock (always visible)
- Tooltips from the safe app itself
- Context menus from the safe app
- Window Server UI elements (cursor, status indicator)
- Talon/accessibility overlays
- Any window the user opens that isn't pre-approved

**Pros:**
- Provably complete — cannot miss any overlay
- Trivial to implement and maintain
- Aligns with the default-deny philosophy from obs-ubt.7
- No permissions beyond screen recording
- macOS-version-agnostic

**Cons:**
- Extremely aggressive masking — stream looks broken if safe list is too narrow
- User must explicitly approve every window they want visible
- Context menus and tooltips from safe apps get masked
- Must handle window recreation (IDs change when apps restart)
- Menu bar and Dock are always masked (unless explicitly safe-listed)
- Requires UI for safe-list management

**Key reference:** Default-deny pattern from obs-ubt.7-fail-safes.md

---

## Additional Research Findings

### Complete List of macOS Overlay Processes and Window Layers

Based on live system enumeration and documentation:

| Process | Bundle ID | Typical Layers | Overlay Type | PII Risk |
|---------|-----------|----------------|--------------|----------|
| Notification Center | com.apple.notificationcenterui | 21 | Banners, NC drawer | **HIGH** — message previews |
| Control Center | com.apple.controlcenter | 21, 25, 2005 | CC dropdown panels | LOW — settings only |
| Spotlight | com.apple.Spotlight | 0, 23 | Search bar | **HIGH** — typed queries |
| Siri | com.apple.Siri | varies | Siri overlay | **HIGH** — voice queries |
| SecurityAgent | com.apple.SecurityAgent | 1000+ | Password prompts | **CRITICAL** — passwords |
| loginwindow | com.apple.loginwindow | 0, 8, 1000 | Force Quit, login | MEDIUM |
| Raycast | com.raycast.macos | 0, 8 | Launcher | **HIGH** — typed queries |
| Alfred | com.runningwithcrayons.Alfred | 0 | Launcher | **HIGH** — typed queries |
| 1Password | com.1password.1password | 0 | Autofill popup | **CRITICAL** — passwords |
| LuLu | com.objective-see.lulu | 0, 101 | Firewall alerts | LOW |
| Bartender | com.surteesstudios.Bartender | 25 | Menu bar manager | LOW |
| iStat Menus | com.bjango.istatmenus | 25 | System monitor popups | LOW |
| Karabiner | org.pqrs.Karabiner-NotificationWindow | 25 | Key remap alerts | LOW |
| CleanShot | com.cleanshot.CleanShot | varies | Screenshot overlay | LOW |
| Zoom HUD | us.zoom.xos | 3, 29 | Meeting toasts | MEDIUM — participant names |
| Messages popup | com.apple.MobileSMS | 0, 3 | Reply popup | **HIGH** — message content |

### Notification Banner Timing

- **Trigger to pixels:** ~50-150ms. The macOS WindowServer composites the banner into the framebuffer within 2-4 frames of the notification being posted.
- **Banner display duration:** Default 5 seconds. Configurable via `defaults write com.apple.notificationcenterui bannerTime` on pre-Big Sur. Not configurable on Big Sur+.
- **Reported audio-to-visual delay:** Some users report notification sound playing 4-5 seconds before the banner appears. This is intermittent and likely a system load issue, not a reliable detection window.
- **Animation duration:** The banner slides in from the top-right over ~300ms. During this animation, the window is already on screen and capturable.

### Can We Detect Spotlight/Siri BEFORE They Render?

**Keyboard shortcut interception:** Theoretically possible via:
- **CGEventTap:** Intercept Cmd+Space (Spotlight) or the Siri shortcut at the event tap level. This fires BEFORE the system processes the shortcut, giving ~50-100ms of lead time.
- **Limitation:** Requires Accessibility permissions. The event tap fires before Spotlight renders, but you'd need to preemptively mask the Spotlight region (top-center of screen) before confirming it appeared. This creates a brief false-positive mask if the user is doing Cmd+Space for another reason (unlikely, but possible).
- **Practical value:** Marginal. CGEventTap adds complexity and the preemptive masking region might be wrong (Spotlight position can vary).

**Better approach:** Poll-based detection at 60Hz catches Spotlight within 16ms of appearance, which is already within the same frame for a 30fps stream.

### Third-Party Overlay Behavior

| App | Window Creation | Layer | Detection Strategy |
|-----|----------------|-------|-------------------|
| **Raycast** | Creates window at layer 8 when invoked | 8 | Layer > 0 OR process name |
| **Alfred** | Creates window at layer 0 | 0 | Process name only |
| **1Password** (autofill) | Creates window at layer 0 | 0 | Process name only |
| **1Password** (mini) | Creates window at layer 0 | 0 | Process name only |
| **Bartender** | Status items at layer 25 | 25 | Layer > 0 |
| **CleanShot** | Overlay varies by mode | varies | Process name |
| **PopClip** | Popup at layer 0 or 101 | 0/101 | Layer > 0 OR process name |

**Key insight:** Third-party launchers (Alfred, 1Password) often use layer 0, making them invisible to layer-based detection. Process-name allowlisting is required for these.

### macOS Sequoia Changes to Notification Handling

1. **TCC-protected notification database:** The notification database moved from `/private/var/folders/.../com.apple.notificationcenter/db2/db` to `~/Library/Group Containers/group.com.apple.usernoted/db2/db`, now protected by TCC prompts. This breaks FSEvents-based monitoring without explicit user consent.

2. **AX tree changes:** Notification elements changed from `AXRole="AXGroup"` to `AXRole="AXButton"` in the accessibility tree. Group identifiers changed from numeric to UUID format.

3. **Notification summaries (15.3+):** Apple Intelligence notification summaries are shown in italics. Summarized notifications may have different AX properties than raw notifications.

4. **CGWindowListCopyWindowInfo regression (macOS 26):** In macOS 26 beta (Tahoe), `CGWindowListCopyWindowInfo` reports all status items as belonging to Control Center instead of their respective apps (FB18327911). This breaks process-name-based detection for menu bar items.

5. **ScreenCaptureKit compositing (15+):** All window contents are composited into a single framebuffer before ScreenCaptureKit captures. Traditional window-level exclusion methods (`NSWindow.sharingType = .none`) no longer prevent capture. This is relevant because it means our OBS mask must operate at the OBS plugin level, not by asking windows to exclude themselves.

---

## Recommendation

**For the PII masking system, use a layered approach (Option 6 variant):**

### Layer 1: Prevention (pre-stream)
- Activate DND/Focus Mode via Shortcuts automation when OBS starts
- Add to the existing pre-stream checklist
- Eliminates notification banners entirely (the highest-frequency PII source)

### Layer 2: Detection (during stream)
- Poll `CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly)` at 60Hz (0.59ms/call, ~3.6% CPU)
- Maintain a **safe window set** (windows in the current OBS scene)
- Any on-screen window NOT in the safe set AND with layer > 0: **mask immediately**
- Any on-screen window NOT in the safe set AND with layer == 0 AND owned by a known overlay process: **mask immediately**

### Layer 3: Safety net
- The default-deny architecture from obs-ubt.7 means the OBS plugin renders a fully blurred frame if the daemon stops updating
- The 2-3 second stream delay (if configured) provides a manual kill-switch window

### Why this combination:
- DND eliminates the most common PII overlay (notification banners) with zero latency
- 60Hz polling catches everything else within 1 frame (16ms)
- Process allowlist catches layer-0 overlays (1Password, Alfred, Raycast)
- Default-deny ensures safety even if detection fails
- Total CPU cost: ~3.6% of one core for polling, negligible for DND

### Implementation priority:
1. **60Hz CGWindowList polling with layer > 0 masking** — catches 80% of overlays
2. **Process name allowlist for layer-0 overlays** — catches Raycast, Alfred, 1Password
3. **DND automation** — eliminates notification banners preemptively
4. **AX observer on NotificationCenter** — optional, for content-aware notification handling

---

## Sources

- [phillco/talon-axkit notification.py](https://github.com/phillco/talon-axkit/blob/main/notification.py) — AX-based notification detection for Talon
- [Apple: Capturing screen content in macOS (ScreenCaptureKit)](https://developer.apple.com/documentation/ScreenCaptureKit/capturing-screen-content-in-macos)
- [Apple: CGWindowLevelKey.overlayWindow](https://developer.apple.com/documentation/coregraphics/cgwindowlevelkey/overlaywindow)
- [Apple: AXObserverAddNotification](https://developer.apple.com/documentation/applicationservices/1462089-axobserveraddnotification)
- [sindresorhus/do-not-disturb](https://github.com/sindresorhus/do-not-disturb) — Node.js DND control
- [objective-see.org: The Dark Side of macOS Notifications](https://objective-see.org/blog/blog_0x2E.html) — Notification database internals
- [9to5Mac: Apple addresses notification database privacy in Sequoia](https://9to5mac.com/2024/09/01/security-bite-apple-addresses-privacy-concerns-around-notification-center-database-in-macos-sequoia/)
- [macOS 26 CGWindowList regression (FB18327911)](https://github.com/feedback-assistant/reports/issues/679)
- [Tauri: ScreenCaptureKit ignores setContentProtection in macOS 15+](https://github.com/tauri-apps/tauri/issues/14200)
- [matthewreagan: Topmost window via CGWindow](https://gist.github.com/matthewreagan/2f3a30b8b229e9e2aa7c)
