# SDF Rounded Rectangle Blur Shader — Implementation Options

Research for OBS video filter plugin: GPU shader that applies blur to rounded
rectangular regions of a video frame.

Date: 2026-03-29

---

## Table of Contents

1. [Option Comparison Matrix](#option-comparison-matrix)
2. [Detailed Analysis per Option](#detailed-analysis)
3. [Cross-Cutting Questions](#cross-cutting-questions)
4. [Recommendation](#recommendation)

---

## Option Comparison Matrix

| # | Approach | Shader Lang | Ease Write | Ease Maint | Type Safety | Perf | Memory | Quality | Best For |
|---|----------|------------|------------|------------|-------------|------|--------|---------|----------|
| 1 | IQ SDF rounded rect | OBS .effect (HLSL) | 4 | 5 | 3 | 5 | 5 | 4 | Mask generation |
| 2 | Texture-based mask | OBS .effect | 3 | 3 | 3 | 3 | 2 | 3 | Static masks |
| 3 | Multi-pass Gaussian + SDF | OBS .effect | 3 | 3 | 3 | 2 | 3 | 5 | Max quality |
| 4 | Kawase blur + SDF | OBS .effect | 3 | 4 | 3 | 4 | 4 | 4 | Balanced |
| 5 | Box blur + SDF | OBS .effect | 5 | 5 | 3 | 3 | 4 | 2 | Prototyping |
| 6 | Dual-filter (down/up) + SDF | OBS .effect | 2 | 3 | 3 | 5 | 3 | 4 | Large radii |
| 7 | Single-pass multi-rect | OBS .effect | 4 | 4 | 3 | 4 | 5 | 4 | Few rects |
| 8 | Compute shader | N/A | N/A | N/A | N/A | N/A | N/A | N/A | Not viable |
| 9 | Metal shader (native) | MSL | 2 | 2 | 5 | 5 | 5 | 5 | macOS-only |
| 10 | OBS .effect format | HLSL-like | 3 | 4 | 3 | 4 | 4 | 4 | Cross-platform |

Scale: 1-5, where 5 is best.

---

## Detailed Analysis

### 1. Inigo Quilez SDF Rounded Rectangle

**Shader language:** OBS .effect format (HLSL subset)

**How it works:** Computes the signed distance from each pixel to the nearest
edge of a rounded rectangle using an analytical formula. The SDF returns
negative values inside the shape, zero on the boundary, and positive values
outside. Anti-aliased edges come from `smoothstep` on the distance value. The
mask alpha is then used to lerp between blurred and original textures.

Core function (from obs-advanced-masks `rectangular-mask.effect`):
```hlsl
float SDF(float2 coord, float2 dims, float4 radii) {
    radii.xy = coord.x > 0.0f ? radii.xy : radii.zw;
    radii.x  = coord.y > 0.0f ? radii.x  : radii.y;
    float2 dist = abs(coord) - dims + radii.x;
    return min(max(dist.x, dist.y), 0.0f)
         + length(max(dist, float2(0.0f, 0.0f))) - radii.x;
}
```

**Ease of writing:** 4/5 — Function is ~6 lines. Well-documented by Quilez.
Per-corner radius adds only 2 lines of conditional selection.

**Ease of maintenance:** 5/5 — Pure math, no state, no textures, no passes.
Single function that anyone can reason about.

**Type safety:** 3/5 — OBS .effect HLSL has loose typing. No compile-time
guarantees about uniform binding.

**Performance:** 5/5 — ~10 ALU ops per pixel per rect. No texture reads for
the mask itself. At 1080p (2M pixels) this is negligible. Can evaluate 32+
rects per pixel without bottleneck.

**Memory overhead:** 5/5 — Zero additional textures or framebuffers for the
mask. All computation is in the pixel shader.

**Visual quality:** 4/5 — Excellent sharp edges with `smoothstep` AA.
Feathering controllable via the smoothstep range. Not "blurred" edges like a
true Gaussian convolution against the shape boundary (see option 3 for that).

**Pros:**
- Industry standard (Quilez SDF, used by Figma, Warp, Flutter, etc.)
- Per-corner radius with `float4 radii`
- Rotation trivial (rotate coord before SDF eval)
- obs-advanced-masks already ships this exact code, GPL-2.0
- No texture allocation, no extra passes for the mask
- Composable with any blur algorithm

**Cons:**
- Produces a hard-edge mask (even with smoothstep, it's screen-space AA, not
  a blurred/feathered falloff that matches a Gaussian kernel)
- SDF alone does not blur anything; must be combined with a blur pass

**Key references:**
- Inigo Quilez: https://iquilezles.org/articles/roundedboxes/
- obs-advanced-masks: https://github.com/FiniteSingularity/obs-advanced-masks
- Shader file: `data/shaders/rectangular-mask.effect`
- Shadertoy demo: https://www.shadertoy.com/view/4llXD7

---

### 2. Texture-Based Mask

**Shader language:** OBS .effect format (HLSL subset)

**How it works:** Pre-render rounded rectangles to a CPU-side or GPU-side
texture (e.g., using Cairo, Core Graphics, or a separate render pass). Upload
as a `texture2d` uniform. In the compositing pass, sample this mask texture to
get alpha and lerp between blurred/original.

**Ease of writing:** 3/5 — Shader side is trivial (one texture sample). But
you need code to render the mask texture, upload it, and keep it in sync when
regions change.

**Ease of maintenance:** 3/5 — Two systems to maintain: mask renderer +
compositing shader. Mask texture resolution must match source resolution.

**Type safety:** 3/5 — Same as any OBS .effect.

**Performance:** 3/5 — Extra texture sample per pixel. Mask texture must be
re-rendered when regions change (CPU cost + GPU upload). If regions move every
frame (tracking PII), this is a per-frame CPU-to-GPU upload.

**Memory overhead:** 2/5 — Requires an additional RGBA texture at source
resolution. For 1080p that's ~8MB. Multiple masks = multiple textures or a
single combined mask.

**Visual quality:** 3/5 — Quality depends on mask texture resolution. Can get
aliasing if mask res < source res. Anti-aliasing requires rendering the mask
with AA enabled.

**Pros:**
- Extremely simple compositing shader
- Can represent any arbitrary shape (not just rounded rects)
- Decouples mask generation from compositing

**Cons:**
- Per-frame CPU-to-GPU texture upload for dynamic regions
- Extra memory for mask texture
- Resolution-dependent quality
- More complex pipeline (render mask → upload → composite)
- Overkill for parametric shapes that SDF handles analytically

**Key references:**
- obs-composite-blur source mask: uses another OBS source as mask texture
- obs-backgroundremoval: uploads `cv::Mat` mask to GPU texture each frame

---

### 3. Multi-Pass Gaussian Blur + SDF Composite

**Shader language:** OBS .effect format (HLSL subset)

**How it works:**
1. Pass 1: Render source to texture A
2. Pass 2: Horizontal Gaussian blur of A → texture B
3. Pass 3: Vertical Gaussian blur of B → texture C (separable Gaussian)
4. Pass 4: Composite — evaluate SDF rounded rect, lerp(A, C, mask_alpha)

This is the approach used by obs-composite-blur: blur the entire frame first,
then mask the blur to specific regions.

**Ease of writing:** 3/5 — Each pass is straightforward, but managing 3+
render targets and the pass sequence requires careful OBS API usage
(`gs_texrender_begin/end`, `obs_source_process_filter_begin/end`).

**Ease of maintenance:** 3/5 — Multiple shaders and render targets. Pass
ordering matters. Debugging requires inspecting intermediate textures.

**Type safety:** 3/5 — Standard OBS .effect limitations.

**Performance:** 2/5 — Gaussian blur is O(radius) texture reads per pixel per
axis. A radius-20 blur = 41 taps per axis = 82 texture reads per pixel (or ~42
with linear sampling optimization). At 1080p that's ~84M texture reads. For
large blur radii, this dominates frame time.

Intel benchmarks: AMD R9-290X at 2560x1600, 127x127 kernel = ~3ms. At 1080p
with radius 20-30, expect 0.5-1.5ms.

**Memory overhead:** 3/5 — Needs 2 intermediate render targets at source
resolution (horizontal pass output + final blur output). ~16MB for 1080p RGBA.

**Visual quality:** 5/5 — True Gaussian blur is the gold standard. Separable
implementation is mathematically exact. Smooth, natural-looking blur with no
artifacts.

**Pros:**
- Highest quality blur
- Well-understood algorithm
- Separable = exact Gaussian for any radius
- obs-composite-blur already implements this (steal their shaders)
- Fractional pixel support for smooth animation

**Cons:**
- Slowest blur option
- Cost scales linearly with blur radius
- Multiple render target allocations
- Blurs the entire frame even when only masking small regions
- obs-composite-blur Gaussian shader: `data/shaders/gaussian_1d.effect`

**Key references:**
- obs-composite-blur: https://github.com/FiniteSingularity/obs-composite-blur
- Intel blur benchmark: https://www.intel.com/content/www/us/en/developer/articles/technical/an-investigation-of-fast-real-time-gpu-based-image-blur-algorithms.html

---

### 4. Kawase Blur + SDF Composite

**Shader language:** OBS .effect format (HLSL subset)

**How it works:** Multi-pass filter where each pass samples 4 texels in a
diamond pattern at increasing offsets. Each pass uses the output of the
previous pass as input. Typically 4-6 passes for a visually equivalent blur to
a large Gaussian kernel. Final pass composites with SDF mask.

A 5-pass Kawase with kernels [0,1,2,2,3] needs 20 texture reads total per
pixel (4 reads x 5 passes), compared to 82 for an equivalent Gaussian.

**Ease of writing:** 3/5 — Each pass is simple (4 texture samples + average),
but managing the multi-pass ping-pong between render targets requires OBS
render pipeline knowledge.

**Ease of maintenance:** 4/5 — Each pass shader is tiny. The pass management
code is the complex part.

**Type safety:** 3/5 — Standard OBS .effect.

**Performance:** 4/5 — 1.5-3x faster than Gaussian for equivalent visual
quality (Intel benchmark data). Bilinear sampling hardware does much of the
work. Excellent on integrated GPUs. obs-composite-blur and obs-backgroundremoval
both use Kawase.

**Memory overhead:** 4/5 — Needs 2 ping-pong render targets, same as Gaussian.
But passes are cheaper so you get more blur per unit of GPU time.

**Visual quality:** 4/5 — Very close to Gaussian. Slight artifacts at extreme
radii. Not mathematically exact but indistinguishable in practice for blur
radii up to ~40px.

**Pros:**
- Best performance-to-quality ratio
- Hardware bilinear interpolation exploited
- Already implemented in obs-composite-blur and obs-backgroundremoval
- Good quality across all typical blur radii
- Well-suited for real-time 60fps

**Cons:**
- Not mathematically exact Gaussian
- Discrete step sizes (pass count determines blur level, not continuous)
- Slightly more complex multi-pass management than single-pass box blur

**Key references:**
- obs-composite-blur Kawase: `data/shaders/kawase.effect` (if present) or
  custom implementation
- obs-backgroundremoval: `data/effects/kawase_blur.effect`
- Intel comparison: https://www.intel.com/content/www/us/en/developer/articles/technical/an-investigation-of-fast-real-time-gpu-based-image-blur-algorithms.html

---

### 5. Box Blur + SDF Composite

**Shader language:** OBS .effect format (HLSL subset)

**How it works:** Simple equally-weighted average of surrounding pixels.
Separable (horizontal + vertical passes). Multiple passes converge toward
Gaussian (Central Limit Theorem). 2-pass box blur ~= Gaussian quality.

**Ease of writing:** 5/5 — Simplest blur to implement. Horizontal pass: average
N pixels in a row. Vertical pass: same but column-wise.

**Ease of maintenance:** 5/5 — Dead simple code. Easy to debug.

**Type safety:** 3/5 — Standard OBS .effect.

**Performance:** 3/5 — Same O(radius) as Gaussian per axis, but simpler ALU
(no weights to compute). Can be optimized with "moving average" technique for
O(1) per pixel using compute shaders (but OBS doesn't support compute).
Multi-pass box with small kernel is competitive.

**Memory overhead:** 4/5 — Same 2 render targets as Gaussian/Kawase.

**Visual quality:** 2/5 — Single-pass box blur has visible banding/square
artifacts. 2-pass is decent. 3+ passes approach Gaussian quality but at that
point Kawase is faster.

**Pros:**
- Simplest to implement and debug
- Good for prototyping
- Multi-pass (2-3x) reaches acceptable quality
- obs-composite-blur includes this: `data/shaders/box_1d.effect`

**Cons:**
- Worst quality for single pass
- Square artifacts visible especially at edges
- No real advantage over Kawase at equal quality
- Not the right choice for production PII masking

**Key references:**
- obs-composite-blur: `data/shaders/box_1d.effect`

---

### 6. Dual-Filter Blur (Down/Upsample) + SDF Composite

**Shader language:** OBS .effect format (HLSL subset)

**How it works:** Also called "Dual Kawase." Alternates between downsampling
and upsampling passes:
1. Downsample source to 1/2 res, then 1/4, then 1/8 etc. (each pass samples
   4-5 texels in a pattern and writes to a half-res target)
2. Upsample back: 1/8 → 1/4 → 1/2 → full res (each pass samples 8 texels)
3. The number of down/up levels controls blur radius (each level ~doubles blur)

Blur radius is 2^N, so granularity is coarse. obs-composite-blur fixes this
with linear interpolation on the final downsample step.

**Ease of writing:** 2/5 — Requires managing multiple render targets at
different resolutions. Down/up pass shaders are simple individually, but the
orchestration code is complex. Need to create/manage render targets at 1/2,
1/4, 1/8 etc. resolution.

**Ease of maintenance:** 3/5 — More render targets to manage. Resolution
changes require re-allocating intermediate textures.

**Type safety:** 3/5 — Standard OBS .effect.

**Performance:** 5/5 — Fastest blur algorithm for large radii. Each level
operates on a quarter the pixels of the previous. A 5-level dual-filter
processes: 100% + 25% + 6.25% + 1.56% + 0.39% = ~133% of source pixels total,
with only 4-8 taps each. Dramatically faster than Gaussian at equivalent blur
size. StreamFX reports it runs on mobile GPUs in real-time.

**Memory overhead:** 3/5 — Needs render targets at each mip level (1/2, 1/4,
1/8, etc.). Total memory is ~1.33x source resolution (geometric series). But
more targets to manage.

**Visual quality:** 4/5 — ~98% accurate Gaussian approximation (StreamFX
docs). Very high quality with minimal artifacts. The main issue is coarse blur
radius control (powers of 2) unless you add the linear interpolation step.

**Pros:**
- Fastest blur for large radii (privacy masking often wants heavy blur)
- Scales incredibly well — doubling blur costs one more level, not 2x taps
- obs-composite-blur implements this: `data/shaders/dual_kawase_*.effect`
- StreamFX also implements it
- Works on integrated/mobile GPUs

**Cons:**
- Most complex render pipeline to set up
- Coarse blur granularity without interpolation workaround
- Intermediate render target management adds code complexity
- Downsampling can lose fine detail (acceptable for blur use case)

**Key references:**
- obs-composite-blur dual Kawase: `data/shaders/dual_kawase_down.effect`,
  `dual_kawase_up.effect`
- StreamFX dual filter: https://github.com/Xaymar/obs-StreamFX/wiki/Filter-Blur
- SIGGRAPH 2015 Marius Bjorge (ARM): https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/siggraph2015_2D00_mmg_2D00_marius_2D00_notes.pdf
- Deep dive: https://blog.frost.kiwi/dual-kawase/

---

### 7. Single-Pass vs Multi-Pass for Multiple Rectangles

**Shader language:** OBS .effect format (HLSL subset)

#### Option A: Single-pass multi-rect (loop in pixel shader)

**How it works:** Pass an array of rect parameters (center, size, radii) as
uniforms. In the pixel shader, loop over all rects, evaluate SDF for each,
combine mask alphas (max), then lerp once.

```hlsl
uniform int num_regions;
uniform float4 region_pos_size[MAX_REGIONS];  // xy=center, zw=half_size
uniform float4 region_radii[MAX_REGIONS];

float4 PSMaskBlur(VertDataOut v_in) : TARGET {
    float4 orig = image.Sample(sampler, v_in.uv);
    float4 blur = image_blur.Sample(sampler, v_in.uv);
    float alpha = 0.0;
    for (int i = 0; i < num_regions; i++) {
        float2 p = v_in.uv * resolution - region_pos_size[i].xy;
        float d = SDF(p, region_pos_size[i].zw, region_radii[i]);
        alpha = max(alpha, 1.0 - smoothstep(-1.0, 1.0, d));
    }
    return lerp(orig, blur, alpha);
}
```

**CRITICAL LIMITATION:** OBS .effect format does NOT support uniform arrays on
OpenGL backend. Arrays only work on DirectX. On macOS (OpenGL or Metal), this
approach may fail.

**Workaround for macOS:** Encode rect data into a small texture (e.g., a 1D
texture with 4 pixels per rect). Sample the texture in the loop instead of
indexing a uniform array. This works on all backends.

**Ease of writing:** 4/5 — Shader is simple. Array limitation requires the
texture-encoding workaround.

**Ease of maintenance:** 4/5 — Single shader, single pass for compositing.

**Performance:** 4/5 — One pass, N SDF evaluations per pixel. Each SDF is ~10
ALU ops. For 8 rects, that's ~80 ALU ops per pixel — trivial for any GPU. The
two texture samples (orig + blur) dominate cost.

**Memory overhead:** 5/5 — No extra render targets beyond the blur output.

**Visual quality:** 4/5 — Same as single-rect SDF.

#### Option B: Multi-pass (one pass per rect)

**How it works:** For each rect, run a separate compositing pass that reads
the current composited output and applies one more rect mask.

**Performance:** 3/5 — Each pass = full-screen texture read + write. For 8
rects = 8 full-screen passes. At 1080p that's 8 x 2M pixels x 2 texture
ops = 32M texture ops just for compositing.

**Memory overhead:** 3/5 — Needs ping-pong render targets.

**Verdict:** Single-pass is strongly preferred. Use texture-encoded parameters
to work around OBS array limitations on macOS.

**Key references:**
- OBS-SL array limitation: https://github.com/Xaymar/obs-StreamFX/wiki/OBS-Shading-Language
- OBS .effect docs: https://docs.obsproject.com/reference-libobs-graphics-effects

---

### 8. Compute Shader Approach

**Shader language:** N/A

**How it works:** Would use GPU compute dispatches for blur (shared memory tiling,
workgroup-local caching of texture data) and SDF evaluation.

**Verdict: NOT VIABLE for OBS.**

The OBS `libobs/graphics/graphics.h` API exposes **zero** compute shader
functionality. There are no `gs_compute_*` functions, no dispatch calls, no
UAV/RWTexture support, and no GPGPU primitives. The graphics abstraction is
exclusively rasterization-based (vertex + pixel shaders).

This is true across all three backends:
- **D3D11** (`libobs-d3d11`): Could theoretically support compute (D3D11 has
  CS 5.0), but OBS doesn't expose it
- **OpenGL** (`libobs-opengl`): No compute shader wrappers
- **Metal** (`libobs-metal`): New experimental renderer, no compute support

**Would require:** Forking OBS or using raw graphics API calls outside the OBS
abstraction layer — fragile and not maintainable.

**Ease of writing:** N/A
**Performance:** N/A (would be 5/5 if available — shared memory tiling is the
fastest blur possible)
**All other dimensions:** N/A

**Key references:**
- OBS graphics API: https://github.com/obsproject/obs-studio/blob/master/libobs/graphics/graphics.h
- No compute shader declarations in the header

---

### 9. Metal Shader (Native)

**Shader language:** MSL (Metal Shading Language, C++14-based)

**How it works:** Write shaders directly in MSL for the new OBS Metal renderer
backend (OBS 32.0+, experimental on macOS).

**Ease of writing:** 2/5 — MSL is stricter than HLSL:
- No global uniforms (must pass as buffer arguments)
- Struct definitions cannot be shared between input/output
- Different intrinsic function names
- Must learn Metal-specific texture/sampler API
- The OBS Metal transpiler rewrites HLSL to MSL at runtime; writing native
  MSL means bypassing this transpiler and losing cross-platform support

**Ease of maintenance:** 2/5 — macOS-only. Would need to maintain parallel
HLSL shaders for Windows/Linux, or rely on the transpiler for those platforms
while hand-writing Metal shaders.

**Type safety:** 5/5 — MSL is based on C++14 with strict typing. Catches many
errors at compile time that HLSL silently promotes.

**Performance:** 5/5 — Native Metal shaders avoid transpilation overhead. Can
use Metal-specific optimizations. But in practice, the transpiled HLSL is
nearly as fast — the bottleneck is texture bandwidth, not ALU.

**Memory overhead:** 5/5 — Same as any other approach for the algorithm used.

**Visual quality:** 5/5 — Same math, same output.

**Pros:**
- Best type safety
- Native performance on macOS
- Access to Metal-specific features (threadgroup memory, etc.)

**Cons:**
- macOS-only — breaks cross-platform support entirely
- OBS Metal renderer is still experimental (OBS 32.0+)
- Third-party shader compatibility is known-broken (GitHub issue #12709)
- Plugin authors must work with OBS team to fix transpiler issues
- Maintaining two shader codebases (MSL + HLSL) is unsustainable
- No ecosystem of OBS plugins written in native MSL to reference

**Verdict:** Do NOT write native Metal shaders. Write OBS .effect (HLSL) and
let the transpiler handle Metal. File issues with OBS if transpilation breaks.

**Key references:**
- OBS Metal renderer: https://obsproject.com/blog/obs-studio-gets-a-new-renderer
- Third-party shader issues: https://github.com/obsproject/obs-studio/issues/12709
- Plugin rendering issues: https://github.com/obsproject/obs-studio/issues/12705

---

### 10. OBS .effect File Format

**Shader language:** "OBS-SL" — HLSL subset with cross-platform transpilation

**How it works:** OBS .effect files use Direct3D 11 HLSL syntax. OBS
transpiles these to GLSL (OpenGL backend) or MSL (Metal backend) at runtime
via string substitution and AST rewriting.

**File structure:**
```hlsl
// Uniforms
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float2 uv_size;

// Sampler states
sampler_state texSampler {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

// Vertex shader
struct VertData {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

VertData VSDefault(VertData v_in) {
    VertData vert_out;
    vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
    vert_out.uv  = v_in.uv;
    return vert_out;
}

// Pixel shader
float4 PSMain(VertData v_in) : TARGET {
    return image.Sample(texSampler, v_in.uv);
}

// Technique
technique Draw {
    pass {
        vertex_shader = VSDefault(v_in);
        pixel_shader  = PSMain(v_in);
    }
}
```

**Supported types:** bool, float, int, float2/3/4, int2/3/4, float3x3,
float3x4, float4x4, texture2d, texture3d, sampler_state

**Key limitations:**
- Uniform arrays only work on DirectX (NOT OpenGL, NOT Metal)
- No `float2x2` matrix type
- No function overloading
- No `#if defined()` preprocessor
- GLSL transpilation is primitive string substitution — complex HLSL may break
- Metal transpilation rewrites structs and global variables — may break
  third-party shaders
- Always use HLSL intrinsics (`lerp` not `mix`, `frac` not `fract`, `saturate`
  not `clamp(x,0,1)`)
- `texture_rect` is OpenGL-only
- Border address mode is DirectX-only

**Techniques and passes:** A technique named `Draw` is the default for video
filters (called by `obs_source_process_filter_end`). Multiple passes are
supported for multi-pass effects.

**How backends see it:**
- **DirectX 11** (Windows): Native HLSL, compiled by D3D compiler
- **OpenGL** (Linux, older macOS): Transpiled to GLSL 3.30 via string
  substitution. Prepends `#version 330`. Line numbers shift (debugging harder).
- **Metal** (macOS 32.0+): Transpiled to MSL. Struct splitting, global
  variable elimination, function signature rewriting. Third-party compatibility
  is fragile.

**Ease of writing:** 3/5 — Familiar if you know HLSL. The cross-platform
gotchas require testing on multiple backends.

**Ease of maintenance:** 4/5 — Single file, single language. But must test on
all three backends.

**Type safety:** 3/5 — HLSL is loosely typed. Silent promotions. No
compile-time uniform binding validation.

**Performance:** 4/5 — Transpilation overhead is negligible (happens once at
load time). Runtime performance is native for each backend.

**Key references:**
- OBS effects docs: https://docs.obsproject.com/reference-libobs-graphics-effects
- OBS-SL wiki: https://github.com/Xaymar/obs-StreamFX/wiki/OBS-Shading-Language
- Shadertastic syntax guide: https://doc.shadertastic.com/effect-development/shader-syntax/
- OBS rendering guide: https://docs.obsproject.com/graphics

---

## Cross-Cutting Questions

### Can OBS shaders do per-corner radius?

**Yes.** The SDF approach natively supports per-corner radius via a `float4`
parameter where each component maps to one corner:

```hlsl
float SDF(float2 coord, float2 dims, float4 radii) {
    // radii = (top-right, bottom-right, bottom-left, top-left)
    radii.xy = coord.x > 0.0f ? radii.xy : radii.zw;  // right vs left
    radii.x  = coord.y > 0.0f ? radii.x  : radii.y;   // top vs bottom
    ...
}
```

obs-advanced-masks ships this exact implementation. The Quilez formula handles
it with just 2 conditional selections before the distance computation.

The texture-based mask approach can also do per-corner radius, but requires
re-rendering the mask texture whenever radii change.

The obs-composite-blur crop mask does NOT support per-corner radius (single
`corner_radius` uniform).

---

### How do existing plugins handle feathering/anti-aliasing at mask edges?

Three approaches observed in the wild:

1. **SDF + smoothstep (obs-advanced-masks):**
   ```hlsl
   float mask = smoothstep(0.0, feather_amount, -sdf_distance);
   ```
   When `feather_amount > 0`, the mask fades over a pixel range proportional
   to the feather value. When `feather_amount = 0`, it falls back to a hard
   edge with screen-space AA using `fwidth()`.

2. **Distance from corner circle (obs-composite-blur):**
   Uses Euclidean distance from the corner circle center, then applies a
   feather factor based on that distance. Less mathematically clean than SDF
   but produces acceptable results.

3. **fwidth-based screen-space AA (general technique):**
   ```hlsl
   float aa = fwidth(sdf_distance);
   float mask = smoothstep(aa, -aa, sdf_distance);
   ```
   Automatically adapts anti-aliasing width to screen-space pixel density.
   Works at any zoom level. Cost: 1 `fwidth` call (computes `abs(dFdx) +
   abs(dFdy)`), which is essentially free on modern GPUs (computed from
   neighboring fragment shader invocations).

**Recommendation:** Use approach 3 (`fwidth`) as baseline AA, with an
additional `feather_amount` uniform for user-controlled soft edges:
```hlsl
float aa = max(fwidth(d), 0.5);  // minimum 0.5px AA
float mask = smoothstep(feather_amount + aa, -aa, d);
```

---

### What's the GPU cost of SDF evaluation per pixel at 1080p60?

**Negligible.** The SDF rounded rect function is ~10 ALU operations:
- 2 conditional moves (corner selection)
- 2 absolute values
- 2 subtractions
- 1 max (component-wise)
- 1 length (sqrt of dot product)
- 1 min
- 1 subtraction

At 1080p (1920x1080 = 2,073,600 pixels) at 60fps:
- 2M pixels x 10 ALU ops = 20M ALU ops per frame
- Modern GPUs (even integrated like Apple M1) can do ~1 TFLOP = 1,000,000M
  FLOPS
- SDF eval takes ~0.02ms per frame — invisible in profiling

Even with 32 rectangles evaluated per pixel:
- 2M x 10 x 32 = 640M ALU ops per frame
- Still ~0.64ms on a 1 TFLOP GPU
- Well within budget for 60fps (16.67ms frame budget)

**The bottleneck is always texture bandwidth (blur passes), never SDF math.**

---

### How many rounded rects can we mask per frame before GPU bottleneck?

**SDF evaluation alone:** 100+ rects per pixel is feasible. At 10 ALU ops
each, 100 rects = 1000 ops/pixel = ~2ms at 1080p on integrated GPU. In
practice, you'll run out of uniform space before you run out of GPU cycles.

**With blur (the real bottleneck):** The blur is applied to the full frame
once, regardless of rect count. The compositing pass with N SDF evaluations
adds minimal cost. So rect count primarily affects:

1. **Uniform data transfer** — ~32 bytes per rect (center, size, radii,
   rotation). 32 rects = 1KB. Trivial.
2. **Loop unrolling** — GPU shader compilers unroll small loops. Beyond ~16-32
   iterations, some compilers fall back to true loops which are slower.
3. **Texture-encoded parameters** — If using texture for rect data (macOS
   workaround), each rect = 1 texture read in the loop. 32 extra texture
   reads per pixel is still well within budget.

**Practical limit:** 32 rects is a safe upper bound for a hard-coded
`MAX_REGIONS`. This handles any realistic PII masking scenario (32 simultaneous
faces/cards/SSNs on screen). The shader should use `[loop]` attribute and
early-out when `i >= num_regions`.

**Recommended approach:**
```hlsl
#define MAX_REGIONS 32
// Encode rect params in a small texture (4 pixels per rect = 128 pixels)
uniform texture2d region_data;
uniform int num_regions;

[loop] for (int i = 0; i < MAX_REGIONS; i++) {
    if (i >= num_regions) break;
    // Read rect params from texture
    float4 pos_size = region_data.Load(int3(i * 2, 0, 0));
    float4 radii    = region_data.Load(int3(i * 2 + 1, 0, 0));
    ...
}
```

---

## Recommendation

### Optimal architecture: Dual Kawase blur + SDF composite (single-pass multi-rect)

This combines options **6 + 1 + 7A** for the best balance:

| Component | Choice | Why |
|-----------|--------|-----|
| **Blur algorithm** | Dual Kawase (option 6) | Fastest for large radii, already in obs-composite-blur |
| **Mask function** | IQ SDF rounded rect (option 1) | Per-corner radius, zero memory, ~10 ALU ops |
| **Multi-rect** | Single-pass with texture-encoded params (option 7A) | Works on macOS, no extra passes |
| **Shader format** | OBS .effect (option 10) | Cross-platform, maintained by OBS team |
| **AA/feathering** | fwidth + smoothstep | Automatic screen-space AA, controllable feather |

### Pipeline:

```
Pass 1-N: Dual Kawase downsample/upsample → blurred texture
Pass N+1: Composite shader
  - Sample original texture
  - Sample blurred texture
  - For each rect: evaluate SDF, accumulate mask alpha
  - lerp(original, blurred, mask_alpha)
  - Output
```

### Code to adapt from existing plugins:

| Component | Source repo | File |
|-----------|------------|------|
| Dual Kawase shaders | obs-composite-blur | `data/shaders/dual_kawase_down.effect`, `dual_kawase_up.effect` |
| Dual Kawase C pipeline | obs-composite-blur | `src/blur/dual_kawase.c` |
| SDF function | obs-advanced-masks | `data/shaders/rectangular-mask.effect` |
| Filter boilerplate | obs-composite-blur | `src/obs-composite-blur-filter.c` |
| Region data encoding | Custom | Encode rect array into small texture for macOS compat |

### Fallback option: Kawase blur (option 4)

If Dual Kawase's multi-resolution render target management is too complex for
initial implementation, standard Kawase blur (option 4) is simpler to set up
(fixed-resolution ping-pong) and still 1.5-3x faster than Gaussian. Start with
Kawase, upgrade to Dual Kawase if blur radius needs exceed ~30px or performance
is tight.

---

## Sources

- [Inigo Quilez — Rounded Boxes SDF](https://iquilezles.org/articles/roundedboxes/)
- [obs-advanced-masks](https://github.com/FiniteSingularity/obs-advanced-masks)
- [obs-composite-blur](https://github.com/FiniteSingularity/obs-composite-blur)
- [obs-StreamFX](https://github.com/Xaymar/obs-StreamFX)
- [OBS Effects/Shaders docs](https://docs.obsproject.com/reference-libobs-graphics-effects)
- [OBS Shading Language (StreamFX wiki)](https://github.com/Xaymar/obs-StreamFX/wiki/OBS-Shading-Language)
- [OBS Metal renderer announcement](https://obsproject.com/blog/obs-studio-gets-a-new-renderer)
- [OBS Metal shader compat issue #12709](https://github.com/obsproject/obs-studio/issues/12709)
- [Intel blur algorithm investigation](https://www.intel.com/content/www/us/en/developer/articles/technical/an-investigation-of-fast-real-time-gpu-based-image-blur-algorithms.html)
- [Dual Kawase deep dive (frost.kiwi)](https://blog.frost.kiwi/dual-kawase/)
- [Raph Levien — Blurred Rounded Rects](https://raphlinus.github.io/graphics/2020/04/21/blurred-rounded-rects.html)
- [Warp — SDF Rectangles in Metal](https://www.warp.dev/blog/how-to-draw-styled-rectangles-using-the-gpu-and-metal)
- [SDF Anti-Aliasing techniques](https://drewcassidy.me/2020/06/26/sdf-antialiasing/)
- [Shadertoy — Rounded Box SDF](https://www.shadertoy.com/view/4llXD7)
- [Shadertastic — OBS shader syntax](https://doc.shadertastic.com/effect-development/shader-syntax/)
- [OBS-shaderfilters rounded_rect.shader](https://github.com/Oncorporation/OBS-shaderfilters/blob/master/rounded_rect.shader)
- [Shape Lens Blur with SDFs (Codrops)](https://tympanus.net/codrops/2024/06/12/shape-lens-blur-effect-with-sdfs-and-webgl/)
- [ARM SIGGRAPH 2015 — Bandwidth-efficient rendering (Dual Kawase origin)](https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/siggraph2015_2D00_mmg_2D00_marius_2D00_notes.pdf)
