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
| `runtime/vk_dlaa.c` | The DLAA/upscaling stage shared by DLSS and FSR: finds the scene's images, jitters the scene, runs `evaluate`, picks the Mipmap bias (`dlss_mode`). |
| `runtime/vk_ffx.c`, `vk_ffx.h` | The FSR backend (Windows only): loads the AMD DLLs, creates the context, dispatches. |
| `runtime/ffx_api_min.h` | Hand-copied descriptor layouts and constants of the FSR SDK 2.4.0 ffx-api (no SDK code is compiled in). |
| `runtime/resolution.c` | Output and nominal scene sizes (`res_sizes`), and the Mipmap bias formulas (`res_dlss_lod_bias`, `res_fsr_lod_bias`). |
| `runtime/vk_device.c` | Enables the device extensions and features FSR needs; logs them (`fsr_feature_log`). |
| `runtime/vk_texture.c` | Adds the Mipmap bias to the material samplers (`vk_sampler_dlss_bias`, `make_sampler`). |

## Selecting FSR

`dlaa = 1` turns the stage on; `dlaa_backend = fsr` selects FSR instead of DLSS/NGX (`vk_ffx_wanted`). On a non-Windows
build FSR is absent. `vk_ffx_device_exts` adds the extensions at device creation, `vk_ffx_init` runs once the device
exists.

## The AMD DLLs and the version

`amd_fidelityfx_loader_vk.dll` exports the five ffx-api entry points (`ffxCreateContext`, `ffxDestroyContext`,
`ffxConfigure`, `ffxQuery`, `ffxDispatch`) and finds `amd_fidelityfx_upscaler_vk.dll` next to itself; both are loaded
from the executable's folder. The upscaler DLL lists several versions (FSR 4.x and FSR 3.1.x, and the log also shows
2.3.4). `vk_ffx_init` pins the newest 4.x if the device runs it, otherwise leaves the DLL's default, which is FSR 3.1.6
on GPUs without FSR 4. Every context is created with that version (`ffxOverrideVersion`). The DLL is signed and its source
is not in this project: what happens inside is known only from the SDK's public headers, shaders and guide.

## What FSR is given (`evaluate` in `vk_dlaa.c`)

- **Colour**: the R11G11B10F light target, linear HDR, with automatic exposure.
- **Depth**: the D32F scene depth. Inverted or infinite depth is stated at context creation.
- **Motion vectors**: at the render resolution, in render pixels, from this frame's position to last frame's, without the
  jitter (`motionVectorScale` 1).
- **Jitter**: the jitter the scene was drawn with. The scene's draws are moved by it in `vk_dlaa_viewport` (the viewport is
  shifted by the frame's jitter when it is recorded); `next_jitter(w, ow)` gives the phase from the render and presentation
  widths. `fsr_jitter_sign = -1` flips the sign for testing.
- **Context flags**: `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | AUTO_EXPOSURE | DYNAMIC_RESOLUTION`, plus `DEPTH_INVERTED` and
  `DEPTH_INFINITE` as the scene needs, and `DEBUG_CHECKING` with `fsr_debug = 1`. No debug visualization or non-linear
  colour flag is set yet.
- **One context per run**: created with the dynamic-resolution flag and the largest expected render resolution; each
  dispatch states its own `renderSize`.
- Optional inputs are asked of the upscaler once per context (`vk_ffx_uses_reactive`, `vk_ffx_uses_transparency`). For
  FSR 3.1, `ffxConfigure` hands it the constants the `fsr_*` settings name.

## The inputs, one by one: used or not

Every input of the FSR dispatch is either supplied by this code or it is not. There is no third state: an input that has
no real source is left out and the log says so.

| Input | State | What it is |
| --- | --- | --- |
| Colour, depth, jitter, reset, render and presentation size, camera near/far/fov, frame time | used | As above. |
| Motion vectors | used | The camera's reprojection from depth (`dlaa_mv.comp`) with the per-object draws (`dlaa_objmv`, `vk_exec_draw.c`) written over it. |
| Sharpness | used | `sharpness` from the `sharpness` setting; `enableSharpening` when it is above 0. |
| Reactive mask | used | `e->mask`, written by `mv_pass`; contents below. |
| Transparency and composition mask | **not supplied** | No source for it is implemented. `fd.transparency` stays empty, `vk_ffx_dispatch` hands FSR nothing, and the log says `fsr: transparency and composition mask: not supplied`. The Debug view's input panel says the same. |
| Exposure | not supplied | `AUTO_EXPOSURE` is set on the context. |

## What goes into which mask

The FSR guide gives each mask its own job, and nothing is put into one that belongs in another.

- **Reactive mask**: where this frame's colour is to count for more than the history because the pixel's colour is not
  that of a surface that moved with the motion vector. Two things are written into it, the larger of the two per pixel:
  1. *Alpha of alpha-blended draws* (particles, smoke, sparks): what the guide asks for. The draws are recorded a second
     time with the game's own shaders into an R8 target and their alpha accumulates there (`alpha_draw`,
     `vk_alphaobj_execute`, `dlaa_mv.comp`); the value is at most `DLAA_ALPHA_CAP` (0.9). The sky takes none (scene depth at
     the far plane). Draws it does not take are counted by cause in the log line `vk_exec: reactive mask draws`: blends
     without an alpha factor (plain additive), line loops, byte indices, indirect draws, draws with
     storage resources, draws still waiting for their pipeline. Those are not in the mask. `fsr_alpha = 0` turns this off.
  2. *The characters' class mask*: characters, creatures and what they hold, from the G-buffer's class channel, and where
     they were the frame before. A character's vector is the camera's only where per-object motion does not reach it, and
     the guide names drawing such an object into the reactive mask as the way to limit its ghosting. Value `dlss_char_mask`
     per cent, at most 0.9 for FSR.
- **Transparency and composition mask**: for areas of special shading that has no motion, such as reflections or animated
  textures. Nothing in the game's draws has been identified as such a source, so nothing is supplied (see above).
- **Motion vectors**: motion that comes from a vertex shader (wind in grass, cloth) belongs here, not in a mask. Draws that
  read no object blocks, terrain and grass among them, are not listed for per-object motion (`mvobj_all = 0`), so their
  vectors are the camera's. The mask code does not cover for that.

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
