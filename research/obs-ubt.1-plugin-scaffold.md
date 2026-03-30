# OBS Video Filter Plugin Scaffold — Research

**Date:** 2026-03-29
**Target:** OBS Studio 32.1.0 (installed on this machine)
**Goal:** Custom video filter plugin with `video_tick`/`video_render` hooks for PII masking

---

## Environment

- **Installed OBS version:** 32.1.0
- **Platform:** macOS (Darwin 25.3.0, Apple Silicon)
- **Existing user plugins:** `obs-backgroundremoval.plugin` at `~/Library/Application Support/obs-studio/plugins/`
- **Bundled plugins location:** `/Applications/OBS.app/Contents/PlugIns/`
- **Current architecture:** File-based mask (PNG on disk, read by `mask_filter_v2`) — see `ARCHITECTURE.md` Phase 4 for plugin motivation

---

## OBS API Targeting

OBS moved to SemVer at version 30. The current latest is **32.1.0**. The plugin API is stable across 28-32 for `obs_source_info`-based filters. Key considerations:

- `obs_source_info` struct has not had breaking changes — new fields are additive
- `obs_register_source()` is the registration function, called from `obs_module_load()`
- The `version` field in `obs_source_info` controls settings migration — start at 0
- API docs: https://docs.obsproject.com/reference-sources

**Recommendation:** Target OBS 30+ (SemVer era). This covers 30, 31, 32 and is what most users run. The plugintemplate CI builds against the latest OBS release.

---

## Minimum Viable Video Filter

A video filter in OBS is a source of `type = OBS_SOURCE_TYPE_FILTER` with `output_flags = OBS_SOURCE_VIDEO`. The minimum callbacks:

```c
#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("pii-mask", "en-US")

struct pii_mask_data {
    obs_source_t *source;
    gs_effect_t  *effect;
};

static const char *pii_mask_get_name(void *unused) {
    return "PII Mask";
}

static void *pii_mask_create(obs_data_t *settings, obs_source_t *source) {
    struct pii_mask_data *d = bzalloc(sizeof(*d));
    d->source = source;
    return d;
}

static void pii_mask_destroy(void *data) {
    bfree(data);
}

static void pii_mask_video_tick(void *data, float seconds) {
    // Called every frame — update mask state here
}

static void pii_mask_video_render(void *data, gs_effect_t *effect) {
    // Called during render — apply filter
    struct pii_mask_data *d = data;
    obs_source_t *target = obs_filter_get_target(d->source);
    if (!target) {
        obs_source_skip_video_filter(d->source);
        return;
    }
    // Pass-through: render parent unchanged
    if (!obs_source_process_filter_begin(d->source, GS_RGBA,
                                          OBS_ALLOW_DIRECT_RENDERING))
        return;
    obs_source_process_filter_end(d->source,
        obs_get_base_effect(OBS_EFFECT_DEFAULT), 0, 0);
}

static struct obs_source_info pii_mask_filter = {
    .id             = "pii_mask_filter",
    .type           = OBS_SOURCE_TYPE_FILTER,
    .output_flags   = OBS_SOURCE_VIDEO,
    .get_name       = pii_mask_get_name,
    .create         = pii_mask_create,
    .destroy        = pii_mask_destroy,
    .video_tick     = pii_mask_video_tick,
    .video_render   = pii_mask_video_render,
};

bool obs_module_load(void) {
    obs_register_source(&pii_mask_filter);
    return true;
}
```

**That's ~50 lines for a no-op pass-through filter.** Adding properties, settings, and actual masking logic grows it, but the scaffold is small.

---

## Option Comparison Matrix

### 1. C Plugin Using obs-plugintemplate

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/tooling** | C11, CMake, obs-plugintemplate | |
| **How it works** | Fork the template repo, fill in `obs_source_info` callbacks. CMake builds a `.so`/`.dylib`/`.dll`. | |
| **Ease of writing** | 3/5 | Manual memory management, but OBS API is C-native so no impedance mismatch |
| **Ease of maintenance** | 3/5 | C is verbose but the plugin API is stable |
| **Type safety** | 2/5 | C has minimal type safety, `void*` everywhere |
| **Performance** | 5/5 | Zero overhead — direct API calls, no FFI, no GC |
| **Memory overhead** | 5/5 | No runtime, no allocator overhead |
| **Build complexity** | 3/5 | CMake + obs-plugintemplate handles CI/CD for Win/Mac/Linux. Initial setup takes ~30min following the wiki. |
| **Pros** | Native API match. All OBS examples/docs are in C. Maximum community support. Template has CI/CD for all 3 platforms with codesigning. |
| **Cons** | Manual memory management. Verbose. No RAII. |
| **Key reference** | https://github.com/obsproject/obs-plugintemplate |

### 2. C++ Plugin

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/tooling** | C++17, CMake | |
| **How it works** | Same as C plugin but wrap OBS C API in C++ classes. Still register via `obs_source_info` (C struct). Many existing plugins use this approach (obs-backgroundremoval, obs-composite-blur). | |
| **Ease of writing** | 4/5 | RAII, smart pointers, std::string, lambdas reduce boilerplate |
| **Ease of maintenance** | 4/5 | Better abstractions than C, but still need C interop at the boundary |
| **Type safety** | 3/5 | Templates and strong typing help, but OBS API boundary is still `void*` |
| **Performance** | 5/5 | Same as C — compiles to native, no overhead |
| **Memory overhead** | 5/5 | Same as C |
| **Build complexity** | 3/5 | Same CMake setup. Minor: need to `extern "C"` the module entry points. |
| **Pros** | Used by most complex OBS plugins (obs-backgroundremoval, obs-composite-blur). Better abstractions. Can use ONNX Runtime, OpenCV directly. |
| **Cons** | OBS API is still C — wrapping it adds a thin layer. Slightly longer compile times. |
| **Key reference** | https://github.com/royshil/obs-backgroundremoval (C++ filter plugin) |

### 3. Rust Plugin (obs-rs / rust-obs-plugins)

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/tooling** | Rust, Cargo + CMake (hybrid), FFI bindings | |
| **How it works** | Use `rust-obs-plugins` crate (bennetthardwick) which provides safe wrappers. Implement `Module`, `Sourceable`, `GetNameSource`, `VideoRenderSource` traits. Compiles to `cdylib` that OBS loads. | |
| **Ease of writing** | 2/5 | Trait-based API is clean but: incomplete bindings, sparse docs, small community. Debugging FFI issues is painful. |
| **Ease of maintenance** | 3/5 | Rust's safety helps long-term, but bindings may lag OBS releases |
| **Type safety** | 5/5 | Rust's type system catches most bugs at compile time |
| **Performance** | 5/5 | Same as C/C++ — compiles to native |
| **Memory overhead** | 5/5 | No GC, no runtime |
| **Build complexity** | 2/5 | Cargo for Rust + need libobs headers/linkage. Cross-compilation harder. No official template with CI/CD. Must manually set up codesigning. |
| **Pros** | Memory safety without GC. No null pointer bugs. Strong ecosystem for async, networking. |
| **Cons** | `rust-obs-plugins` is incomplete and the author acknowledges "likely API changes." Only 2 example plugins (scroll-focus, rnnoise). No macOS examples. No official OBS support for Rust. Debugging crashes in FFI boundary is hard. `obs-rs` (OtaK) appears abandoned. |
| **Key references** | https://github.com/bennetthardwick/rust-obs-plugins, https://github.com/OtaK/obs-rs, https://crates.io/crates/libobs-wrapper |

### 4. OBS Lua Script

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/tooling** | Lua 5.1 (LuaJIT), OBS built-in scripting | |
| **How it works** | Lua scripts CAN register custom sources including filters with `video_render` and `video_tick`. The halftone filter tutorial demonstrates a full Lua video filter with shader effects. Register a `source_info` table with `obs.obs_register_source(source_info)`. | |
| **Ease of writing** | 5/5 | No compilation. Drop a `.lua` file into scripts. Iterate by reloading in OBS. |
| **Ease of maintenance** | 4/5 | Simple, readable. But: no type checking, limited debugging. |
| **Type safety** | 1/5 | Dynamic typing, no compile-time checks |
| **Performance** | 2/5 | LuaJIT is fast for scripting but: mutex contention with OBS sources_mutex causes deadlocks. Known bug: `video_tick` + properties that enumerate sources = infinite mutex lock. Per-frame Lua calls add overhead. |
| **Memory overhead** | 4/5 | LuaJIT is lightweight |
| **Build complexity** | 5/5 | Zero build. Just a `.lua` file. |
| **Pros** | Fastest iteration cycle. No build system. Halftone tutorial is a working video filter example. Can use HLSL shaders via effect files. |
| **Cons** | **Known deadlock bug** when combining `video_tick` with source enumeration in properties. Performance ceiling for complex per-pixel operations. Can't call external C libraries easily. Limited to what OBS scripting API exposes. |
| **Key references** | https://docs.obsproject.com/scripting, https://github.com/obsproject/obs-studio/wiki/scripting-tutorial-halftone-filter-listing |

**CRITICAL for our use case:** Lua video filters work via shaders (HLSL effect files). For PII masking, we'd need to pass mask data (from shared memory or socket) into the shader as a texture uniform. Lua CAN set shader parameters and load textures, but the mask data pipeline (IPC from the daemon) would be awkward in Lua — no direct shared memory access, no Unix sockets.

### 5. OBS Python Script

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/tooling** | Python 3.x, OBS built-in scripting | |
| **How it works** | Python scripts can automate OBS but **cannot register custom sources**. Only Lua can use `obs_register_source()`. Python lacks `video_render`/`video_tick` callbacks entirely. | |
| **Ease of writing** | N/A | **Cannot implement a video filter.** |
| **Ease of maintenance** | N/A | |
| **Type safety** | N/A | |
| **Performance** | N/A | |
| **Memory overhead** | N/A | |
| **Build complexity** | N/A | |
| **Pros** | Good for automation scripts (scene switching, hotkeys, OBS control). |
| **Cons** | **Fundamentally cannot create video filters.** No `video_render`, no `video_tick`, no source registration. |
| **Key reference** | https://docs.obsproject.com/scripting ("Script Sources (Lua Only)") |

**Verdict: Eliminated.** Python scripts cannot implement video filters.

### 6. Fork obs-composite-blur

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/tooling** | C, CMake, GPL-2.0 | |
| **How it works** | Fork the repo, strip blur-specific code, keep the plugin scaffold and GPU effect infrastructure. Has working `video_render` with multiple shader effects, masking support, and proper compositing. | |
| **Ease of writing** | 4/5 | Starting from working code with 4 blur algorithms + masking already built |
| **Ease of maintenance** | 3/5 | Inherited codebase may have assumptions you don't need. Must understand someone else's architecture. |
| **Type safety** | 2/5 | C codebase |
| **Performance** | 5/5 | GPU-accelerated, optimized with linear sampling |
| **Memory overhead** | 5/5 | Native C |
| **Build complexity** | 3/5 | CMake with buildspec.json. Supports OBS 28-30. Has CI/CD. |
| **Pros** | Already has masking support (mask sources as inputs). Already has GPU effect pipeline. Already has proper compositing. Multi-platform CI. Good starting point if we want blur + mask compositing. |
| **Cons** | GPL-2.0 license (copyleft). Substantial codebase to understand before modifying. May carry assumptions about blur that don't apply to PII masking. |
| **Key reference** | https://github.com/FiniteSingularity/obs-composite-blur |

### 7. Fork obs-backgroundremoval

| Dimension | Rating | Notes |
|-----------|--------|-------|
| **Language/tooling** | C++, CMake, GPL-3.0 | |
| **How it works** | Fork the repo, replace ML model inference with our daemon's mask data. Plugin already has video frame capture, GPU effects, mask application, and compositing. | |
| **Ease of writing** | 3/5 | Complex codebase with ONNX Runtime, OpenCV, multiple model backends. More to strip out. |
| **Ease of maintenance** | 2/5 | Heavy dependency tree (ONNX Runtime, OpenCV, CoreML). Much of this is unnecessary for our use case. |
| **Type safety** | 3/5 | C++ |
| **Performance** | 5/5 | Native, GPU-accelerated |
| **Memory overhead** | 3/5 | ONNX Runtime + OpenCV are heavy (hundreds of MB). Stripping them helps but adds work. |
| **Build complexity** | 2/5 | Complex dependency management via vcpkg. ONNX Runtime builds are notoriously painful. |
| **Pros** | Already installed on this machine. Already working. Has the mask-application pipeline we need. C++ is easier to extend than C. Has a walkthrough blog post. |
| **Cons** | GPL-3.0 (strongest copyleft). Massive dependency tree for features we don't need. Complex build with vcpkg triplets. Overkill for our use case. |
| **Key references** | https://github.com/royshil/obs-backgroundremoval, https://www.morethantechnical.com/blog/2023/05/20/building-an-obs-background-removal-plugin-a-walkthrough/ |

---

## Build System Comparison

### CMake (Recommended)

- **The official choice.** OBS itself uses CMake. obs-plugintemplate uses CMake. Every major OBS plugin uses CMake.
- **obs-plugintemplate** provides CMakePresets.json, buildspec.json, and CI/CD workflows for all 3 platforms.
- macOS codesigning/notarization is integrated into the template's GitHub Actions.
- **Verdict:** Use CMake. Fighting the ecosystem is not worth it.

### Meson

- Cleaner syntax than CMake. Faster builds. Python-like DSL.
- **No OBS ecosystem support.** No template, no CI/CD examples, no community plugins using it.
- Would need to write all OBS integration from scratch.
- **Verdict:** Not worth the trailblazing cost for this project.

### Cargo (Rust)

- Excellent for Rust code, but OBS plugin loading expects a shared library with C ABI entry points.
- Needs hybrid build: Cargo for Rust compilation, then manual or CMake integration for plugin packaging.
- **Verdict:** Only if choosing Rust (Option 3).

### Makefile / bare build

- Maximum control, minimum magic.
- Would need to replicate everything the template gives you (platform detection, codesigning, packaging).
- **Verdict:** No.

---

## macOS-Specific Considerations

### Plugin Loading Paths

1. **User plugins:** `~/Library/Application Support/obs-studio/plugins/<name>.plugin/`
   - This is a macOS bundle with `Contents/{MacOS,Resources}` structure
   - The `.plugin` directory IS the bundle — it must contain `Contents/MacOS/<binary>` and `Contents/Info.plist`
2. **System plugins:** `/Applications/OBS.app/Contents/PlugIns/<name>.plugin/`
3. **Custom paths:** Set `OBS_PLUGINS_PATH` and `OBS_PLUGINS_DATA_PATH` env vars

### Bundle Structure (observed from obs-backgroundremoval.plugin)

```
obs-backgroundremoval.plugin/
  Contents/
    Info.plist
    MacOS/           # Binary (.dylib renamed to match bundle)
    Resources/       # Data files (shaders, locale, models)
    Frameworks/      # Bundled dylibs (ONNX Runtime, etc.)
    _CodeSignature/  # codesign artifacts
```

### Codesigning & Notarization

- Required for distribution. Users get Gatekeeper warnings without it.
- **For local development:** Not needed. OBS loads unsigned plugins from `~/Library/Application Support/obs-studio/plugins/` without issue.
- For CI/CD distribution:
  - Developer ID Application certificate (signs the binary)
  - Developer ID Installer certificate (signs the .pkg)
  - Notarization via `xcrun notarytool` with Apple ID credentials
  - obs-plugintemplate has this fully automated in GitHub Actions
  - Secrets needed: `CODESIGN_IDENTITY`, `CODESIGN_TEAM`, `MACOS_NOTARIZATION_USERNAME`, `MACOS_NOTARIZATION_PASSWORD`

### Apple Silicon

- OBS 28+ supports Apple Silicon natively
- Build as universal binary (`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`) or arm64-only
- obs-plugintemplate handles this via CMakePresets

---

## Recommendation

### For pii_mask Phase 4: **Option 1 (C plugin via obs-plugintemplate)** or **Option 2 (C++ plugin)**

**Rationale:**

1. **We need IPC** — the mask daemon sends mask data, the plugin receives it. C/C++ can do shared memory (`mmap`/`shm_open`), Unix sockets, or memory-mapped files trivially. Lua cannot.

2. **We need `video_render`** — to apply the mask as a texture and composite it with the source. C/C++ is the native API for this. Lua has deadlock risks.

3. **We don't need ML inference** — ruling out the complexity of obs-backgroundremoval's dependencies.

4. **We don't need blur algorithms** — ruling out obs-composite-blur as a starting point (though its mask compositing code is worth studying).

5. **obs-plugintemplate** gives us CI/CD, codesigning, and cross-platform support out of the box.

6. **Rust** is tempting for safety but the bindings are immature and debugging FFI issues is not worth it for a plugin this small.

### Suggested approach:

```
1. Fork obs-plugintemplate
2. Implement minimum viable filter (~50 lines, pass-through)
3. Add shared memory reader (receive mask from daemon)
4. Add HLSL effect that composites mask texture over source
5. Add properties UI (mask source path, IPC config)
```

**Estimated LOC for working plugin:** 200-400 lines of C/C++ + 1 HLSL effect file.

### C vs C++ decision:

- **C** if we want maximum simplicity and minimal dependencies (our plugin is small)
- **C++** if we want to use RAII for OBS object lifetimes and potentially integrate OpenCV later

For a ~300-line plugin, C is fine. If scope grows, C++ is better.

---

## Key References

| Resource | URL |
|----------|-----|
| obs-plugintemplate | https://github.com/obsproject/obs-plugintemplate |
| OBS Plugin Docs (32.1.0) | https://docs.obsproject.com/plugins |
| Source API Reference | https://docs.obsproject.com/reference-sources |
| OBS Scripting Docs | https://docs.obsproject.com/scripting |
| Lua Halftone Filter Tutorial | https://github.com/obsproject/obs-studio/wiki/scripting-tutorial-halftone-filter-listing |
| rust-obs-plugins | https://github.com/bennetthardwick/rust-obs-plugins |
| obs-composite-blur | https://github.com/FiniteSingularity/obs-composite-blur |
| obs-backgroundremoval | https://github.com/royshil/obs-backgroundremoval |
| obs-backgroundremoval walkthrough | https://www.morethantechnical.com/blog/2023/05/20/building-an-obs-background-removal-plugin-a-walkthrough/ |
| DeepWiki: OBS Filters & Effects | https://deepwiki.com/obsproject/obs-studio/4.7-filters-and-effects |
| macOS Plugin Installation | https://obsproject.com/forum/threads/plugins-folder-on-mac.140442/ |
| OBS Rendering Graphics API | https://docs.obsproject.com/graphics |
