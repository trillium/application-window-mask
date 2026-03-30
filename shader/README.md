# Shader — GPU Mask Rendering

GPU shaders that apply blur with rounded rectangle masking to video frames.
Used by the plugin during `video_render()`.

## Interface

- **Input uniforms:** Rect parameters (center, size, corner_radius, feathering)
  encoded as a texture (uniform arrays broken on macOS)
- **Input textures:** Original frame, blurred frame
- **Output:** Composited frame with blur applied inside rounded rects

## Visual Spec

- SDF (signed distance function) rounded rectangle per Inigo Quilez
- Supports per-corner radius
- Smooth feathered edges (anti-aliased)
- Composites: `lerp(original, blurred, sdf_alpha)` per pixel
- Must handle up to 32 rects per frame

## Implementations

| Directory | Format | Status |
|-----------|--------|--------|
| `shader_effect/` | OBS .effect (HLSL-like, cross-platform) | — |
| `shader_glsl/` | Raw GLSL | — |
| `shader_metal/` | Native Metal Shading Language | — |
