// AMD FSR upscaling (vk_ffx.c) for the DLAA path (vk_dlaa.c): with totk.ini dlaa = 1 and dlaa_backend = fsr, the
// scene that DLSS would get goes to AMD's FSR (FSR 4 where the GPU has it, FSR 3.1 elsewhere) through
// amd_fidelityfx_loader_vk.dll next to the executable. Windows only; elsewhere every call does nothing.
#pragma once
#include "vk.h"

int vk_ffx_wanted(void);   // dlaa and dlaa_backend = fsr at start (the device may still lack FSR)
// vk_device.c, while it builds the device: the extensions FSR's shaders use (cooperative matrix, 8-bit floats) join
// the list when the device has them; their feature structures are chained there.
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max);
void vk_ffx_init(void);      // after the device exists: loads the loader and picks the upscaler version
int vk_ffx_available(void);  // vk_ffx_init found an upscaler for this device

typedef struct {   // one image FSR reads or writes, in VK_IMAGE_LAYOUT_GENERAL, level 0 and layer 0
    VkImage image;
    VkFormat format;
    uint32_t width, height;
} VkFfxImage;

typedef struct {
    VkFfxImage color, depth, mv, reactive, transparency, out;   // reactive, transparency .image VK_NULL_HANDLE: none
    float jx, jy;               // this frame's jitter, render pixels (window x right, y down), as the scene was drawn
    float dt_ms;                // time since the previous dispatch
    float cam_near, cam_far;    // view-space distances of the near and the far end of the depth range (vk_ffx.c swaps them for
                                // FSR where the depth is inverted); cam_far is ignored for an infinite far plane
    float fov_y;                // vertical field of view, radians
    float sharpness;            // FSR's own sharpening pass (RCAS) on its output, 0..1; 0: none
    int reset;                  // a new history
} VkFfxDispatch;

// A context for one output size that renders at any scene size up to max_w x max_h: it is created with the upscaler's
// dynamic resolution flag, and every dispatch names the render size of its own images, so one context (and one FSR
// history, and one working set of memory) serves all the scene sizes of a run. The upscaler sizes its internal
// render-size images by the maximum, so it should be the largest scene expected, not the output. vk_dlaa.c keeps a
// single context and replaces it only where vk_ffx_fits says it no longer serves. depth_inverted: depth 1 is near;
// depth_infinite: the far plane is at infinity. Both are creation flags of the upscaler and stay fixed for the life of
// the context: when the game's depth convention changes, the context has to be released and a new one created
// (vk_ffx_depth_matches tells).
// NULL on failure (logged).
void *vk_ffx_create(uint32_t max_w, uint32_t max_h, uint32_t ow, uint32_t oh, int depth_inverted, int depth_infinite);
// 1 when ctx can upscale a w x h scene to ow x oh: the output size is the one it was made for and the scene is within
// its largest render size. 0 for a NULL ctx.
int vk_ffx_fits(const void *ctx, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh);
// 1 when ctx was created with this depth convention (also for a NULL ctx, which has none to disagree).
int vk_ffx_depth_matches(const void *ctx, int depth_inverted, int depth_infinite);
// 1 when ctx uses a reactive mask: the upscaler lists it among the inputs it requires or takes (queried when the
// context is created). A mask the context does not take is work for nothing, so vk_dlaa.c does not make one then;
// vk_ffx_dispatch leaves it out either way. 1 for a NULL ctx (none yet to ask) and where the upscaler cannot say.
int vk_ffx_uses_reactive(const void *ctx);
// Records the upscaling into cb. 0 on failure (logged). FSR binds pipelines and descriptors of its own. The reactive and
// the transparency and composition image go along only where the context uses them (FSR 3.1 takes both as optional
// inputs; the transparency one is for areas of special rendering such as reflections or animated textures, where it
// takes the history's protection off).
int vk_ffx_dispatch(void *ctx, VkCommandBuffer cb, const VkFfxDispatch *d);
void vk_ffx_release(void *ctx);
const char *vk_ffx_version(void);   // the upscaler version in use, for the log ("" before vk_ffx_init)
