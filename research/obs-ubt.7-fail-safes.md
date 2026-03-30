# Fail-Safe Mechanisms for PII Protection During Streaming

> Research date: 2026-03-29
> Context: OBS plugin reading mask geometry from shared memory, with a Python daemon writing mask data.
> Goal: Guarantee that if ANY component fails, the stream shows a fully masked/blurred screen — never raw PII.

---

## Design Principle: Default-Deny

Every approach below should be evaluated against the core invariant:

> **If the system cannot prove a region is safe, it MUST be masked.**

This is analogous to firewall design (default-deny) and broadcast standards (fail to black/bars). The stream output must be safe at rest — safety is the ground state, not an active process.

---

## Option Comparison Matrix

| # | Approach | Ease of Writing | Ease of Maintenance | Reliability | Latency to Safe | Best For |
|---|----------|:-:|:-:|:-:|---|---|
| 1 | Default-white canvas | 5 | 5 | 5 | 0ms (already safe) | Foundation layer |
| 2 | Heartbeat/watchdog | 3 | 4 | 4 | 1 frame (16ms) | Daemon crash detection |
| 3 | Sequence counter staleness | 4 | 4 | 4 | 1 frame (16ms) | Daemon hang detection |
| 4 | OBS scene fallback | 3 | 3 | 3 | 100-300ms | Catastrophic failure |
| 5 | Pre-mask on switch | 4 | 4 | 5 | 0ms (mask first) | Window transitions |
| 6 | Dual-layer approach | 3 | 3 | 5 | 0ms (always masked) | Belt-and-suspenders |
| 7 | Stream delay | 2 | 2 | 3 | 2-3s budget | Last resort |
| 8 | Process supervisor | 4 | 5 | 4 | 1-5s (restart time) | Daemon crashes |
| 9 | Plugin self-test on startup | 4 | 5 | 4 | N/A (startup gate) | Initialization |
| 10 | Graceful degradation ladder | 2 | 2 | 5 | Varies by level | Full system design |

---

## Detailed Analysis

### 1. Default-White Canvas

**How it works:** The OBS plugin's `video_render()` starts every frame with a fully opaque white/blurred output. It only "punches holes" (reveals original pixels) for regions explicitly marked safe in the shared memory data. If the shm is missing, stale, or corrupt, the plugin renders the fully blurred frame — because no safe regions were read.

**Ease of writing:** 5/5 — This is the simplest approach. The plugin's default code path is "blur everything." Revealing safe regions is the opt-in behavior.

**Ease of maintenance:** 5/5 — No moving parts, no timers, no external dependencies. The safety guarantee is structural, not behavioral.

**Reliability:** 5/5 — Cannot fail. There is no code path that produces an unmasked frame without explicit safe-region data. Even if the plugin has a bug in region parsing, the worst case is "too much masking" (false positive), never "too little" (false negative).

**Latency to safe state:** 0ms — The system is already in the safe state. It is never NOT safe.

**Pros:**
- Zero-latency fail-safe — safety is the resting state
- No race conditions — there is no window between "detecting failure" and "applying mask"
- Simple mental model — developers can reason about correctness easily
- Composes with every other approach (they're all additive)

**Cons:**
- Only protects against "no data" or "corrupt data" scenarios — doesn't help if the daemon sends wrong safe regions (e.g., marks iMessage as safe)
- Requires the blur to be computed every frame regardless (minor GPU cost)

**Key reference:** This is the pattern used in the existing architecture (ARCHITECTURE.md section 6: "The compositor starts with a fully white (masked) canvas"). Also the principle behind OpenBSD's `pledge()` — restrict first, allow specifically.

**Implementation sketch:**
```c
void pii_mask_video_render(void *data, gs_effect_t *effect) {
    struct pii_filter *f = data;

    // 1. Render source to texture
    // 2. Apply blur to entire frame -> blurred_tex
    // 3. Start with blurred output (DEFAULT SAFE)
    // 4. Only if shm valid AND regions present:
    //    punch through to original for safe regions
    // 5. If anything fails at steps 3-4, output is fully blurred
}
```

---

### 2. Heartbeat / Watchdog

**How it works:** The daemon writes a timestamp (e.g., `uint64_t heartbeat_ns`) into a known offset in shared memory on every compositor cycle (or at minimum every 100ms). The OBS plugin reads this timestamp in `video_tick()`. If `now - heartbeat > threshold` (e.g., 500ms), the plugin stops revealing any safe regions and renders fully blurred.

**Ease of writing:** 3/5 — Requires adding a heartbeat field to the shm layout, clock synchronization (use `CLOCK_MONOTONIC` / `mach_absolute_time`), and threshold tuning.

**Ease of maintenance:** 4/5 — Simple logic, but the threshold needs tuning. Too tight = false alarms during load spikes. Too loose = longer PII exposure window.

**Reliability:** 4/5 — Catches daemon crashes and hangs reliably. Can produce false positives under heavy system load (daemon alive but slow). Does NOT catch corruption (daemon writes garbage timestamps).

**Latency to safe state:** 1 frame after threshold exceeded — the plugin checks every `video_tick()` (every frame), so once the threshold is crossed, the very next frame is fully blurred. With a 500ms threshold, worst case PII exposure after daemon death is 500ms + 16ms = ~516ms.

**Pros:**
- Catches daemon crashes (SIGKILL, SIGABRT, OOM kill)
- Catches daemon hangs (infinite loop, deadlock)
- Simple to implement and reason about
- Works even if the daemon process is replaced by a zombie

**Cons:**
- Threshold tuning: too tight triggers false positives under CPU pressure, too loose leaves a PII window
- Clock skew is not an issue on same-machine shm, but `mach_absolute_time` vs `clock_gettime` differences need care
- Does not detect corruption — if daemon writes a fresh timestamp but stale/wrong mask data, the watchdog is satisfied
- Adds 500ms (or whatever threshold) to the PII exposure window on daemon crash

**Key reference:** The watchdog pattern is standard in embedded systems (hardware watchdog timers) and container orchestration (Kubernetes liveness probes). OBS itself uses this pattern internally for detecting stalled encoders.

**Recommended threshold:** 200-500ms. The daemon writes mask data at 30Hz+ (every 33ms), so any gap >200ms strongly indicates failure. At 500ms, the exposure window is still under the 1-second mark where a viewer could realistically read PII text.

---

### 3. Sequence Counter Staleness

**How it works:** The daemon increments an atomic `uint64_t sequence` counter in shared memory every time it writes new mask data. The OBS plugin reads this counter in `video_tick()`. If the counter hasn't advanced in N frames (e.g., 30 frames = 1 second at 30fps), the plugin falls back to full blur.

**Ease of writing:** 4/5 — Simpler than heartbeat (no clock involved). Just an atomic increment and comparison.

**Ease of maintenance:** 4/5 — No threshold tuning beyond "how many stale frames." Easy to reason about.

**Reliability:** 4/5 — Same coverage as heartbeat (crashes, hangs) with the advantage of being clock-independent. Slightly more robust because it detects "daemon is alive but not producing new mask data" (e.g., daemon's CG poller thread died but main thread lives).

**Latency to safe state:** N frames after last update. If N=15 at 30fps, that's 500ms. Can be made tighter (N=5 = 166ms) at the cost of false positives during legitimate idle periods (no window changes = no new mask data).

**Pros:**
- No clock synchronization needed
- Detects daemon crashes, hangs, AND partial failures (compositor thread dead)
- Already planned in the architecture (EXISTING_IMPLEMENTATIONS.md shows `uint64_t sequence` in the shm struct)
- Lock-free: use `__atomic_load_n` / `atomic_load_explicit`

**Cons:**
- Ambiguity during legitimate idle: if no windows changed, daemon may not write new data. Solutions:
  - Daemon writes a "keepalive" increment even when mask hasn't changed
  - Plugin distinguishes "no change" from "no update" via a separate keepalive counter
- Same PII exposure window as heartbeat (threshold-dependent)
- Does not detect data corruption (counter advances but data is garbage)

**Key reference:** This is the standard pattern for lock-free single-producer/single-consumer shared memory. Used by audio drivers (JACK), video capture (V4L2), and the OBS `circlebuf` implementation.

**Implementation detail:** Use a double-buffer in shm with the sequence counter:
```c
// Writer (daemon):
atomic_store(&shm->sequence, shm->sequence + 1);  // odd = writing
memcpy(&shm->regions, new_data, size);
atomic_store(&shm->sequence, shm->sequence + 1);  // even = complete

// Reader (plugin):
uint64_t seq1 = atomic_load(&shm->sequence);
if (seq1 & 1) { /* writer mid-update, use cached */ return; }
memcpy(local_buf, &shm->regions, size);
uint64_t seq2 = atomic_load(&shm->sequence);
if (seq1 != seq2) { /* torn read, use cached */ return; }
filter->last_good_seq = seq1;
```

---

### 4. OBS Scene Fallback

**How it works:** The plugin detects a catastrophic failure (shm missing, daemon dead for >2s, plugin internal error) and triggers an OBS scene switch to a pre-configured "full blur" scene. This scene contains only a blurred display capture with no mask filter — guaranteeing full coverage.

**Ease of writing:** 3/5 — Requires the plugin to call OBS frontend API (`obs_frontend_set_current_scene()`) or send an obs-websocket message. Cross-thread concerns: `video_tick` runs on the graphics thread, scene switching must happen on the UI thread.

**Ease of maintenance:** 3/5 — Scene must be pre-created and kept in sync. If someone renames/deletes the fallback scene, the mechanism breaks silently.

**Reliability:** 3/5 — Depends on OBS's scene switching working correctly, the fallback scene existing, and the scene switch completing before the next frame renders. Scene switches can take 1-3 frames due to transition animations.

**Latency to safe state:** 100-300ms — Scene switch + optional transition. Can be minimized by using "Cut" transition (instant), but OBS still takes at least 1-2 frames to fully switch sources.

**Pros:**
- Complete isolation: the fallback scene has no dependency on the mask plugin at all
- Visible to the streamer: they see the scene switch in OBS and know something went wrong
- Can be manually triggered (streamer hotkey)
- Works even if the plugin itself crashes (if triggered by an external watchdog)

**Cons:**
- Scene switching is not instant (100-300ms)
- Requires pre-configuration (fallback scene must exist)
- Plugin calling `obs_frontend_set_current_scene` from the video thread is not safe — needs to be dispatched to the main thread
- Does not compose well with Studio Mode (preview vs program)
- Recovery is awkward: when do you switch back?

**Key reference:** This is analogous to broadcast master control switchers that have a "safe" source (color bars, slate) they can cut to if the program feed fails. The existing Talon system already does this via `obs_get_blurry()`.

---

### 5. Pre-Mask on Switch

**How it works:** When the daemon detects ANY window change (focus, open, close, move, resize), it immediately writes a fully-masked (all white) frame to shared memory FIRST, then computes the proper mask with safe regions, then writes that. The OBS plugin sees: frame N = full mask, frame N+1 = proper mask.

**Ease of writing:** 4/5 — Two writes instead of one. The "full mask" write is trivial (zero all regions, increment sequence).

**Ease of maintenance:** 4/5 — Simple logic. The only subtlety is ensuring the two writes are properly sequenced and that OBS reads at least one of them.

**Reliability:** 5/5 — Guarantees that during ANY transition, the first frame rendered is fully masked. Even if the daemon crashes during mask computation (between write 1 and write 2), the stream shows full mask.

**Latency to safe state:** 0ms — The pre-mask is written before the proper mask. There is never a frame where an old (potentially wrong) mask is shown for a new window configuration.

**Pros:**
- Eliminates the "stale mask" problem during transitions entirely
- One extra frame of full masking is imperceptible to viewers
- Works even for rapid app switching (each switch starts with full mask)
- Composable with default-white canvas (belt and suspenders)

**Cons:**
- One extra frame of "flash" (full blur → proper mask) on every window change. At 60fps this is 16ms — invisible to the eye, but technically present.
- Doubles the write frequency during transitions (minor, <1ms cost)
- Does not help with gradual drift (daemon slowly producing wrong masks without any window change event)

**Key reference:** This is the "mask-first" strategy from ARCHITECTURE.md section 6. Also analogous to the pattern in concurrent programming where you "lock then check" rather than "check then lock."

---

### 6. Dual-Layer Approach

**How it works:** Two OBS sources are stacked:
- **Bottom layer:** Display capture with an always-on blur filter (no mask — entire frame blurred)
- **Top layer:** Display capture with the PII mask plugin (reveals safe regions as transparent, masks unsafe regions as opaque-blurred)

If the top layer's plugin fails, crashes, or produces garbage, the bottom layer still shows a fully blurred screen. The top layer only adds "windows" of clarity over the blurred base.

**Ease of writing:** 3/5 — Requires configuring two sources in OBS and potentially a different rendering mode for the top layer (alpha channel output rather than direct composition).

**Ease of maintenance:** 3/5 — Two sources means double the GPU work (two display captures, two blur passes). Scene configuration is more complex. Must ensure z-order is never accidentally swapped.

**Reliability:** 5/5 — The safety guarantee is structural. Even if the plugin DLL is unloaded, the bottom blur layer remains. This is the most robust approach for surviving plugin crashes.

**Latency to safe state:** 0ms — The blurred layer is always rendering. If the top layer disappears, the output is immediately fully blurred.

**Pros:**
- Survives plugin crashes, unloads, and OBS filter chain errors
- No detection logic needed — safety is architectural, not reactive
- Works even if OBS's filter chain has bugs
- The bottom layer can be a different, simpler blur implementation for redundancy

**Cons:**
- Double GPU cost: two display captures + two blur passes. On a MacBook Pro this may matter.
- Complexity in scene setup: must maintain two synchronized sources
- Alpha blending between layers requires careful configuration
- If using two display captures, macOS may throttle to reduce overhead (Screen Recording permission captures are not free)
- Harder to reason about visually during scene editing

**Key reference:** This is the "defense in depth" pattern from security engineering. Also used in avionics displays where a hardware overlay provides safety-critical information independent of the software rendering layer.

**Practical variant:** Instead of two display captures, use a single capture with two filter paths:
1. Filter chain: Source → Blur → Output (always-on base)
2. Filter chain: Source → PII Mask Plugin → composites safe regions over the blur

This can be done within a single OBS source using the plugin's own rendering:
```c
// In video_render():
// 1. Always render blurred version (this is your safety net)
// 2. Only composite safe regions on top if shm is valid
// Result: if step 2 fails, you still have step 1's output
```

This is effectively what the default-white-canvas approach already does — making this a natural extension rather than a separate mechanism.

---

### 7. Stream Delay as Safety Net

**How it works:** OBS's built-in stream delay (Settings → Advanced → Stream Delay) buffers the encoded output for N seconds before sending to the RTMP/SRT server. A monitoring process watches the output and can trigger a "flush and replace" if PII is detected in the delayed buffer.

**Ease of writing:** 2/5 — OBS's stream delay is a simple buffer, but it does NOT support frame replacement. You would need to either:
- Kill the stream output and restart (causes viewer disconnect)
- Use a custom output plugin that can replace frames in the buffer
- Use an external restreaming proxy (e.g., nginx-rtmp) with buffer manipulation

**Ease of maintenance:** 2/5 — Adds 2-3 seconds of latency to the stream, which degrades chat interaction. The monitoring process is itself a failure point. Buffer manipulation is fragile.

**Reliability:** 3/5 — The delay gives you TIME but not MECHANISM. You still need something that detects the problem and can act on it within the delay window. If the detection is the same watchdog/heartbeat, the delay just extends the reaction window.

**Latency to safe state:** Not applicable in the traditional sense. The delay is a time buffer, not a detection mechanism. The actual latency to safe state depends on whatever detection mechanism triggers the response.

**Pros:**
- Gives 2-3 seconds of reaction time for ANY failure mode
- Human-in-the-loop: the streamer can see the preview and manually kill the stream before PII reaches viewers
- Works with existing OBS functionality (no plugin needed for the delay itself)
- Composable with other approaches — adds a safety margin on top of everything else

**Cons:**
- 2-3 second stream latency degrades interactivity
- No frame-replacement mechanism in OBS's built-in delay — you can only "flush" (disconnect viewers) or wait
- Does not prevent PII from being captured — only delays its transmission
- Local recording is NOT delayed, so PII appears in VODs
- If the monitoring process fails, the delay provides no benefit
- Adds complexity without adding a reliable automated response

**Key reference:** Broadcast TV uses 7-second delays for profanity, but they have dedicated hardware (Eventide BD600) that can dump/replace audio. OBS's delay is much simpler — it's just a FIFO buffer with no manipulation capability.

**Verdict:** Useful as an additional margin but should not be relied upon as a primary mechanism. Best used to give a human operator time to react.

---

### 8. Process Supervisor

**How it works:** The mask daemon is managed by macOS's `launchd` (via a plist in `~/Library/LaunchAgents/`). If the daemon crashes, launchd automatically restarts it. The plist specifies `KeepAlive = true` and `ThrottleInterval = 5` (restart within 5 seconds, with rate limiting).

**Ease of writing:** 4/5 — Writing a launchd plist is straightforward. The daemon just needs to be a well-behaved process (exit cleanly, handle SIGTERM).

**Ease of maintenance:** 5/5 — launchd is battle-tested macOS infrastructure. The plist is declarative. No custom supervisor code to maintain.

**Reliability:** 4/5 — launchd reliably restarts crashed processes. However:
- Restart takes 1-5 seconds (process startup + shm initialization + first CG poll)
- During restart, shm may be stale or missing — other mechanisms (watchdog, default-white) must cover this gap
- If the daemon is crash-looping (bug triggered on startup), launchd will throttle restarts (10-second backoff), leaving a longer gap

**Latency to safe state:** 1-5 seconds (daemon restart time). This is NOT the latency to safe state — that's handled by the plugin's default-white canvas and watchdog. The process supervisor's job is to restore NORMAL operation, not to provide fail-safe.

**Pros:**
- Automatic recovery from transient crashes (memory corruption, OOM, signal)
- No custom supervisor code — leverages OS infrastructure
- Works even if the crash is in a library (pyobjc, numpy)
- Can configure `StandardOutPath` and `StandardErrorPath` for crash forensics
- Can set `ProcessType = Adaptive` for appropriate scheduling priority

**Cons:**
- Does not provide instant safety — there's a restart gap
- Cannot fix bugs that cause crash loops
- On macOS, launchd is the only option (no systemd, no runit)
- The daemon must re-establish shm on restart — the plugin must handle shm disappearing and reappearing
- If the daemon writes corrupt data, the supervisor won't catch it (it's not crashing)

**Key reference:** launchd plist documentation: `man launchd.plist`. Example plist:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.trillium.pii-mask-daemon</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/bin/python3</string>
        <string>/path/to/pii_mask/daemon.py</string>
    </array>
    <key>KeepAlive</key>
    <true/>
    <key>ThrottleInterval</key>
    <integer>5</integer>
    <key>StandardOutPath</key>
    <string>/tmp/pii_mask_daemon.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/pii_mask_daemon.err</string>
</dict>
</plist>
```

---

### 9. Plugin Self-Test on Startup

**How it works:** When the OBS plugin loads (`obs_module_load`) and when the filter is created (`filter_create`), it runs a series of checks before allowing any region reveals:

1. Can it open the shm segment? (`shm_open` succeeds)
2. Can it mmap the segment? (correct size, readable)
3. Is the shm header valid? (magic number, version, sane dimensions)
4. Is the sequence counter non-zero? (daemon has written at least once)
5. Is the heartbeat fresh? (daemon is currently running)

If any check fails, the plugin operates in "fully masked" mode and logs a warning. It re-checks periodically (every 5 seconds) and transitions to normal mode when all checks pass.

**Ease of writing:** 4/5 — Straightforward sequence of checks. The only complexity is the state machine (startup → checking → normal → degraded → checking → ...).

**Ease of maintenance:** 5/5 — Self-contained within the plugin. No external dependencies for the check logic.

**Reliability:** 4/5 — Catches all initialization failures (daemon not running, shm not created, version mismatch). Does not catch runtime failures (daemon crashes after startup check passes) — that's the watchdog's job.

**Latency to safe state:** N/A — This is a gate, not a detection mechanism. The plugin never enters an unsafe state; it only leaves the safe state when all checks pass.

**Pros:**
- Prevents PII exposure during the "OBS started before daemon" race condition
- Validates shm format compatibility (version check catches daemon/plugin version skew)
- Provides clear diagnostic logging ("shm not found", "daemon not running", "version mismatch")
- Naturally composes with watchdog (startup check + ongoing monitoring)

**Cons:**
- Only protects startup, not runtime (need watchdog for that)
- Must handle the shm appearing asynchronously (daemon starts after OBS)
- Re-checking every 5 seconds means up to 5 seconds of full-blur after daemon starts

**Key reference:** The "preflight check" pattern. Used in avionics (Built-In Test Equipment — BITE), medical devices (POST), and container orchestration (readiness probes).

---

### 10. Graceful Degradation Ladder

**How it works:** Multiple fallback levels form a chain, each activated when the level above it fails:

```
Level 0: NORMAL        — shm valid, sequence advancing, regions rendered
  |  (shm stale > 200ms)
  v
Level 1: CACHED        — use last-known-good regions, blink indicator
  |  (stale > 1s OR shm unmapped)
  v
Level 2: FULL BLUR     — plugin renders fully blurred (default-white)
  |  (plugin detects own failure OR OBS filter error)
  v
Level 3: SCENE SWITCH  — switch to full-blur scene (no plugin dependency)
  |  (OBS unresponsive)
  v
Level 4: STREAM KILL   — disconnect stream output entirely
```

**Ease of writing:** 2/5 — Requires implementing multiple detection mechanisms, state machine transitions, and recovery paths. The ladder logic itself is complex with edge cases (what if you're at level 2 and the daemon comes back? Do you jump to level 0 or go through level 1 first?).

**Ease of maintenance:** 2/5 — More failure modes = more code paths = more testing surface. Each level transition needs to be tested independently. State machine bugs could cause oscillation (rapid switching between levels).

**Reliability:** 5/5 — This is the most comprehensive approach. Every failure mode is covered by at least one level. Even total system failure (OBS crash) results in stream disconnection rather than PII exposure.

**Latency to safe state:** Varies by level:
- Level 0→1: 200ms (shm staleness threshold)
- Level 1→2: 800ms additional (1s total)
- Level 2→3: Near-instant (same frame, but scene switch takes 100-300ms to complete)
- Level 3→4: Configurable (e.g., 5s timeout)

**Pros:**
- Covers every failure mode
- Graceful: minor issues (brief daemon stall) don't cause full-blur
- Observable: each level can be logged/alerted differently
- Recoverable: the ladder can descend (recover) as well as ascend (degrade)

**Cons:**
- Significant implementation complexity
- State machine bugs could themselves be a safety issue
- Testing all transitions is combinatorially expensive
- Overkill for a system where the simple approach (default-white + watchdog) covers 99% of cases
- The "cached regions" level (1) is debatable — stale data may be wrong data

**Key reference:** TCP congestion control (slow start → congestion avoidance → fast recovery) and circuit breaker patterns (closed → open → half-open) in distributed systems.

**Recommendation:** Implement levels 0, 2, and 3. Skip level 1 (cached data is risky) and level 4 (too aggressive). This simplifies the ladder to: Normal → Full Blur → Scene Switch.

---

## Additional Research

### What does obs-backgroundremoval do when its model fails?

Based on the source code (`src/background-filter.cpp`):

1. **Model load failure:** Falls back to a "no mask" state — the source renders unchanged (no blur applied). This is the OPPOSITE of fail-safe for our use case. The plugin prioritizes "show something" over "hide something."

2. **Inference produces garbage:** The plugin applies whatever mask the model outputs. There is no validation of mask quality. If the model outputs all-zeros, the background is fully visible. If it outputs all-ones, the person disappears.

3. **Inference timeout / stall:** The plugin uses the last-known-good mask from the previous successful inference. This can persist indefinitely if the inference thread hangs.

4. **Plugin crash:** OBS catches the exception in the filter chain and disables the filter. The source renders without any blur — fully visible.

**Lesson for us:** obs-backgroundremoval's failure modes are exactly wrong for PII protection. It fails OPEN (shows everything). We must fail CLOSED (hides everything). This is a fundamental design difference.

### How do broadcast systems (TV) handle fail-safe for live content?

**Broadcast delay hardware (e.g., Eventide BD600, Evertz delays):**
- 7-second (FCC standard) or configurable delay
- Hardware button to "dump" (replace buffer with silence/black)
- Automatic dump on loss of input signal
- Some models can "build back" delay after a dump without disconnecting

**Master control switchers (e.g., Grass Valley, Ross Video):**
- Default output is color bars + tone if no source is routed
- Automatic fallback to "safe" source (slate, pre-recorded content) on input loss
- GPI (General Purpose Interface) triggers for emergency switching
- "Upstream keyer" architecture means overlays fail independently of program source

**Playout automation (e.g., Harmonic, Imagine Communications):**
- If playout server fails, downstream switcher cuts to backup source
- Redundant paths: A/B playout servers, automatic failover
- "Black detector" and "freeze detector" trigger alerts and automatic switching

**Standards:**
- SMPTE ST 2110: Requires "black frame" output when no valid source is connected
- EBU R128: Audio loudness standards include silence detection
- ATSC A/85: Requires audio monitoring and automatic level correction

**Key takeaway:** Broadcast engineering ALWAYS defaults to a known-safe output (black, bars, slate). The system is designed so that "no input" produces a safe output. This is exactly our default-white-canvas approach.

### Can the OBS plugin detect its own crash and recover?

**Signal handlers in C:**

Yes, a C plugin can install signal handlers for `SIGSEGV`, `SIGBUS`, `SIGABRT`, and `SIGFPE`. However:

1. **OBS runs filters on the graphics thread.** A signal handler in the plugin would execute in the context of OBS's video thread. Calling OBS API functions from a signal handler is NOT safe (they're not async-signal-safe).

2. **What you CAN do in a signal handler:**
   - Set an atomic flag (`volatile sig_atomic_t crashed = 1`)
   - Write to a pre-opened file descriptor (for logging)
   - Call `_exit()` (but this kills OBS entirely)
   - Call `longjmp` back to a safe point (VERY dangerous, will corrupt OBS state)

3. **What you CANNOT do:**
   - Call `malloc`, `free`, `printf`, or any non-async-signal-safe function
   - Call any OBS API function
   - Switch scenes or modify the filter chain
   - Reliably recover the corrupted state that caused the crash

4. **Practical approach:** Use `sigsetjmp`/`siglongjmp` to jump back to the start of `video_render()` and return early (rendering the blurred default). This is fragile but has precedent in JVMs and Lua interpreters.

```c
static sigjmp_buf render_recovery;
static volatile sig_atomic_t in_render = 0;

static void crash_handler(int sig) {
    if (in_render) {
        siglongjmp(render_recovery, 1);
    }
    // If not in render, let the default handler run
    signal(sig, SIG_DFL);
    raise(sig);
}

void pii_mask_video_render(void *data, gs_effect_t *effect) {
    struct pii_filter *f = data;
    in_render = 1;

    if (sigsetjmp(render_recovery, 1) != 0) {
        // Crashed during render — output fully blurred
        render_full_blur(f, effect);
        in_render = 0;
        return;
    }

    // Normal render path...
    in_render = 0;
}
```

**Verdict:** Possible but extremely fragile. The `siglongjmp` may corrupt OBS's graphics context (OpenGL state, texture bindings). A better approach is to keep the render path so simple that it can't crash (just texture lookups and shader calls), and move all complex logic (shm reading, region parsing) to `video_tick()` with proper error handling.

**Recommendation:** Don't rely on signal-handler recovery. Instead:
- Keep `video_render()` trivially simple (no shm access, just use cached data from `video_tick()`)
- Do all fallible work in `video_tick()` with proper error checking
- If `video_tick()` encounters an error, set a flag that makes `video_render()` output full blur
- Let the dual-layer approach handle the case where the plugin itself is unloaded/crashes

### Maximum acceptable PII exposure window

**Context:** The question is how long PII can be visible on the stream before it's considered a privacy breach.

**Analysis by content type:**

| Content | Read Time | Exposure Risk | Acceptable Window |
|---------|-----------|---------------|-------------------|
| Full name in large text | ~200ms | Medium | <100ms (unreadable) |
| Email address | ~500ms | High | <100ms |
| Phone number | ~300ms | High | <100ms |
| Chat message text | ~1-2s | Medium | <200ms |
| Address/location | ~1s | Medium | <200ms |
| Credit card number | ~1s | Very High | <50ms |
| Password field (dots) | N/A | Low | 1s+ |
| Desktop icons/filenames | ~500ms | Low | <500ms |
| Small text in background | ~2s+ | Low | <1s |

**Frame-level analysis:**
- 1 frame at 30fps = 33ms — **unreadable** by humans. Even with pause, stream compression artifacts make single frames nearly unreadable.
- 2-3 frames = 66-100ms — **marginally readable** for large text if paused. Not readable in real-time.
- 10 frames = 333ms — **readable** for large text by attentive viewers.
- 30 frames = 1s — **easily readable** for most content.

**Stream recording consideration:** VODs can be paused and frame-advanced. A single clear frame could expose PII if someone deliberately scrubs through the recording. However, stream encoding (x264/NVENC) produces significant artifacts on scene changes, and a single "flash" frame will be heavily compressed (low quality I-frame or degraded P-frame).

**Recommendation:**
- **Target: < 1 frame (0ms)** using default-white-canvas approach
- **Acceptable: < 3 frames (100ms)** for daemon restarts and edge cases
- **Maximum tolerable: < 500ms** for catastrophic failures with recovery
- **Unacceptable: > 1 second** — this should trigger stream kill

### How to test fail-safes without crashing production

**1. Chaos testing via kill signals:**
```bash
# Test daemon crash recovery
kill -9 $(pgrep -f pii_mask_daemon)
# Observe: plugin should fall back to full blur within threshold

# Test daemon hang
kill -STOP $(pgrep -f pii_mask_daemon)  # freeze the process
# Observe: watchdog should trigger after threshold
kill -CONT $(pgrep -f pii_mask_daemon)  # resume
# Observe: should recover to normal operation
```

**2. Shared memory corruption testing:**
```python
# Write garbage to shm while daemon is stopped
import mmap, posix_ipc
shm = posix_ipc.SharedMemory("/pii_mask_regions")
m = mmap.mmap(shm.fd, 0)
m[:] = b'\xff' * len(m)  # corrupt everything
# Observe: plugin should detect invalid data and full-blur
```

**3. Plugin-side test mode:**
Build the plugin with a `CHAOS_TEST` compile flag that:
- Randomly skips shm reads (simulates read failure)
- Randomly zeroes out region data (simulates corruption)
- Periodically unmaps shm (simulates daemon crash)
- Logs every state transition for analysis

**4. OBS replay buffer analysis:**
Enable OBS replay buffer (30-second rolling buffer). After each test:
- Save the replay buffer
- Frame-by-frame analysis to verify no PII frames leaked
- Automated: pipe replay buffer through OCR and flag any text detection

**5. Dedicated test scene:**
Create an OBS scene with a known PII source (e.g., a text file with "CANARY: 555-0123") that should always be masked. Automated monitoring checks the stream output for the canary string.

**6. Integration test harness:**
```bash
# Start daemon with known window layout
# Start OBS with test scene
# Run test sequence:
for test in daemon_kill daemon_hang shm_corrupt shm_delete rapid_switch; do
    run_test $test
    sleep 2
    capture_obs_output
    assert_no_pii_in_output
    restore_normal
done
```

**7. Simulated shm via test fixtures:**
The plugin can be configured to read from a file-backed mmap instead of POSIX shm. Test fixtures can pre-create various failure scenarios:
- `test_shm_empty.bin` — no regions, valid header
- `test_shm_corrupt.bin` — invalid magic number
- `test_shm_stale.bin` — valid but old sequence counter
- `test_shm_missing` — file doesn't exist

---

## Recommended Implementation Plan

Based on this research, the recommended fail-safe architecture combines approaches that are complementary:

### Must-Have (implement for v1)

| Priority | Mechanism | Why |
|----------|-----------|-----|
| P0 | **Default-white canvas** (#1) | Foundation. Zero-cost, zero-latency, structural safety. |
| P0 | **Pre-mask on switch** (#5) | Eliminates stale-mask exposure during transitions. |
| P0 | **Sequence counter** (#3) | Detects daemon crash/hang. Low complexity. |
| P0 | **Plugin self-test** (#9) | Prevents startup race conditions. |
| P1 | **Process supervisor** (#8) | Automatic daemon restart via launchd. |

### Should-Have (implement for v2)

| Priority | Mechanism | Why |
|----------|-----------|-----|
| P1 | **OBS scene fallback** (#4) | Handles plugin-level failures. |
| P2 | **Stream delay** (#7) | 2-3s buffer for human intervention. Only if latency is acceptable. |

### Nice-to-Have (implement if needed)

| Priority | Mechanism | Why |
|----------|-----------|-----|
| P2 | **Dual-layer** (#6) | Maximum redundancy. Implement if plugin crashes are observed. |
| P3 | **Full degradation ladder** (#10) | Only if the simpler approach proves insufficient. |

### Why NOT heartbeat (#2)?

The sequence counter (#3) subsumes the heartbeat. If the sequence counter is advancing, the daemon is alive and producing data. A separate heartbeat adds no information. The daemon should increment a keepalive counter even during idle periods (no window changes), which makes the sequence counter equivalent to a heartbeat.

### Combined architecture

```
[Daemon]                              [OBS Plugin]
   |                                      |
   | writes mask regions + increments     |
   | sequence counter to POSIX shm        |
   |                                      |
   |     /dev/shm/pii_mask_regions        |
   +----> [header: magic, version, seq] --+---> video_tick():
          [regions: x,y,w,h,r per rect]   |      1. self-test (shm exists, valid)
                                           |      2. check sequence counter freshness
                                           |      3. if stale: set full_blur flag
                                           |      4. if fresh: copy regions to local buf
                                           |
                                           +---> video_render():
                                                  1. render source to texture
                                                  2. apply blur to full frame
                                                  3. if full_blur OR no regions:
                                                       output blurred frame (SAFE)
                                                  4. else: punch safe-region holes
                                                       output composited frame

[launchd] -- restarts daemon if it exits
[OBS scene fallback] -- triggered if plugin reports fatal error
```

---

## Sources

- OBS `mask-filter.c`: https://github.com/obsproject/obs-studio/blob/master/plugins/obs-filters/mask-filter.c
- obs-backgroundremoval: https://github.com/royshil/obs-backgroundremoval
- obs-composite-blur: https://github.com/FiniteSingularity/obs-composite-blur
- POSIX shared memory: `man shm_open`, `man mmap`
- macOS launchd: `man launchd.plist`
- Eventide BD600 broadcast delay: https://www.eventideaudio.com/bd600
- SMPTE ST 2110 (professional media over IP)
- Lock-free SPSC patterns: Lamport, "Proving the Correctness of Multiprocess Programs" (1977)
- Signal-safe functions: POSIX.1-2017, Section 2.4.3
- `siglongjmp` recovery: used in OpenJDK HotSpot (`os_linux.cpp` signal handler), Lua 5.x (`luaD_rawrunprotected`)
- Circuit breaker pattern: Nygard, "Release It!" (2007)
