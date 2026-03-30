# OBS Mask Timing Research for PII Protection

> Research date: 2026-03-29
> Goal: Determine end-to-end latency from "mask data available" to "PII hidden on output"

---

## 1. Built-in Image Mask/Blend Filter Internals (`mask_filter_v2`)

**Source**: [`plugins/obs-filters/mask-filter.c`](https://github.com/obsproject/obs-studio/blob/master/plugins/obs-filters/mask-filter.c)

### How it reloads images

The mask filter uses **polling-based file monitoring**, not filesystem events:

```c
static void mask_filter_tick(void *data, float seconds)
{
    struct mask_filter_data *filter = data;
    filter->update_time_elapsed += seconds;

    if (filter->update_time_elapsed >= 1.0f) {
        time_t t = get_modified_timestamp(filter->image_file);
        filter->update_time_elapsed = 0.0f;

        if (filter->image_file_timestamp != t) {
            mask_filter_image_load(filter);
        }
    }
}
```

**Key findings:**

| Aspect | Detail |
|--------|--------|
| Polling interval | **1.0 second** — hardcoded, not configurable |
| Detection method | `os_stat()` → compares `st_mtime` against cached timestamp |
| Reload trigger | mtime change detected, OR `obs_source_update()` called (settings change) |
| Image loading | `gs_image_file_init()` → reads file from disk → `gs_image_file_init_texture()` → uploads to GPU |
| Thread sync | Uses `obs_enter_graphics()` / `obs_leave_graphics()` (graphics context lock) |
| Cache | `gs_image_file` struct holds the texture in GPU memory; entire file is re-read on reload |

### Latency analysis for file-based approach

**Worst case**: You write a new mask PNG. The tick just checked 0.01s ago → waits ~1.0s for next poll → detects mtime change → reads PNG from disk (~1-5ms for a small file) → uploads texture to GPU (sub-ms) → applied on next frame render (~16.7ms at 60fps).

| Scenario | Latency |
|----------|---------|
| **Best case** (mtime check happens right after write) | ~17-34ms (1-2 frames) |
| **Average case** | ~500ms + 1 frame |
| **Worst case** (just missed the poll) | ~1000ms + file I/O + 1 frame ≈ **1017ms** |

**Verdict: Completely unsuitable for real-time PII protection.** The 1-second polling interval alone makes this a non-starter.

### Settings-triggered reload (via obs-websocket)

When `obs_source_update()` is called (e.g., from `SetSourceFilterSettings`), `mask_filter_update_internal()` fires immediately and reloads the image — **bypassing the 1-second poll entirely**. This is a separate, faster path.

---

## 2. obs-websocket Approaches

### SetSourceFilterSettings — Force reload by changing `image_path`

**Protocol**: [obs-websocket v5 protocol](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md)

The `SetSourceFilterSettings` request triggers `obs_source_update()` on the filter, which calls `mask_filter_update_internal()` → `mask_filter_image_load()`. This **immediately** re-reads the file and uploads the texture — no 1-second poll wait.

**Double-buffer (A/B swap) technique:**
1. Pre-create two mask files: `mask_a.png` and `mask_b.png`
2. Write new mask data to the *inactive* file
3. Call `SetSourceFilterSettings` to swap `image_path` to the newly-written file
4. On next frame, the new mask is active

**Latency breakdown:**

| Stage | Time |
|-------|------|
| Write PNG to disk | 1-5ms (small file, SSD) |
| WebSocket roundtrip (localhost) | 1-5ms |
| OBS processes message (next tick) | 0-16.7ms (up to 1 frame) |
| `gs_image_file_init()` — file read | 1-3ms |
| `gs_image_file_init_texture()` — GPU upload | <1ms |
| Applied on next render | 0-16.7ms (up to 1 frame) |
| **Total** | **~20-50ms typical** |

**Batch requests**: obs-websocket supports `RequestBatch` with execution types:
- `SerialRealtime` — processes sequentially
- `SerialFrame` — all execute within one frame
- `Parallel` — concurrent execution

Using `SerialFrame` ensures the filter update is applied atomically within a single frame.

### SetSourceFilterEnabled — toggle filter on/off

Can be used to briefly disable then re-enable a filter, but this causes a visible flash. Not useful for mask swaps.

### Latency of obs-websocket itself

- Local loopback WebSocket: **~1-2ms** roundtrip
- OBS processes WebSocket messages on the **main/graphics thread tick**, so there's up to one frame of delay before the message is handled
- No documented latency guarantees in the protocol spec

---

## 3. Alternative Faster Approaches

### 3a. Browser Source Overlay (Recommended for simplicity)

Use a browser source as a mask/overlay layer positioned above the video source.

**How it works:**
- Browser source renders HTML/CSS/Canvas content via CEF (Chromium Embedded Framework)
- Your PII detection system sends mask coordinates via WebSocket to the browser source's JavaScript
- JavaScript draws black rectangles (or blur via CSS/Canvas) over PII regions
- OBS composites the browser source over the video

**Latency:**
| Stage | Time |
|-------|------|
| WebSocket message to browser source | 1-2ms (localhost) |
| JavaScript processing + Canvas render | 1-5ms |
| CEF composites to shared texture | 0-16.7ms (frame-synced with OBS) |
| OBS renders the scene | 0-16.7ms |
| **Total** | **~20-40ms (1-2 frames at 60fps)** |

**Pros**: No file I/O, no PNG encoding/decoding, simple to implement, uses standard web tech.
**Cons**: CEF has known frame-pacing quirks (historically stuttery in OSR mode, largely fixed). Browser source may lag if OBS window loses focus (Efficiency Mode on Windows). Adds ~100-200MB memory for CEF process.

**obs-browser vendor events**: The obs-browser plugin supports custom events via obs-websocket's vendor request system (`emit_event`), which broadcasts to all browser sources. This provides a clean signaling path.

### 3b. Custom OBS C Plugin — In-Memory Mask Filter (Fastest possible)

Write a native OBS filter plugin that:
1. Reads mask data from **shared memory** (POSIX shm or mmap)
2. Uploads the mask as a GPU texture each frame via `gs_texture_create()` or `gs_texture_set_image()`
3. Applies it as an alpha mask in a shader during `video_render()`

**This is how obs-backgroundremoval and similar real-time plugins work:**

```
video_tick():
  - Read mask data from shared memory / run inference
  - Store in CPU-side buffer

video_render():
  - obs_source_process_filter_begin()
  - Upload mask buffer → gs_texture_create(width, height, GS_R8, 1, &data, 0)
  - Bind texture to shader: gs_effect_set_texture(param, mask_texture)
  - obs_source_process_filter_end()
  - gs_texture_destroy(mask_texture)
```

**Latency:**
| Stage | Time |
|-------|------|
| Shared memory read | <0.1ms |
| Texture upload to GPU | <1ms (for 1920x1080 single-channel) |
| Shader application | <0.5ms |
| **Total per frame** | **<2ms additional render time** |
| **End-to-end** (mask written → pixels hidden) | **0-16.7ms (next frame boundary)** |

**This achieves sub-frame latency** — the mask is applied on the very next rendered frame after it appears in shared memory.

**Pros**: Absolute minimum latency (1 frame max), no file I/O, no IPC overhead, proven pattern (background removal plugins do this at 30+ fps).
**Cons**: Requires writing and compiling a C/C++ OBS plugin, platform-specific shared memory code, more complex deployment.

### 3c. OBS Lua/Python Script with Source Manipulation

OBS scripts can use `script_tick(seconds)` which fires every frame. A script could:
- Read mask data from a file or named pipe
- Create/update an image source's texture

**Limitations:**
- Lua/Python scripting API does NOT expose `gs_texture_create` or direct texture manipulation
- Scripts can call `obs_source_output_video()` for async sources but cannot create custom filter rendering
- File I/O in `script_tick` would block the render thread
- **Not viable for per-frame mask updates**

### 3d. obs-shaderfilter with External Texture

The [obs-shaderfilter](https://github.com/exeldro/obs-shaderfilter) plugin allows custom HLSL/GLSL shaders with `texture2d` uniform parameters that map to image files.

**Approach**: Write a custom shader that reads a mask texture and applies alpha masking. The texture parameter points to a file that you update externally.

**Problem**: The shaderfilter's texture parameter uses the same `gs_image_file` infrastructure — subject to the same 1-second polling. You'd need to trigger settings updates via obs-websocket to force reloads, bringing you back to the ~20-50ms latency of approach #2.

### 3e. Advanced Masks Plugin — Source Mask

The [obs-advanced-masks](https://github.com/FiniteSingularity/obs-advanced-masks) plugin supports **Source Masks** — using any OBS source as a dynamic mask.

**Approach:**
1. Create a "mask source" (e.g., a browser source or custom source)
2. Apply Advanced Masks filter to the video source
3. Set mask type to "Source Mask"
4. Point it at your mask source
5. Update the mask source dynamically (e.g., browser source via WebSocket)

**Latency**: Same as the mask source's update latency. If using a browser source as the mask source, ~20-40ms. If using a custom C plugin source, ~0-17ms.

**Pros**: No custom plugin compilation needed, flexible, the mask source updates in real-time automatically.
**Cons**: Requires the Advanced Masks plugin to be installed.

---

## 4. OBS Rendering Pipeline Timing

### Frame cycle

The OBS video pipeline runs on a dedicated graphics thread (`obs_graphics_thread`):

```
1. Calculate frame timing (sleep until next frame boundary)
2. tick_sources() — calls video_tick() on ALL sources and filters
3. render_main_texture() — traverses scene tree, calls video_render()
   - For each source: render base → apply filter chain (reverse order)
   - Each filter: obs_source_process_filter_begin() → shader → _end()
4. Scale to output resolution
5. Color format conversion (RGB → YUV on GPU)
6. Copy to CPU staging surface (for encoder)
7. Distribute to encoders and outputs
```

### Key timing facts

| Parameter | Value |
|-----------|-------|
| Frame interval at 30fps | 33.3ms |
| Frame interval at 60fps | 16.7ms |
| Typical render time (single preview) | ~2-3ms |
| Typical render time (studio mode) | ~10-45ms |
| `video_tick()` → `video_render()` gap | Near-zero (same thread, sequential) |
| Filter chain processing | Sequential per-source, sub-ms per filter |

### When filter inputs are read

Filters read their inputs during `video_render()`, which happens **after** `video_tick()` in the same frame. This means:
- If you update mask data during or before `video_tick()`, it's applied in that **same frame**
- If you update after `video_render()` has started, it won't take effect until the **next frame**

### Theoretical minimum latency

**For a custom plugin reading from shared memory:**
- Mask data written to shm → next `video_tick()` reads it → `video_render()` applies it
- Minimum: **0ms** (if written just before tick) to **16.7ms** (if written just after tick at 60fps)
- Average: **~8ms** at 60fps

**For file-based approaches (obs-websocket + SetSourceFilterSettings):**
- Minimum: **~20ms** (WebSocket latency + file I/O + next frame)
- Average: **~35ms**

---

## 5. Direct Pixel Manipulation via Plugin API

### How real-time plugins update masks

The obs-backgroundremoval plugin demonstrates the gold standard:

1. **`video_tick()`**: Runs neural network inference on the current frame, produces a mask `cv::Mat`
2. **`video_render()`**: Uploads mask as GPU texture, binds to shader, applies alpha blending
3. **Per-frame cost**: Inference time (5-50ms depending on model/GPU) + texture upload (<1ms) + shader (<0.5ms)
4. **`maskEveryXFrames`**: Option to skip inference on some frames (e.g., every 5 frames at 30fps halves CPU time)

### Relevant OBS graphics API functions

```c
// Create a texture from CPU-side pixel data (call within graphics context)
gs_texture_t *gs_texture_create(uint32_t width, uint32_t height,
    enum gs_color_format color_format, uint32_t levels,
    const uint8_t **data, uint32_t flags);

// Destroy texture when done
void gs_texture_destroy(gs_texture_t *tex);

// Bind texture to effect parameter
void gs_effect_set_texture(gs_eparam_t *param, gs_texture_t *val);

// Filter rendering helpers
bool obs_source_process_filter_begin(obs_source_t *filter,
    enum gs_color_format format, enum obs_allow_direct_render allow_direct);
void obs_source_process_filter_end(obs_source_t *filter,
    gs_effect_t *effect, uint32_t width, uint32_t height);
```

### Shared memory approach (for custom plugin)

```c
// In video_tick():
void *shm_ptr = /* mmap'd shared memory region */;
uint32_t *version = (uint32_t *)shm_ptr;
if (*version != filter->last_version) {
    memcpy(filter->mask_buffer, shm_ptr + header_size, width * height);
    filter->last_version = *version;
    filter->mask_dirty = true;
}

// In video_render():
if (filter->mask_dirty) {
    gs_texture_destroy(filter->mask_tex);
    const uint8_t *data_ptr = filter->mask_buffer;
    filter->mask_tex = gs_texture_create(w, h, GS_R8, 1, &data_ptr, 0);
    filter->mask_dirty = false;
}
gs_effect_set_texture(filter->mask_param, filter->mask_tex);
```

### obs-framebridge (read-only)

The [obs-framebridge](https://github.com/heiner-palmen/obs-framebridge) plugin captures frame buffers to CPU-accessible memory for Lua scripts. It's **read-only** — useful for feeding frames to a PII detector, but cannot write mask data back into the pipeline.

---

## 6. Recommended Architecture for PII Protection

### Tier 1: Fastest (requires C plugin development)

**Custom OBS filter plugin + shared memory**

```
Camera → OBS Source
            ↓
    [Custom PII Mask Filter]
       video_tick():
         - Read mask from shared memory (written by external PII detector)
       video_render():
         - Upload mask texture, apply via shader
            ↓
       Masked Output → Encoder → Stream
```

- **Latency**: 1 frame (0-16.7ms at 60fps)
- **Effort**: High (C/C++ plugin, shared memory IPC, shader code)

### Tier 2: Fast enough for most cases (no custom compilation)

**Browser source overlay + WebSocket signaling**

```
Camera → OBS Source (bottom layer)
Browser Source (top layer, black regions over PII)
            ↑
    PII Detector → WebSocket → JS draws mask rectangles
```

- **Latency**: 1-3 frames (~17-50ms at 60fps)
- **Effort**: Low (HTML/JS/CSS, standard WebSocket)

### Tier 3: Acceptable with caveats

**obs-websocket + A/B file swap on Image Mask/Blend filter**

```
PII Detector → Write mask_b.png
            → SetSourceFilterSettings(image_path="mask_b.png")
            → OBS reloads immediately (bypasses 1s poll)
```

- **Latency**: 2-4 frames (~35-65ms at 60fps)
- **Effort**: Low (Python/Node script, obs-websocket client)
- **Caveat**: File I/O on every update, PNG encode/decode overhead

---

## 7. Summary of Latency Numbers

| Approach | Min Latency | Typical Latency | Max Latency |
|----------|-------------|-----------------|-------------|
| Built-in mask filter (file polling) | ~17ms | ~500ms | **~1017ms** |
| obs-websocket A/B file swap | ~20ms | ~35ms | ~65ms |
| Browser source overlay | ~17ms | ~30ms | ~50ms |
| Advanced Masks + source mask (browser) | ~17ms | ~30ms | ~50ms |
| **Custom C plugin + shared memory** | **<1ms** | **~8ms** | **~17ms** |
| obs-backgroundremoval pattern | ~5ms | ~15ms | ~50ms+ (inference) |

**Bottom line**: Do NOT rely on the built-in mask filter's file polling for real-time PII protection. The 1-second polling interval is a dealbreaker. Use either a browser source overlay (simplest) or a custom plugin with shared memory (fastest). The obs-websocket A/B swap is a viable middle ground if you can tolerate ~35ms latency.

---

## Sources

- [OBS mask-filter.c source](https://github.com/obsproject/obs-studio/blob/master/plugins/obs-filters/mask-filter.c)
- [obs-websocket protocol documentation](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md)
- [OBS Source API Reference](https://docs.obsproject.com/reference-sources)
- [OBS Core Graphics API](https://docs.obsproject.com/reference-libobs-graphics-graphics)
- [OBS Rendering Graphics guide](https://docs.obsproject.com/graphics)
- [OBS Video and Audio Pipelines (DeepWiki)](https://deepwiki.com/obsproject/obs-studio/2.4-video-and-audio-pipelines)
- [OBS Filters and Effects (DeepWiki)](https://deepwiki.com/obsproject/obs-studio/4.7-filters-and-effects)
- [Building an OBS Background Removal Plugin walkthrough](https://www.morethantechnical.com/blog/2023/05/20/building-an-obs-background-removal-plugin-a-walkthrough/)
- [obs-backgroundremoval GitHub](https://github.com/royshil/obs-backgroundremoval)
- [obs-advanced-masks GitHub](https://github.com/FiniteSingularity/obs-advanced-masks)
- [obs-shaderfilter GitHub](https://github.com/exeldro/obs-shaderfilter)
- [obs-framebridge GitHub](https://github.com/heiner-palmen/obs-framebridge)
- [obs-browser README](https://github.com/obsproject/obs-browser/blob/master/README.md)
- [OBS Python/Lua Scripting docs](https://docs.obsproject.com/scripting)
- [OBS Backend Design](https://docs.obsproject.com/backend-design)
