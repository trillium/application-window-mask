# Window Safety Classifier: Implementation Options

Research into viable approaches for a classifier that takes window metadata and returns
safe/unsafe for PII masking purposes.

## Available Metadata from CGWindowListCopyWindowInfo

Every window returned by `CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID)` provides:

| Key | Type | Example | Classification Use |
|-----|------|---------|-------------------|
| `kCGWindowOwnerName` | string | `"Google Chrome"` | Primary app identification |
| `kCGWindowOwnerPID` | int | `1234` | Process correlation |
| `kCGWindowName` | string | `"Gmail - Inbox"` | Per-tab/per-document granularity |
| `kCGWindowNumber` | int | `4567` | Unique window tracking |
| `kCGWindowLayer` | int | `0` (normal), `20+` (overlay) | Popup/overlay detection |
| `kCGWindowBounds` | dict | `{X:0, Y:0, Width:1728, Height:1117}` | Mask geometry |
| `kCGWindowIsOnscreen` | bool | `true` | Filter off-screen windows |
| `kCGWindowAlpha` | float | `1.0` | Detect transparent windows |

**Not directly available from CGWindowList but obtainable:**

| Field | Source | Cost |
|-------|--------|------|
| Bundle ID (`com.google.Chrome`) | `NSRunningApplication` via PID | ~0.1ms per lookup, cacheable |
| Window subrole (`AXDialog`, `AXSheet`) | AXUIElement query | 5-50ms, expensive |
| Talon `ui.App.bundle` | Talon shim event payload | Free if Talon is sending events |

Bundle ID is the most reliable identifier since display names can change across locales
or updates. A PID-to-bundle-ID cache makes this effectively free after first lookup.

---

## Option 1: Static Allowlist (Current Approach)

**How it works:** Hardcoded Python set of safe app names. `if owner_name in SAFE_APPS: reveal()`.

| Dimension | Score | Notes |
|-----------|-------|-------|
| Ease of writing | 5 | A literal set in Python |
| Ease of maintenance | 1 | Code change + restart for every rule change |
| Type safety | 3 | No schema, but simple enough to be correct |
| Performance | 5 | O(1) set lookup |
| Flexibility | 1 | All-or-nothing per app, no title/layer rules |

**Pros:**
- Zero dependencies, zero config parsing
- Impossible to misconfigure (it either compiles or it doesn't)
- Fastest possible lookup

**Cons:**
- Any rule change requires editing code and restarting the daemon
- Cannot handle "Chrome is safe but Chrome showing Gmail is not"
- Cannot handle per-window-title overrides (R10)
- Locale-dependent: display names differ across macOS languages

**Key reference:** Current architecture already specifies this as the starting point in ARCHITECTURE.md.

---

## Option 2: TOML Config File

**How it works:** Rules defined in a `.toml` file. Parsed with `tomllib` (stdlib 3.11+).
Reloaded on SIGHUP or file-change notification.

```toml
[defaults]
policy = "unsafe"  # default-unsafe: unknown apps are masked

[apps.safe]
# Bundle ID is primary key, display name as fallback
"com.apple.finder" = { name = "Finder" }
"com.googlecode.iterm2" = { name = "iTerm2" }
"com.obsproject.obs-studio" = { name = "OBS" }

[apps.unsafe]
# Explicitly mark even if default is unsafe (documentation)
"com.apple.MobileSMS" = { name = "Messages" }

[title_overrides]
# Per-window-title rules that override app-level classification
[[title_overrides.rules]]
app = "com.google.Chrome"
title_pattern = "Gmail|Messenger|WhatsApp"
policy = "unsafe"

[layers]
# Windows above this layer are always masked (popups, notifications)
mask_above_layer = 0
# Exception: these apps are safe even as overlays
safe_overlay_apps = ["com.obsproject.obs-studio"]
```

| Dimension | Score | Notes |
|-----------|-------|-------|
| Ease of writing | 4 | tomllib is stdlib, schema is simple |
| Ease of maintenance | 4 | Edit a file, send SIGHUP; no restart |
| Type safety | 3 | No built-in schema validation; need manual checks |
| Performance | 5 | Parse once, dict lookup at runtime |
| Flexibility | 4 | App-level + title patterns + layer rules |

**Pros:**
- Python stdlib (`tomllib`), zero extra dependencies
- Already specified in ARCHITECTURE.md as the intended approach
- Human-readable, easy to hand-edit
- Composable: defaults + app rules + title overrides + layer rules
- SIGHUP reload is a Unix convention, trivial to implement

**Cons:**
- TOML's nested table syntax can be confusing for complex rule hierarchies
- No native regex support in the format (patterns are just strings, interpreted by Python)
- `tomllib` is read-only; programmatic rule updates require a separate writer
- No schema validation without a third-party library (e.g., `pydantic`)

**Key reference:** `tomllib` docs: https://docs.python.org/3/library/tomllib.html

---

## Option 3: YAML Config

**How it works:** Same rule structure as TOML but in YAML. Requires `PyYAML` or `ruamel.yaml`.

```yaml
defaults:
  policy: unsafe

apps:
  safe:
    com.apple.finder: { name: Finder }
    com.googlecode.iterm2: { name: iTerm2 }
  unsafe:
    com.apple.MobileSMS: { name: Messages }

title_overrides:
  - app: com.google.Chrome
    title_pattern: "Gmail|Messenger"
    policy: unsafe

layers:
  mask_above_layer: 0
```

| Dimension | Score | Notes |
|-----------|-------|-------|
| Ease of writing | 3 | Extra dependency, YAML gotchas (Norway problem, etc.) |
| Ease of maintenance | 4 | Same as TOML for editing |
| Type safety | 2 | Implicit typing leads to surprises (`on` -> `True`) |
| Performance | 5 | Parse once, dict lookup at runtime |
| Flexibility | 4 | Same as TOML |

**Pros:**
- More natural nesting syntax than TOML for deep hierarchies
- Widely known format
- Supports comments (unlike JSON)

**Cons:**
- Requires `PyYAML` or `ruamel.yaml` (external dependency)
- YAML's implicit typing causes bugs: `on: true` instead of string `"on"`, version `3.10` becomes float `3.1`
- Adds a dependency we don't need when TOML does the same job with stdlib
- Security: `yaml.safe_load()` is required; `yaml.load()` allows arbitrary code execution

**Key reference:** PyYAML docs: https://pyyaml.org/wiki/PyYAMLDocumentation

---

## Option 4: JSON Config

**How it works:** Rules in a JSON file. Parsed with `json` (stdlib).

| Dimension | Score | Notes |
|-----------|-------|-------|
| Ease of writing | 4 | stdlib, trivial to parse |
| Ease of maintenance | 2 | No comments allowed; hard to hand-edit |
| Type safety | 3 | Strict typing but no schema without JSON Schema |
| Performance | 5 | Parse once, dict lookup at runtime |
| Flexibility | 4 | Same expressiveness as TOML/YAML |

**Pros:**
- stdlib, universal format
- Strict types (no YAML "Norway problem")
- Every editor has JSON support

**Cons:**
- No comments -- a config file without comments is hostile to future maintainers
- Trailing commas are syntax errors
- Verbose compared to TOML for simple key-value config
- Not pleasant to hand-edit for a config that changes occasionally

**Key reference:** `json` stdlib.

---

## Option 5: SQLite Database

**How it works:** Rules stored in a SQLite database. Queried at classification time.

```sql
CREATE TABLE app_rules (
    bundle_id TEXT PRIMARY KEY,
    display_name TEXT,
    policy TEXT CHECK(policy IN ('safe', 'unsafe'))
);
CREATE TABLE title_rules (
    bundle_id TEXT,
    title_pattern TEXT,
    policy TEXT,
    priority INTEGER DEFAULT 0
);
```

| Dimension | Score | Notes |
|-----------|-------|-------|
| Ease of writing | 2 | Schema design, migrations, query layer needed |
| Ease of maintenance | 3 | Easy to update rules programmatically; hard to hand-edit |
| Type safety | 4 | Schema constraints enforce valid data |
| Performance | 4 | Fast for lookups but slower than in-memory dict |
| Flexibility | 5 | Full SQL expressiveness, easy to add new rule types |

**Pros:**
- Atomic updates (ACID transactions)
- Easy to build a UI or CLI tool for rule management
- Can store audit logs (when was this rule added/changed?)
- Handles complex queries naturally (e.g., "all rules matching Chrome with priority > 2")
- `sqlite3` is stdlib

**Cons:**
- Massive over-engineering for ~20-50 rules
- Cannot be hand-edited easily (need a tool)
- Schema migrations needed as rule model evolves
- Adds conceptual overhead for contributors
- File locking can complicate hot-reload

**Key reference:** `sqlite3` stdlib, but this is overkill for this use case.

---

## Option 6: Rule Engine with Patterns

**How it works:** Rules are a list of pattern-match predicates evaluated in priority order.
Each rule has a matcher (regex/glob on app name, title, bundle ID) and an action (safe/unsafe).

```toml
[[rules]]
priority = 10
match = { owner_name = "Finder" }
policy = "safe"

[[rules]]
priority = 20
match = { bundle_id = "com.google.Chrome", title = ".*Gmail.*" }
policy = "unsafe"

[[rules]]
priority = 5
match = { layer = ">0" }
policy = "unsafe"
```

Evaluation: sort rules by priority descending. First match wins.

| Dimension | Score | Notes |
|-----------|-------|-------|
| Ease of writing | 3 | Rule engine logic is moderate complexity |
| Ease of maintenance | 4 | Very clear rule semantics; easy to add new rules |
| Type safety | 3 | Depends on validation of match predicates |
| Performance | 4 | Linear scan of rules per window; fast enough for ~50 rules |
| Flexibility | 5 | Any combination of fields, regex, comparisons |

**Pros:**
- Extremely flexible: combine any metadata fields
- Priority ordering makes conflict resolution explicit
- Easy to reason about: "why was this window masked?" -> show matching rule
- Can log which rule matched (great for debugging)

**Cons:**
- More complex to implement than a simple dict lookup
- Rule ordering bugs (two rules at same priority with conflicting actions)
- Regex compilation needs to happen at config load, not per-window
- Harder to understand at a glance than a simple safe-list

**Key reference:** This is essentially a firewall rule model (iptables/pf style).

---

## Option 7: Layered Rules (App Default + Per-Title Overrides)

**How it works:** Two-tier system. App-level default (safe/unsafe) plus per-window-title
overrides that take precedence.

```toml
[defaults]
policy = "unsafe"

[apps."com.google.Chrome"]
policy = "safe"
[[apps."com.google.Chrome".overrides]]
title_pattern = "Gmail|Facebook Messenger"
policy = "unsafe"
[[apps."com.google.Chrome".overrides]]
title_pattern = "^New Tab$"
policy = "safe"

[apps."com.apple.finder"]
policy = "safe"

[apps."com.apple.MobileSMS"]
policy = "unsafe"
```

Evaluation order:
1. Check if any title override matches -> use override policy
2. Else -> use app-level policy
3. Else -> use default policy

| Dimension | Score | Notes |
|-----------|-------|-------|
| Ease of writing | 4 | Clean two-tier model, straightforward |
| Ease of maintenance | 5 | Intuitive: "Chrome is safe, except these titles" |
| Type safety | 3 | Same as TOML config |
| Performance | 5 | Dict lookup for app, then linear scan of few overrides |
| Flexibility | 4 | Covers 95% of use cases; less flexible than full rule engine |

**Pros:**
- Most intuitive mental model for users: "this app is safe, except when..."
- Directly addresses requirement R10 (per-window-title granularity)
- Fast: O(1) app lookup + O(n) override scan where n is small (1-5 per app)
- Easy to explain to non-technical users

**Cons:**
- Only two tiers; cannot express "Chrome Gmail is unsafe, but Chrome Gmail Compose is safe"
- Title patterns are the only override dimension (cannot override by window layer)
- Slightly less flexible than a full rule engine

**Key reference:** This is the model used by browser content blockers (uBlock Origin rules).

---

## Option 8: ML-Based Classification

**How it works:** Feed window title text (and optionally OCR'd content) into a text
classifier that predicts PII risk. Could be a local model (Core ML, ONNX) or heuristic
(keyword matching).

| Dimension | Score | Notes |
|-----------|-------|-------|
| Ease of writing | 1 | Training data, model selection, inference pipeline |
| Ease of maintenance | 1 | Model drift, retraining, false positive tuning |
| Type safety | 2 | Probabilistic output, threshold tuning |
| Performance | 2 | 10-100ms per inference even with Core ML |
| Flexibility | 5 | Can learn arbitrary patterns |

**Pros:**
- Could catch PII risks that static rules miss (e.g., a new messaging app)
- No manual rule maintenance for new apps

**Cons:**
- Massive over-engineering for a ~50-rule classification problem
- Inference latency (10-100ms) blows the 5ms mask budget
- False negatives are catastrophic (PII leak on stream)
- False positives are annoying (safe content masked)
- Training data is specific to one user's workflow -- not generalizable
- Adds heavyweight dependencies (Core ML, ONNX, etc.)
- A safety system should be deterministic, not probabilistic

**Verdict: Not viable.** The cost/benefit ratio is terrible. Static rules with a default-unsafe
posture provide better safety guarantees with 1/100th the complexity.

**Key reference:** N/A -- this is an anti-pattern for this use case.

---

## Option 9: Bundle ID Matching

**How it works:** Same as static allowlist (Option 1) but keyed on macOS bundle identifiers
(`com.google.Chrome`) instead of display names.

| Dimension | Score | Notes |
|-----------|-------|-------|
| Ease of writing | 5 | Same as static allowlist |
| Ease of maintenance | 2 | Still hardcoded; but IDs are stable |
| Type safety | 3 | Same as static allowlist |
| Performance | 5 | O(1) dict lookup |
| Flexibility | 1 | All-or-nothing per app |

**Pros:**
- Bundle IDs are locale-independent (`com.apple.finder` regardless of language)
- Bundle IDs are stable across app updates (display name can change)
- Unique per app (no collisions between "Mail" from different vendors)
- Available via `NSRunningApplication(withProcessIdentifier:)` from PID

**Cons:**
- Same rigidity as static allowlist (no title overrides, no layered rules)
- Requires PID-to-bundle-ID resolution (not in CGWindowList directly)
- Some system processes have no bundle ID (e.g., `loginwindow`, `SystemUIServer`)
- Menu bar extras and helper apps may have unexpected bundle IDs

**Key reference:** Bundle IDs available via `NSRunningApplication.runningApplication(withProcessIdentifier:)`

---

## Option 10: Hybrid (Bundle ID + Title Patterns + Layer Rules) -- RECOMMENDED

**How it works:** Combines the best elements:
- **Bundle ID** as primary key (stable, locale-independent)
- **Display name** as fallback for processes without bundle IDs
- **Title patterns** for per-window granularity (browser tabs, document names)
- **Layer rules** for popup/overlay detection
- **TOML config** for human-editable, reloadable rules

```toml
[defaults]
policy = "unsafe"           # Unknown apps are masked
mask_above_layer = 0        # Overlays always masked (with exceptions)

# App rules keyed by bundle ID
[apps."com.apple.finder"]
policy = "safe"

[apps."com.google.Chrome"]
policy = "safe"
[[apps."com.google.Chrome".title_overrides]]
pattern = "Gmail|Messenger|WhatsApp|Signal"
policy = "unsafe"

[apps."com.googlecode.iterm2"]
policy = "safe"

[apps."com.obsproject.obs-studio"]
policy = "safe"
safe_as_overlay = true      # OBS preview windows are safe even on overlay layers

# Fallback rules for apps without bundle IDs (system processes)
[apps_by_name."Window Server"]
policy = "safe"

[apps_by_name."Dock"]
policy = "safe"
```

**Classification algorithm:**
```
classify(window) -> safe | unsafe:
  1. If window.layer > mask_above_layer:
     - Check if app has safe_as_overlay = true -> safe
     - Else -> unsafe (overlay/popup)
  2. Look up app by bundle_id in apps table:
     - If found, check title_overrides (regex match on window.title)
       - If title override matches -> use override policy
       - Else -> use app policy
  3. Look up app by owner_name in apps_by_name table (fallback)
  4. Return defaults.policy (unsafe)
```

| Dimension | Score | Notes |
|-----------|-------|-------|
| Ease of writing | 3 | Most complex option, but well-bounded |
| Ease of maintenance | 5 | Edit TOML, SIGHUP; covers all scenarios |
| Type safety | 3 | Needs validation at load time |
| Performance | 5 | Dict + small linear scan; sub-0.1ms |
| Flexibility | 5 | Handles every known scenario |

**Pros:**
- Handles all requirements: R2 (app masking), R3 (notifications), R6 (overlays), R9 (runtime config), R10 (per-title)
- Default-unsafe means new/unknown apps are automatically protected
- Bundle IDs resist locale changes and app renames
- Layer rules catch all popups/overlays without enumerating them
- Title patterns handle the browser-tab problem
- TOML is stdlib, human-readable, supports comments
- Classification algorithm is deterministic and auditable

**Cons:**
- Most complex to implement (~150-200 lines vs ~20 for static allowlist)
- Three lookup tiers (bundle ID, display name, default) could confuse contributors
- Regex compilation at load time adds startup cost (~1ms, negligible)
- Need to build and maintain the PID-to-bundle-ID cache

**Key reference:** This is the architecture already implied by ARCHITECTURE.md sections 2 and 5.

---

## Comparison Matrix

| Option | Writing | Maintenance | Type Safety | Performance | Flexibility | Total |
|--------|---------|-------------|-------------|-------------|-------------|-------|
| 1. Static allowlist | 5 | 1 | 3 | 5 | 1 | 15 |
| 2. TOML config | 4 | 4 | 3 | 5 | 4 | 20 |
| 3. YAML config | 3 | 4 | 2 | 5 | 4 | 18 |
| 4. JSON config | 4 | 2 | 3 | 5 | 4 | 18 |
| 5. SQLite database | 2 | 3 | 4 | 4 | 5 | 18 |
| 6. Rule engine | 3 | 4 | 3 | 4 | 5 | 19 |
| 7. Layered rules | 4 | 5 | 3 | 5 | 4 | 21 |
| 8. ML classifier | 1 | 1 | 2 | 2 | 5 | 11 |
| 9. Bundle ID match | 5 | 2 | 3 | 5 | 1 | 16 |
| **10. Hybrid** | **3** | **5** | **3** | **5** | **5** | **21** |

---

## Cross-Cutting Concerns

### Default-Unsafe vs Default-Safe

**Default-unsafe is the only correct choice.** Rationale:

- **False negative (PII leaked on stream) is catastrophic.** There is no undo. The damage is
  immediate and permanent once viewers see it.
- **False positive (safe content unnecessarily masked) is merely annoying.** The streamer
  sees a white rectangle where their terminal should be, realizes the app isn't in the
  safe list, and adds it.
- The failure mode asymmetry is extreme: ~100x worse to leak PII than to over-mask.
- Default-unsafe means a new app install is automatically protected without action.
- This matches the architecture's "white canvas" approach: start fully masked, reveal only
  what is explicitly classified safe.

**Implementation:** The classifier returns `unsafe` for any window whose app is not found in
the config. The TOML config has `policy = "unsafe"` as the default.

### Handling Apps That Are Sometimes Safe, Sometimes Not

The primary case is **web browsers** where different tabs show different content.

**Approach: title_overrides with regex patterns.**

```toml
[apps."com.google.Chrome"]
policy = "safe"                    # Chrome is generally safe (docs, code, etc.)
[[apps."com.google.Chrome".title_overrides]]
pattern = "Gmail|Messenger|WhatsApp|Signal|Facebook|Instagram|Twitter|X\\.com|Slack DM"
policy = "unsafe"                  # These tabs contain PII
```

**Edge cases to handle:**
- **Empty window title:** Some browsers report empty `kCGWindowName` for background tabs.
  Since the visible tab is in the title bar, the frontmost tab always has a title. Empty
  title -> use app-level default.
- **Multiple browser windows:** Each window has its own title. The CG poller returns all
  on-screen windows, so each browser window is classified independently.
- **Private/incognito mode:** Window title often includes "[Incognito]" or "Private Browsing".
  Could add a rule to mask these.
- **Electron apps** (Slack, Discord, VS Code): These have their own bundle IDs distinct from
  the browser, so they get their own rules. No conflict.

**Alternative considered: content-based classification.** OCR the window content and look for
PII patterns (email addresses, phone numbers). Rejected because: (a) OCR adds 50-200ms
latency, (b) requires Screen Recording to capture pixels, (c) probabilistic, and (d) the
title-based approach catches 95% of cases with zero latency.

### Hot-Reload Mechanisms

Three viable options, not mutually exclusive:

| Mechanism | How | Latency | Complexity |
|-----------|-----|---------|------------|
| **SIGHUP** | `kill -HUP <pid>`; daemon re-reads config | Instant | Low (signal handler) |
| **File watcher** | `kqueue`/`FSEvents` on config file | ~100ms | Medium (`watchdog` lib or `select.kqueue`) |
| **Socket command** | `echo "reload" | nc -U /tmp/pii_mask.sock` | Instant | Low (already have socket server) |

**Recommendation: SIGHUP + socket command.**
- SIGHUP is the Unix convention for "re-read config." One line: `signal.signal(signal.SIGHUP, reload_config)`.
- Socket command integrates with the existing IPC channel from the Talon shim.
- File watcher adds a dependency (`watchdog`) or platform-specific code for marginal benefit.
  The config changes rarely enough that manual reload is fine.

**Implementation sketch:**
```python
import signal, tomllib

def load_config(path: str) -> dict:
    with open(path, "rb") as f:
        return tomllib.load(f)

def reload_config(signum=None, frame=None):
    global classifier_rules
    classifier_rules = load_config(CONFIG_PATH)
    log.info("Config reloaded: %d app rules", len(classifier_rules.get("apps", {})))

signal.signal(signal.SIGHUP, reload_config)
```

### Temporary Overrides ("Trust This Window for 5 Minutes")

**Use case:** Streamer needs to briefly show an app that is normally masked (e.g., read a
notification, check Messages for a non-sensitive thread).

**Options evaluated:**

1. **Socket command with TTL:**
   ```
   {"cmd": "trust", "bundle_id": "com.apple.MobileSMS", "ttl_seconds": 300}
   ```
   Daemon adds a temporary override with an expiry timestamp. Checked before config rules.
   Timer fires and removes the override automatically.

2. **Voice command via Talon:**
   ```
   "trust this window" -> send trust command for focused app
   "trust this window for five minutes" -> with TTL
   "untrust this window" -> immediate revoke
   ```

3. **OBS hotkey:** Toggle mask for current window via OBS websocket -> daemon.

**Recommendation:** Socket command with TTL, triggered by Talon voice command. This is the
most natural UX for a streamer who is live and cannot reach for a keyboard.

**Safety rails:**
- Maximum TTL of 10 minutes (cannot accidentally leave a window trusted)
- Temporary overrides are NOT persisted to disk (lost on daemon restart)
- Log every trust/untrust event for post-stream audit
- Visual indicator in OBS (optional): overlay text showing "Messages trusted for 4:32"

### CGWindowListCopyWindowInfo Metadata Deep Dive

Full list of keys available in the returned dictionaries:

```
kCGWindowNumber          -> int     (unique window ID, stable for window lifetime)
kCGWindowStoreType       -> int     (1=retained, 2=nonretained, 3=buffered)
kCGWindowLayer           -> int     (z-layer: 0=normal, 20+=overlay, 25=screensaver, etc.)
kCGWindowBounds          -> dict    (X, Y, Width, Height as floats -- logical pixels)
kCGWindowSharingState    -> int     (1=none, 2=read-only, 3=read-write)
kCGWindowAlpha           -> float   (0.0 to 1.0)
kCGWindowOwnerPID        -> int     (process ID)
kCGWindowOwnerName       -> string  (process display name)
kCGWindowName            -> string  (window title -- MAY BE EMPTY or nil)
kCGWindowIsOnscreen      -> bool    (true if visible)
kCGWindowMemoryUsage     -> int     (bytes, only with kCGWindowListOptionIncludingWindow)
```

**Important notes:**
- `kCGWindowName` requires Screen Recording permission. Without it, all titles are empty.
- `kCGWindowBounds` is in logical (non-Retina) coordinates. For a Retina display showing
  1728x1117 logical, physical pixels are 3456x2234. Mask must match OBS capture resolution.
- Windows are returned in front-to-back z-order within each layer. Layer ordering is:
  desktop (layer -2147483648) < normal (0) < overlays (3-25) < cursor (2147483647).
- `kCGWindowOwnerName` is the process name, not the app display name. Usually the same but
  not guaranteed (e.g., helper processes).

**Layer values observed on macOS Sonoma/Sequoia:**

| Layer | Window Type |
|-------|------------|
| -2147483648 | Desktop (Finder background) |
| 0 | Normal application windows |
| 3 | Dock |
| 8 | Menu bar items / status menus |
| 20 | Notification banners |
| 24 | Spotlight |
| 25 | Screen saver, lock screen |
| 2147483647 | Cursor, other system overlays |

---

## Recommendation

**Option 10 (Hybrid)** with TOML config, implemented in phases:

**Phase 1 (MVP):** Bundle ID + display name allowlist in TOML. Default-unsafe. No title
overrides. SIGHUP reload. This covers R2, R5, R9 and is ~80 lines of Python.

**Phase 2:** Add `title_overrides` for browser tabs. Add layer rules for overlay detection.
This covers R3, R6, R10. Adds ~60 lines.

**Phase 3:** Add socket-based temporary overrides with TTL. Talon voice commands for trust/
untrust. Adds ~40 lines.

The layered approach means Phase 1 ships fast and delivers immediate value, while Phases 2-3
add granularity without changing the core architecture.
