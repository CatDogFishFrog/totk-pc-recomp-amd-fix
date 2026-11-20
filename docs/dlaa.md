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
| Reactive mask | used | An R8 image at scene resolution, `e->mask`. It is passed to FSR when the context accepts the resource and `mv_pass` wrote it from at least one source that is on. Contents are below. |
| Transparency and composition mask | used, if a source is on | An R8 image at scene resolution, `e->tc`, its own image. It is passed when the context accepts the resource and `mv_pass` wrote it from at least one source that is on; every source is off by default, so by default it is not supplied. Contents are below. |
| Exposure | not supplied | `AUTO_EXPOSURE` is set on the context. |

## What goes into each mask

Every way a pixel can enter a mask is a *source*: a row of the table in `runtime/fsr_mask.c`, with a setting of its own
(`fsr_mask_*`, the "FSR mask sources" rows of the Video tab, listed in [settings.md](settings.md)). The enabled sources of
a mask combine per pixel by the larger value. `fsrmask_plan` turns the settings into one frame's plan, `mv_pass` in
`vk_dlaa.c` hands the plan to `dlaa_mv.comp` as push constants, and a mask nobody feeds is not written and not given to
FSR. DLSS keeps its one bias mask: the three character classes at the Character trails strength, no other source.

A new source is a table row and a catalog text (`UI_MENU_FSRM_*`); the menu builds its rows from the table. Which pixels
a source marks is a hypothesis to check in the Debug view of the Video tab (the Reactive and T&C inputs views), not an
established property of the game's materials.

### Method 1: G-buffer class

Target 0 of the G-buffer carries an object class in green. Measured on single frames: Link 253/255, other characters and
creatures 125, held items 160, foliage 32, terrain 0. `dlaa_mv.comp` compares the value with each class's band
(`BANDS` in `fsr_mask.c`):

| Source (setting) | Mask | Band | Default |
| --- | --- | --- | --- |
| Link (`fsr_mask_r_link`) | Reactive | 207..255 | on |
| Characters and creatures (`fsr_mask_r_creatures`) | Reactive | 65..142 | on |
| Held items (`fsr_mask_r_held`) | Reactive | 143..206 | on |
| Foliage (`fsr_mask_r_foliage`, strength `fsr_mask_r_foliage_level`) | Reactive | 28..36 | off |
| Foliage (`fsr_mask_tc_foliage`, strength `fsr_mask_tc_foliage_level`) | T&C | 28..36 | off |

The three character bands are contiguous and together equal the single threshold (above 64/255) the characters' mask
had before, so with all three on nothing changed. Their value is the Character trails strength (`dlss_char_mask`, default
65%, at most 90% for FSR; 0 turns the three off). The pass also marks a 3x3 area around the pixel's previous-frame
position when that position belonged to an enabled character class: this covers the footprint of a character whose motion
vector is only the camera's. Foliage is a narrow comparison round its measured value, not a threshold, so it cannot grow
into the character bands; it has no footprint. Its strengths are percent, default 25, a starting value nobody measured.
Reactive and T&C foliage are separate sources on purpose: the first tests unstable reconstruction of moving leaves, the
second a colour that changes although the motion vector is right.
Not established: that the band values hold in every scene (they are single-frame measurements), that foliage moves with
the wind in the G-buffer class, and that either mask helps foliage at all.

### Method 2: alpha replay

`alpha_draw` and `vk_alphaobj_execute` replay qualifying draw calls with the game's shaders into an R8 target cleared to
1, depth-tested and with no depth writes. The value is `1 - retained_alpha`, capped at 0.9 for Reactive and 1.0 for T&C,
zero outside covered draw pixels and on the sky (scene depth at the far plane).

A draw qualifies only when it belongs to the armed light/depth targets, target 0 has blend enabled, and the RGB blend
uses either `ONE_MINUS_SRC_ALPHA` for destination alpha coverage or `SRC_ALPHA`/`ONE` for alpha-scaled ADD coverage.
It must also have a usable, non-multisample pipeline, enabled depth test, disabled depth writes, and no feedback or
storage resources. Plain additive blends, indirect and byte-index draws, line loops, and unbuilt pipelines are omitted.
The log line `vk_exec: reactive mask draws` reports omitted draws by cause.

The two blend kinds are separate sources, and each can feed Reactive, T&C or both:

| Blend kind | Reactive | T&C |
| --- | --- | --- |
| Alpha-blended (`ONE_MINUS_SRC_ALPHA`) | `fsr_mask_r_alpha_blend`, on | `fsr_mask_tc_alpha_blend`, off |
| Alpha-scaled additive (`SRC_ALPHA`, `ONE`) | `fsr_mask_r_alpha_add`, on | `fsr_mask_tc_alpha_add`, off |

A draw is recorded once and drawn into the image of each mask it feeds; a blend kind no enabled source takes is not
recorded at all, which saves the second pass. The replay does not know what a draw is: smoke, glass and water that pass
the criteria above are taken alike, and the replay can widen the coverage where the original draw relied on stencil. The
T&C alpha sources write the same coverage as the Reactive ones into the T&C image; they are experimental and off. The
old `fsr_alpha = 0` still turns every alpha source off.

No source for a mask means `e->mask` (or `e->tc`) is not written and FSR receives no resource for that input. The log line
`fsr: masks given` says, whenever it changes, which masks were given and which the context takes.

### Not implemented

Sources that need a draw allowlist, additive-specific replay, replay of more draw types, temporal-residual detection or
reflection data flow are not implemented. No source detects animated textures or reflections.

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
