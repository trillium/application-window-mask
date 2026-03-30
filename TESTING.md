# PII Mask — Testing & Verification Strategy

## Automated Verification Pipeline

Every test uses the OBS websocket + shm reader to verify programmatically.
No human-in-the-loop required for pass/fail.

### 1. Protocol Layer

**shm struct correctness**
- Write known rects via Python PiiMaskWriter
- Read back via raw struct.unpack and verify field-by-field
- Assert magic, version, sequence (even), rect_count, flags, timestamp
- Assert each rect's x, y, width, height, corner_radius, flags match input

**seqlock correctness**
- Write rects, verify sequence increments by 2 (odd during write, even after)
- Concurrent read during write should see odd sequence and retry

**staleness detection**
- Write with old timestamp, verify C reader marks stale after 5s

### 2. Daemon Layer

**window polling**
- Call WindowPoller.poll(), assert returns list of dicts
- Assert each dict has required keys: owner, title, pid, layer, x, y, width, height
- Assert reasonable values (width > 0, height > 0, owner is non-empty string)

**classification**
- For each SAFE_OWNER: assert is_unsafe() returns False at layer=0
- For each ALWAYS_MASK_OWNER: assert is_unsafe() returns True regardless of layer
- Assert layer > 0 always returns True (even for safe owners)
- Assert unknown owner returns True (default-deny)

**scene model dirty detection**
- Same rects twice: second update returns False
- Changed rect: update returns True
- Different count: update returns True

**shm write integration**
- Start daemon, wait 1s, read shm
- Assert magic/version correct
- Assert rect_count > 0
- Assert FLAG_DAEMON_ALIVE set
- Assert timestamp is recent (< 1s old)

### 3. Plugin Layer (via OBS websocket)

**plugin loads**
- Parse OBS log file, assert "pii-mask-filter" appears in module load section

**filter attaches**
- Add filter via websocket, query filter list, assert pii_mask_filter present

**visual verification — full mask**
- Set FLAG_FULL_MASK in shm
- Take screenshot via `get_source_screenshot`
- Assert screenshot is entirely black (all pixels RGB < threshold)

**visual verification — no mask**
- Write 0 rects with FLAG_DAEMON_ALIVE
- Take screenshot
- Assert screenshot is NOT entirely black (has varied pixel values)

**visual verification — rect placement**
- Write a single known rect (e.g. x=100, y=100, 200x200)
- Take screenshot
- Assert pixels inside rect region are black
- Assert pixels outside rect region are NOT black

**visual verification — unsafe window masking**
- Start daemon normally
- Take screenshot via websocket
- For each unsafe rect the daemon wrote to shm:
  - Sample pixels at the center of that rect in the screenshot
  - Assert they are black (RGB < threshold)
- For at least one safe window region:
  - Sample pixels at its center
  - Assert they are NOT black

**fail-safe — daemon not running**
- Kill daemon, do NOT write to shm
- Wait for staleness timeout (5s) or disconnect
- Take screenshot
- Assert entire frame is black (full mask engaged)

**fail-safe — startup**
- Fresh start with no shm present
- Assert plugin renders full black (not connected = full mask)

### 4. Coordinate Mapping

**screen coords match OBS coords**
- Write a rect at a known screen position
- Take screenshot, locate the black rect in the image
- Verify pixel position in screenshot matches the rect coordinates
  (accounting for any display scaling factor between logical points and capture resolution)

### 5. Performance

**daemon poll latency**
- Time 100 poll cycles, assert average < 5ms per poll
- Assert shm write time < 1ms

**shm read latency (from C side)**
- Not directly testable without instrumenting plugin, but can verify
  indirectly: change daemon rects rapidly, take rapid screenshots,
  verify mask positions update within 2 frames

## Running Tests

```bash
# Protocol + daemon unit tests (no OBS required)
python3 -m pytest tests/

# Integration tests (OBS must be running with websocket enabled)
python3 -m pytest tests/integration/ --obs-password=<password>
```

## Test Fixtures

- `test_shm_roundtrip` — write/read protocol structs
- `test_classifier_rules` — exhaustive classification checks
- `test_daemon_smoke` — start daemon 2s, verify shm valid
- `test_obs_filter_loaded` — check OBS logs
- `test_obs_full_mask` — visual: all black when full mask
- `test_obs_rect_placement` — visual: black at known coords
- `test_obs_failsafe` — visual: all black when daemon dead
