# DLAA and the FSR upscaler: how they are built here

A reference for anyone (human or agent) who works on the upscaler, so that nobody has to rediscover how it fits together.
It records what the code does and what has been established; where something is not established it says so. Settings are
in [settings.md](settings.md), the rest of the renderer in [architecture.md](architecture.md).

Terms (the FSR guide's): *render resolution* is the size of the image the upscaler reads; *presentation resolution* is
the size of the image it writes; *Mipmap bias* is the texture LOD bias that offsets the loss of texture detail at a lower
render resolution.

## Files

| File | Role |
| --- | --- |
| `runtime/vk_dlaa.c` | The DLAA/upscaling stage shared by DLSS and FSR: finds the scene's images, applies scene jitter, runs `evaluate`, and selects the Mipmap bias (`dlss_mode`). |
| `runtime/vk_ffx.c`, `vk_ffx.h` | The FSR backend (Windows only): loads the AMD DLLs, creates the context, dispatches. |
| `runtime/ffx_api_min.h` | Hand-copied descriptor layouts and constants of the FSR SDK 2.4.0 ffx-api (no SDK code is compiled in). |
| `runtime/resolution.c` | Output and nominal scene sizes (`res_sizes`), and the Mipmap bias formulas (`res_dlss_lod_bias`, `res_fsr_lod_bias`). |
| `runtime/vk_device.c` | Enables the device extensions and features FSR needs; logs them (`fsr_feature_log`). |
| `runtime/vk_texture.c` | Adds the Mipmap bias to the material samplers (`vk_sampler_dlss_bias`, `make_sampler`). |

## Selecting FSR

`dlaa = 1` turns the stage on. `dlaa_backend = fsr` selects the newest FSR 4 version offered for the GPU, falling back
to the DLL's default (FSR 3.1); `dlaa_backend = fsr3` selects the newest FSR 3 version offered. Both use `vk_ffx_wanted`;
FSR is available only on Windows. `vk_ffx_device_exts` adds available FSR extensions at device creation, and
`vk_ffx_init` runs once the device exists.

## The AMD DLLs and the version

`amd_fidelityfx_loader_vk.dll` exports the five ffx-api entry points (`ffxCreateContext`, `ffxDestroyContext`,
`ffxConfigure`, `ffxQuery`, `ffxDispatch`) and finds `amd_fidelityfx_upscaler_vk.dll` next to itself; both are loaded
from the executable's folder. The upscaler DLL lists several versions. `vk_ffx_init` pins the newest FSR 4 version for
`fsr`, or newest FSR 3 version for `fsr3`, when offered for the GPU; otherwise it leaves the DLL's default. Every context
is created with the selected version (`ffxOverrideVersion`). The DLL is signed and its source
is not in this project: what happens inside is known only from the SDK's public headers, shaders and guide.

## What FSR is given (`evaluate` in `vk_dlaa.c`)

- **Colour**: the R11G11B10F light target, linear HDR, with automatic exposure.
- **Depth**: the D32F scene depth. Inverted or infinite depth is stated at context creation.
- **Motion vectors**: at the render resolution, in render pixels, from this frame's position to last frame's, without the
  jitter (`motionVectorScale` 1).
- **Jitter**: the jitter applied to the scene and passed to FSR. Normally `vk_dlaa_viewport` shifts scene draw viewports;
  on game version 1.4.3, `dlaa_cam_jitter` can instead apply it in the camera block. `next_jitter(w, ow)` selects the
  phase from render and presentation widths. `fsr_jitter_sign = -1` flips the sign sent to FSR for testing.
- **Context flags**: `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | AUTO_EXPOSURE | DYNAMIC_RESOLUTION`, plus `DEPTH_INVERTED` and
  `DEPTH_INFINITE` as the scene needs, and `DEBUG_CHECKING` with `fsr_debug = 1`. No debug visualization or non-linear
  colour flag is set yet.
- **Context lifetime**: created with the dynamic-resolution flag and a maximum render size; each dispatch states its own
  `renderSize`. It is reused while it fits and recreated when output size, depth convention, or context requirements
  change. Recreating it resets FSR history.
- Optional inputs are asked of the upscaler once per context (`vk_ffx_uses_reactive`, `vk_ffx_uses_transparency`). For
  FSR 3.1, `ffxConfigure` hands it the constants the `fsr_*` settings name.

## The inputs, one by one: used or not

Every input of the FSR dispatch is either supplied by this code or it is not. There is no third state: an input that has
no real source is left out and the log says so.

| Input | State | What it is |
| --- | --- | --- |
| Colour, depth, jitter, reset, render and presentation size, camera near/far/fov, frame time | used | As above. |
| Motion vectors | used | The camera's reprojection from depth (`dlaa_mv.comp`) with the per-object draws (`dlaa_objmv`, `vk_exec_draw.c`) written over it. |
| Sharpness | used | `fsr_sharpness`, passed as `sharpness`; DLSS/game `sharpness` is separate. |
| Reactive mask | used | An R8 image at scene resolution, `e->mask`. It is passed to FSR when the context accepts the resource and `mv_pass` wrote it. Contents are below. |
| Transparency and composition mask | **not supplied** | No source or image is implemented; `fd.transparency` remains empty. |
| Exposure | not supplied | `AUTO_EXPOSURE` is set on the context. |

## What goes into each mask

### Reactive mask

`dlaa_mv.comp` builds `e->mask` as an R8 image at scene resolution. At each pixel it takes the larger value from:

1. **Qualifying alpha-blended draws.** `alpha_draw` and `vk_alphaobj_execute` replay qualifying draw calls with the game's
   shaders into an R8 target cleared to 1, depth-tested and with no depth writes. The value is
   `1 - retained_alpha`, capped at 0.9. It is zero outside covered draw pixels and on the sky (scene depth at the far
   plane).

   A draw qualifies only when it belongs to the armed light/depth targets, target 0 has blend enabled, and the RGB blend
   uses either `ONE_MINUS_SRC_ALPHA` for destination alpha coverage or `SRC_ALPHA`/`ONE` for alpha-scaled ADD coverage.
   It must also have a usable, non-multisample pipeline, enabled depth test, disabled depth writes, and no feedback or
   storage resources. Plain additive blends, indirect and byte-index draws, line loops, and unbuilt pipelines are omitted.
   `TOTK_FSR_ALPHA=0` disables this source. The log line `vk_exec: reactive mask draws` reports omitted draws by cause.

2. **Characters and their previous-frame footprint.** Target 0 of the G-buffer carries object classes in green. A pixel
   whose green value is greater than `64/255` is a character, creature, or held item. It gets
   `TOTK_DLSS_CHAR_MASK` percent, default 65% and capped at 90% for FSR.

   The pass also marks a 3x3 area around the pixel's previous-frame position when that position belonged to a character.
   This covers the footprint of a character whose motion vector is only the camera's. The two character sources in one
   frame are already combined into one binary footprint; this footprint and the alpha source are then combined as a
   per-pixel maximum. `TOTK_DLSS_CHAR_MASK=0` disables the character source.

No character or alpha source means `e->mask` is not written and FSR receives no reactive resource for that dispatch.

### Transparency and composition mask

**Not supplied.** The code does not create or identify a T&C image, and no source detects reflections, animated textures,
or other specialist shading. `fd.transparency` stays empty. If FSR's context lists T&C as usable, FSR receives no image
for this input.

## Rule: no stand-ins

If an input, a mask or a feature is not implemented, it is absent, and the log says so in plain words (`not supplied`,
`not implemented`, an error where it is one). Nothing is filled in with something that merely looks like it: a copy of
another mask, a constant, a guess from the picture. This documentation uses the same two words for every input: used, or
not. Work that is wanted but not done is listed as not done, with what it waits for.

## Render resolution

- `resolution.c` asks for a *nominal* scene size (`scene_for`, rounded to the nearest pixel: 1129x635 for Balanced at
  1920x1080). The patch (`site scene` in `recomp/patches-v143.txt`) passes it to the game's renderer as two floats.
- The size FSR actually gets is the width and height of the game's light target (`w`, `h` in `evaluate`). In the logs of FSR
  runs it is the nominal size rounded down to a multiple of 8 (1129x635 gives 1128x632, 960x540 gives 960x536; sizes that
  are already multiples of 8 do not change).
- That rounding is not in the host: `scene_for`, `nvn_api_obj.c` (the texture builder passes sizes through) and
  `vk_texture.c` do not align sizes. It happens in the game's own target setup, which this repository does not contain in
  readable form, so it is observed, not located.
- `evaluate` logs `dlaa: render resolution ...` whenever the sizes change, with the nominal scene and the Mipmap bias worked out
  from each. `vk_dlaa_viewport` logs the scene's viewport and scissor once per distinct combination (FSR only) and says
  whether both cover the whole target; the two had not been compared before that line existed.

## Mipmap bias

`dlss_mode` adds a bias to the material samplers (filtered, mipmapped, repeating ones; `vk_sampler_dlss_bias`). The sum
with the game's own bias is clamped to `maxSamplerLodBias` in `make_sampler`. FSR: `log2(k) - 1`; DLSS: `log2(k)`; k is
render width over presentation width from the nominal scene (`res_fsr_lod_bias`), not from the target actually drawn. The
log line `dlaa: mipmap bias ...` gives the value in use.

## Device requirements

`vk_ffx_device_exts` adds, where the device has them: `VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8`, and
`VK_KHR_compute_shader_derivatives`, falling back to `VK_NV_compute_shader_derivatives` when the KHR one is missing
(either satisfies `SPV_KHR_compute_shader_derivatives`). The rest FSR uses is core Vulkan 1.3. `vk_device.c` chains the
feature structs and, for FSR runs, logs `vk: FSR device features: ...` (which derivatives extension, its two features,
`shaderStorageImageReadWithoutFormat`). A missing feature is only reported, never required. Which DLL version creates the
shader modules that declare the derivatives capability is not established.

## Constants checked against the SDK

`ffx_api_min.h` was compared with the headers of tag v2.4.0 (a program printing constants, `sizeof` and `offsetof`):
43 shared values are identical, `ffxDispatchDescUpscale` is 432 bytes with `flags` at offset 428. Values it does not have
yet: `FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE` 256 (1<<8), `FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION` 512 (1<<9),
`FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW` 1 (1<<0), `FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB` 2, `FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ` 4.
