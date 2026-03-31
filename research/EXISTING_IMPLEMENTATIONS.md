# Existing Implementations for Real-Time PII Masking in OBS

Research conducted 2026-03-29. Covers OBS plugins, shaders, IPC patterns, and
non-OBS tools that can be used or adapted for a real-time PII mask filter plugin.

---

## Table of Contents

1. [Top Recommendations (Build-vs-Borrow)](#top-recommendations)
2. [OBS Plugins — Region Masking & Blurring](#obs-plugins-region-masking--blurring)
3. [OBS Plugins — Rounded Rectangle Rendering](#obs-plugins-rounded-rectangle-rendering)
4. [OBS Plugin Architecture References](#obs-plugin-architecture-references)
5. [Shared Memory / IPC with OBS Plugins](#shared-memory--ipc-with-obs-plugins)
6. [GPU Shader Approaches for Rounded Rect Masking](#gpu-shader-approaches)
7. [Non-OBS Implementations to Port From](#non-obs-implementations)
8. [Key Shader Code Collected](#key-shader-code)

---

## Top Recommendations

### Best path: Fork/adapt obs-composite-blur + custom IPC layer

**Why:** obs-composite-blur already implements:
- GPU-accelerated blur (Gaussian, Box, Dual Kawase, Pixelate)
- Rectangle mask with center/width/height positioning
- Crop mask with **corner radius** for rounded corners
- Feathering support
- Proper `video_render()` / `video_tick()` pipeline
- GPL-2.0 license (compatible)
- Cross-platform (macOS, Linux, Windows)

**What we'd add:**
1. Replace static UI-driven mask params with shared-memory reader in `video_tick()`
2. Support an **array** of mask regions (the current plugin does one region)
3. Add SDF-based rounded rect (from obs-advanced-masks) for cleaner corners

### Alternative: Write from scratch using obs-plugintemplate + steal shaders

Take the SDF rounded-rect shader from obs-advanced-masks and the blur
pipeline from obs-composite-blur, wire them to a shared-memory reader.

---

## OBS Plugins — Region Masking & Blurring

### 1. obs-composite-blur (FiniteSingularity) — MOST RELEVANT

- **Repo:** https://github.com/FiniteSingularity/obs-composite-blur
- **License:** GPL-2.0
- **What it does:** Comprehensive blur plugin with multiple algorithms and **mask types**
- **Relevant features:**
  - **Rectangle Mask:** center x/y, width, height — exactly our use case
  - **Crop Mask:** edge insets with **corner radius** for rounded corners
  - **Circle Mask:** center + radius
  - **Source/Image Mask:** use another OBS source as alpha mask
  - Blur algorithms: Gaussian, Box, Dual Kawase, Pixelate
  - All GPU-shader-based, runs in real-time
- **OBS hooks:** `video_render` -> `composite_blur_video_render()`, `video_tick` -> `composite_blur_video_tick()`
- **Key source files:**
  - `src/obs-composite-blur-filter.c` — main filter with render pipeline
  - `data/shaders/effect_mask_crop.effect` — crop mask with corner radius shader
  - `data/shaders/gaussian_1d.effect`, `box_1d.effect`, `dual_kawase_*.effect` — blur shaders
  - `data/shaders/composite.effect` — final compositing
  - `src/blur/gaussian.c`, `box.c`, `dual_kawase.c`, `pixelate.c` — blur implementations
- **Architecture pattern:**
  1. `get_input_source(filter)` — captures source as texture
  2. `filter->video_render(filter)` — applies blur to full frame
  3. `apply_effect_mask(filter)` — masks blur to specified region
  4. `draw_output_to_source(filter)` — writes result

### 2. obs-advanced-masks (FiniteSingularity) — BEST SDF SHAPES

- **Repo:** https://github.com/FiniteSingularity/obs-advanced-masks
- **License:** GPL-2.0
- **What it does:** Advanced masking with SDF-based shape rendering
- **Relevant features:**
  - **SDF Rectangle mask** with per-corner radius, rotation, feathering
  - Uses Inigo Quilez's rounded box SDF (industry standard)
  - Alpha masking AND adjustment masking (brightness/contrast/saturation/hue)
  - Shape masks: Rectangle, Circle, Ellipse, Polygon, Star, Heart
- **Key source files:**
  - `data/shaders/rectangular-mask.effect` — **THE rounded rect SDF shader we want**
  - `src/mask-shape.c` / `.h` — shape mask C implementation
  - `src/advanced-masks-filter.c` — filter pipeline with `video_render`/`video_tick`
  - `data/shaders/common.effect` — shared utilities
- **SDF function (from rectangular-mask.effect):**
  ```hlsl
  // Based on Inigo Quilez: https://iquilezles.org/articles/distfunctions2d/
  float SDF(float2 coord, float2 dims, float4 radii) {
      radii.xy = coord.x > 0.0f ? radii.xy : radii.zw;
      radii.x = coord.y > 0.0f ? radii.x : radii.y;
      float2 dist = abs(coord) - dims + radii.x;
      return min(max(dist.x, dist.y), 0.0f) + length(max(dist, float2(0.0f, 0.0f))) - radii.x;
  }
  ```

### 3. obs-StreamFX (Xaymar) — MATURE BLUR + REGION MASK

- **Repo:** https://github.com/Xaymar/obs-StreamFX
- **License:** GPL-3.0
- **What it does:** Advanced effects plugin with blur filter + region/image/source masking
- **Relevant features:**
  - Blur filter with Region mask (left/top/right/bottom + feather)
  - Image mask and Source mask
  - Multiple blur types: Box, Box Linear, Gaussian, Gaussian Linear, Dual Filtering
  - Well-structured C++ codebase
- **Key source files:**
  - `components/blur/source/filter/filter-blur.cpp` / `.hpp` — blur filter implementation
  - `components/blur/source/gfx/blur/gfx-blur-gaussian.cpp` — Gaussian blur impl
  - `data/effects/mask.effect` — region mask shader (simple rect, no rounded corners)
  - `data/effects/blur/box.effect`, `gaussian.effect` etc. — blur effect shaders
- **Mask shader architecture:** Blends `image_blur` and `image_orig` using `lerp()` based on region test
- **Limitation:** Region mask is axis-aligned rectangle only, no rounded corners

### 4. obs-face-tracker (norihiro)

- **Repo:** https://github.com/norihiro/obs-face-tracker
- **License:** GPL-2.0
- **What it does:** Detects/tracks faces using dlib, applies effects per-frame
- **Relevant to us:** Shows how to run detection in background thread, feed results to `video_render` per-frame
- **Key source files:**
  - `src/face-tracker.cpp` / `.hpp` — main filter with video_render/video_tick
  - `src/face-tracker-manager.cpp` — manages detection thread
  - `src/face-detector-dlib-hog.cpp` — detection backend

### 5. obs-shaderfilter (exeldro) — CUSTOM SHADER PLATFORM

- **Repo:** https://github.com/exeldro/obs-shaderfilter
- **License:** GPL-2.0
- **What it does:** Allows applying arbitrary HLSL shaders to any OBS source
- **Relevant features:**
  - Could use directly with a custom rounded-rect-blur shader
  - Includes example `rounded_rect.shader`, `rounded_rect_per_corner.shader`
  - Includes `gaussian-blur-simple.shader`, `box-blur.shader`
  - Supports uniform parameters exposed as UI controls
- **Key files:**
  - `data/examples/rounded_rect.shader` — rounded rect with border
  - `data/examples/gaussian-blur-simple.shader` — simple gaussian blur
  - `data/examples/circle-mask-filter.shader` — circle mask example
- **Limitation:** Single shader at a time; combining blur + mask would need a single combined shader. No IPC.

### 6. Ashmanix Blur Filter

- **Repo:** https://github.com/ashmanix/blur-filter-obs-plugin
- **License:** GPL-2.0
- **What it does:** Simple blur filter, based on obs-plugintemplate
- **Relevant:** Good example of minimal blur plugin structure. Uses OBS 31+ API.

---

## OBS Plugins — Rounded Rectangle Rendering

### obs-advanced-masks — SDF Rounded Rectangle

The `rectangular-mask.effect` shader implements:
- Per-corner radius (`float4 corner_radius` = TL, TR, BL, BR)
- Rotation (sin_theta, cos_theta)
- Feathering with smoothstep
- Anti-aliasing scale
- Mask inversion

### obs-shaderfilter — Rounded Rect Shaders

Multiple rounded rect shader examples:
- `rounded_rect.shader` — basic rounded corners with optional border
- `rounded_rect2.shader` — variant implementation
- `rounded_rect_per_corner.shader` — per-corner radius control
- `rounded_rect_per_side.shader` — per-side control
- `rounded_stroke.shader` / `rounded_stroke_gradient.shader` — stroke variants

### obs-composite-blur — Crop Mask Corner Radius

The `effect_mask_crop.effect` shader computes corner rounding by:
1. Scaling to 1:1 aspect ratio
2. Checking if pixel is in corner quadrant
3. Computing distance from corner circle center
4. Applying feathering based on distance

---

## OBS Plugin Architecture References

### obs-plugintemplate (Official)

- **Repo:** https://github.com/obsproject/obs-plugintemplate
- **License:** GPL-2.0
- **Structure:**
  - `src/plugin-main.c` — entry point with `obs_module_load()`
  - `buildspec.json` — plugin metadata
  - CMake-based build with GitHub Actions CI
- **To make a video filter, register `obs_source_info` with:**
  ```c
  struct obs_source_info my_filter = {
      .id = "my_filter",
      .type = OBS_SOURCE_TYPE_FILTER,
      .output_flags = OBS_SOURCE_VIDEO,
      .create = my_create,
      .destroy = my_destroy,
      .video_render = my_video_render,
      .video_tick = my_video_tick,
      .get_properties = my_get_properties,
      .update = my_update,
  };
  ```

### obs-backgroundremoval (royshil / occ-ai) — VIDEO RENDER PATTERN

- **Repo:** https://github.com/royshil/obs-backgroundremoval
- **License:** GPL-2.0
- **Architecture pattern (relevant to our IPC design):**
  1. `video_tick()` — runs inference on a background thread, reads input frame, writes mask to `cv::Mat backgroundMask`
  2. `video_render()` — reads mask, creates GPU texture from mask data, applies shader
  3. Uses `std::mutex` to synchronize between inference thread and render thread
  4. Applies Kawase blur for background blur effect
- **Key source files:**
  - `src/background-filter.cpp` — complete filter implementation
  - `src/background-filter.h` — filter data struct
  - `data/effects/blend_images.effect` — compositing shader
  - `data/effects/kawase_blur.effect` — blur shader
  - `data/effects/mask_alpha_filter.effect` — alpha masking
- **This is the closest architecture to what we need:** External data (model output) feeds into `video_tick`, mask geometry applied in `video_render` via GPU shader

### obs-composite-blur — RENDER PIPELINE

Pattern from `composite_blur_video_render()`:
```c
// 1. Capture input source as texture
get_input_source(filter);
// 2. Apply blur algorithm to full frame
filter->video_render(filter);  // calls gaussian/box/kawase render
// 3. Apply region mask (blend blurred + original)
apply_effect_mask(filter);     // crop/rect/circle/source/image
// 4. Draw to output
draw_output_to_source(filter);
```

---

## Shared Memory / IPC with OBS Plugins

### obs-shm-image-source (watfordjc) — SHARED MEMORY PATTERN

- **Repo:** https://github.com/watfordjc/obs-shm-image-source
- **License:** GPL-2.0
- **What it does:** OBS source that reads GPU textures from shared memory
- **Approach:** Windows-only, uses `gs_texture_open_shared()` for D3D11 textures
- **Hooks:** `video_render` with 16ms mutex timeout
- **Status:** Pre-alpha, experimental
- **Limitation:** Windows-only (D3D11 shared textures)

### Recommended IPC Approach for Our Plugin (macOS)

Since we're on macOS and need to read mask **geometry** (not pixels), the best approach is:

1. **POSIX Shared Memory (`shm_open` + `mmap`):**
   ```c
   int fd = shm_open("/pii_mask_regions", O_RDONLY, 0);
   struct mask_region *regions = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
   ```
   - Read in `video_tick()` (called on OBS video thread, ~60fps)
   - Lock-free with atomic sequence counter for synchronization
   - Kernel-managed, survives process restarts

2. **Memory-Mapped File:**
   ```c
   int fd = open("/tmp/pii_mask_regions.bin", O_RDONLY);
   struct mask_region *regions = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
   ```
   - Simpler, filesystem-backed
   - Slightly higher latency but more debuggable

3. **Unix Domain Socket:**
   - Higher latency but reliable delivery
   - Better for variable-length messages
   - Good for initial connection/config, less good for per-frame data

### Data Structure for Shared Memory

```c
#define MAX_MASK_REGIONS 32

struct mask_region {
    float x, y;          // center position (pixels)
    float width, height;  // dimensions (pixels)
    float corner_radius;  // rounded corner radius
    float rotation;       // rotation angle (radians)
    uint32_t flags;       // blur/blackout/pixelate, etc.
};

struct mask_shared_data {
    uint64_t sequence;              // atomic counter for lock-free sync
    uint32_t frame_width;           // source resolution
    uint32_t frame_height;
    uint32_t num_regions;
    struct mask_region regions[MAX_MASK_REGIONS];
};
```

### obs-websocket — Alternative IPC via WebSocket

- **Repo:** https://github.com/obsproject/obs-websocket
- Built into OBS 28+, provides WebSocket server for control
- Could send mask coordinates via WebSocket messages to a custom handler
- Higher latency than shared memory but simpler integration
- Good for initial prototyping, replace with shm for production

---

## GPU Shader Approaches

### SDF Rounded Rectangle (Inigo Quilez)

The industry-standard approach, used by obs-advanced-masks:

```hlsl
float sdRoundedBox(float2 p, float2 b, float4 r) {
    r.xy = (p.x > 0.0) ? r.xy : r.zw;
    r.x  = (p.y > 0.0) ? r.x  : r.y;
    float2 q = abs(p) - b + r.x;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r.x;
}
```
- `p` = pixel position relative to rect center
- `b` = half-dimensions of rectangle
- `r` = corner radii (float4 for per-corner)
- Returns negative inside, positive outside, zero on edge
- Apply `smoothstep` for anti-aliased edges

Reference: https://iquilezles.org/articles/distfunctions2d/

### Blurred Rounded Rectangle (Raph Levien)

For rendering a blurred rounded rect shadow/glow in O(1) per pixel:
- Separate the blur into x and y components
- Use closed-form erf() solution for 1D Gaussian convolved with box
- Effective corner radius: `sqrt(r_c^2 + 1.25 * r_b^2)`

Reference: https://raphlinus.github.io/graphics/2020/04/21/blurred-rounded-rects.html

### Fast Rounded Rectangle Shadows (Evan Wallace)

- Combines analytical x-axis solution with 4-sample y-axis approximation
- Constant time per pixel regardless of blur radius
- Used in production UI rendering

Reference: https://madebyevan.com/shaders/fast-rounded-rectangle-shadows/

### Combined Approach for Our Plugin

For each mask region, the shader would:
1. Compute SDF distance from rounded rect boundary
2. Use smoothstep on SDF for anti-aliased alpha mask
3. Blend between blurred and original texture using alpha
4. Support multiple regions via loop in pixel shader

```hlsl
float4 PSMaskBlur(VertDataOut v_in) : TARGET {
    float4 orig = image_orig.Sample(sampler, v_in.uv);
    float4 blur = image_blur.Sample(sampler, v_in.uv);
    float alpha = 0.0;
    for (int i = 0; i < num_regions; i++) {
        float2 p = rotate(v_in.uv * resolution - regions[i].center, regions[i].angle);
        float d = sdRoundedBox(p, regions[i].half_size, regions[i].radius);
        alpha = max(alpha, 1.0 - smoothstep(-1.0, 1.0, d));
    }
    return lerp(orig, blur, alpha);
}
```

---

## Non-OBS Implementations

### deface — Video Anonymization Tool

- **Repo:** https://github.com/ORB-HD/deface
- **License:** MIT
- **What it does:** CLI tool for face anonymization in video
- **Approach:** CenterFace DNN detection -> bounding box -> Gaussian blur/pixelate/solid
- **Relevant:** Shows detection -> mask coordinate -> blur pipeline pattern
- **Limitation:** Offline processing, not real-time OBS integration

### FFmpeg Region Blur

- **Approach:** `crop` + `boxblur` + `overlay` filter chain
- **Command pattern:**
  ```
  ffmpeg -i input.mp4 -filter_complex \
    "[0:v]crop=W:H:X:Y,boxblur=10[blur]; \
     [0:v][blur]overlay=X:Y" output.mp4
  ```
- **Limitation:** Static coordinates, no rounded corners natively
- **Dynamic masking:** Can read coordinates from JSON per frame
- **Relevant:** Validates the crop->blur->overlay compositing approach

### GStreamer Face Anonymization (Fluendo)

- `flufaceanonymizer` plugin — detection + blur/pixelate per frame
- `faceblur` plugin — built-in face blur
- `gaussianblur` plugin — general gaussian blur element
- **Relevant:** Production-quality real-time pipeline for privacy masking

### macOS Core Image Filters

- `CIRoundedRectangleGenerator` — generates rounded rect images
- `CIMaskedVariableBlur` — blurs based on mask brightness
- `CIGaussianBlur` — standard gaussian blur
- **Pipeline:** Generate rounded rect mask -> use as input to masked blur filter
- **Relevant:** Could be used for macOS-native implementation, but OBS uses its own graphics layer

### WebGL / Canvas Implementations

- **Liquid Glass JS** (https://github.com/dashersw/liquid-glass-js) — WebGL glass effects with rounded rects
- **Shape Lens Blur** (https://tympanus.net/codrops/2024/06/12/shape-lens-blur-effect-with-sdfs-and-webgl/) — SDF + WebGL blur
- **Relevant:** Demonstrates SDF-based shape masking in shader, portable to HLSL

---

## Key Shader Code Collected

### 1. obs-advanced-masks: rectangular-mask.effect (SDF rounded rect)

Full shader at: `data/shaders/rectangular-mask.effect`
- SDF function from Inigo Quilez
- Per-corner radius (float4)
- Rotation support (sin_theta/cos_theta)
- Feathering via smoothstep
- Alpha and adjustment masking modes

### 2. obs-composite-blur: effect_mask_crop.effect (corner radius blur mask)

Full shader at: `data/shaders/effect_mask_crop.effect`
- Blends `image` (original) and `filtered_image` (blurred)
- Corner radius via distance-from-corner-circle
- Feathering based on distance factor
- Mask inversion support

### 3. StreamFX: mask.effect (region blur mask)

Full shader at: `data/effects/mask.effect`
- Simple rect region test (left/top/right/bottom)
- Feathered variant with per-edge feather
- `lerp(orig, blur, alpha)` compositing
- No rounded corners (opportunity for us to improve)

### 4. obs-shaderfilter: rounded_rect.shader

Full shader at: `data/examples/rounded_rect.shader`
- Corner rounding via pixel distance from corner point
- Border drawing support
- Simple and readable implementation

---

## Summary: What to Build

### Architecture

```
[PII Detector Process]
    |
    | writes mask_region structs
    v
[POSIX Shared Memory: /pii_mask_regions]
    |
    | reads in video_tick()
    v
[OBS PII Mask Filter Plugin]
    |
    | video_render():
    |   1. Render source to texture
    |   2. Apply blur to full frame (Kawase or Gaussian)
    |   3. For each region: SDF rounded rect mask
    |   4. Composite: lerp(original, blurred, mask_alpha)
    |   5. Draw to output
    v
[OBS Output / Stream]
```

### Key Code to Steal/Adapt

| Component | Source | Files |
|-----------|--------|-------|
| Plugin boilerplate | obs-plugintemplate | `src/plugin-main.c` |
| Blur algorithms | obs-composite-blur | `src/blur/gaussian.c`, `dual_kawase.c` |
| Blur shaders | obs-composite-blur | `data/shaders/gaussian_1d.effect`, `dual_kawase_*.effect` |
| SDF rounded rect | obs-advanced-masks | `data/shaders/rectangular-mask.effect` |
| Region mask compositing | StreamFX | `data/effects/mask.effect` |
| Mask application pipeline | obs-composite-blur | `src/obs-composite-blur-filter.c` (apply_effect_mask) |
| External data -> render pattern | obs-backgroundremoval | `src/background-filter.cpp` (video_tick + mutex) |
| Shared memory pattern | POSIX | `shm_open()` + `mmap()` |

### All Repos Referenced

| Repo | URL | License |
|------|-----|---------|
| obs-composite-blur | https://github.com/FiniteSingularity/obs-composite-blur | GPL-2.0 |
| obs-advanced-masks | https://github.com/FiniteSingularity/obs-advanced-masks | GPL-2.0 |
| obs-StreamFX | https://github.com/Xaymar/obs-StreamFX | GPL-3.0 |
| obs-backgroundremoval | https://github.com/royshil/obs-backgroundremoval | GPL-2.0 |
| obs-face-tracker | https://github.com/norihiro/obs-face-tracker | GPL-2.0 |
| obs-shaderfilter | https://github.com/exeldro/obs-shaderfilter | GPL-2.0 |
| obs-plugintemplate | https://github.com/obsproject/obs-plugintemplate | GPL-2.0 |
| obs-shm-image-source | https://github.com/watfordjc/obs-shm-image-source | GPL-2.0 |
| obs-websocket | https://github.com/obsproject/obs-websocket | GPL-2.0 |
| blur-filter (standalone) | https://github.com/prgmitchell/blur-filter | GPL-3.0 |
| ashmanix blur filter | https://github.com/ashmanix/blur-filter-obs-plugin | GPL-2.0 |
| deface | https://github.com/ORB-HD/deface | MIT |
| DistroAV (NDI) | https://github.com/DistroAV/DistroAV | GPL-2.0 |
