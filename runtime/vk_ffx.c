// AMD FSR upscaling for the DLAA path (vk_ffx.h). The FSR SDK 2.4 ships its Vulkan effects as signed DLLs behind a
// loader: amd_fidelityfx_loader_vk.dll exports the five ffx-api entry points and finds the effect DLL
// (amd_fidelityfx_upscaler_vk.dll) next to itself. Both are loaded from the executable's folder at run time; no SDK
// code is compiled in, only the descriptor layouts of ffx_api_min.h.
//
// The upscaler DLL offers several versions (FSR 4.x, FSR 3.1.x) and decides by itself which ones a device can run:
// vk_ffx_init lists them and pins the newest 4.x when there is one (dlaa_backend = fsr), or the newest 3.x
// (dlaa_backend = fsr3), else leaves the DLL's default (3.1 on GPUs without FSR 4). Every context is created with that
// version (ffxOverrideVersion).
//
// What FSR gets is what DLSS gets (vk_dlaa.c evaluate): the R11G11B10F light target as linear HDR colour with automatic
// exposure, the D32F scene depth, motion vectors at the render size in render pixels pointing from this frame's
// position to last frame's without the jitter (motionVectorScale 1), and the jitter the scene was drawn with. That is
// FSR's own convention as well: its guide derives the projection offset as (2 jx / w, -2 jy / h), which moves the
// picture by (jx, jy) window pixels with y down, the way vk_dlaa_viewport moves the viewport. TOTK_FSR_JITTER_SIGN=-1
// flips it to test the other convention.
//
// One context serves every scene size of a run (vk_ffx_create): it is created with the dynamic resolution flag
// (FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION) and the largest render size expected, and each dispatch states the size of
// its own images (renderSize). The input
// images are exactly render-sized, so the region of them the upscaler reads is the whole image; FSR 3.1 reads its
// inputs by integer load or by the input's own size (ffx_fsr3upscaler_callbacks_hlsl.h, prepare_inputs, upsample) and
// keeps its internal images at maxRenderSize.
//
// The upscaler is also asked, once per context, which optional inputs it uses (ffxQueryDescUpscaleGetResourceRequirements:
// vk_ffx_uses_reactive), so that a mask it does not read is not made, and, for FSR 3.1, ffxConfigure hands it the
// constants that the TOTK_FSR_* settings name (constants_sync).
#include "vk_ffx.h"

#include "cfg.h"
#include "host.h"

#ifndef _WIN32
int vk_ffx_wanted(void) { return 0; }
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max) {
    (void)have, (void)nhave, (void)ext, (void)n, (void)max;
}
void vk_ffx_init(void) {}
int vk_ffx_available(void) { return 0; }
void *vk_ffx_create(uint32_t max_w, uint32_t max_h, uint32_t ow, uint32_t oh, int depth_inverted, int depth_infinite,
                    int debug_view) {
    (void)max_w, (void)max_h, (void)ow, (void)oh, (void)depth_inverted, (void)depth_infinite, (void)debug_view;
    return NULL;
}
int vk_ffx_debug_matches(const void *ctx, int debug_view) { return (void)ctx, (void)debug_view, 1; }
int vk_ffx_uses_transparency(const void *ctx) { return (void)ctx, 0; }
int vk_ffx_fits(const void *ctx, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    return (void)ctx, (void)w, (void)h, (void)ow, (void)oh, 0;
}
int vk_ffx_depth_matches(const void *ctx, int depth_inverted, int depth_infinite) {
    return (void)ctx, (void)depth_inverted, (void)depth_infinite, 1;
}
int vk_ffx_uses_reactive(const void *ctx) { return (void)ctx, 1; }
int vk_ffx_dispatch(void *ctx, VkCommandBuffer cb, const VkFfxDispatch *d) { return (void)ctx, (void)cb, (void)d, 0; }
void vk_ffx_release(void *ctx) { (void)ctx; }
const char *vk_ffx_version(void) { return ""; }
#else
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffx_api_min.h"

#define FFX_LOADER L"amd_fidelityfx_loader_vk.dll"
#define FFX_MAX_VERSIONS 16

static struct {
    HMODULE dll;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;   // optional: without it the TOTK_FSR_* constants are left alone
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    int tried, loaded, available;
    VkDeviceContext dev;
    uint64_t version_id;   // 0: the DLL's default
    char version[48];
    float jitter_sign;
    int camera_plain;   // TOTK_FSR_CAMERA_PLAIN=1: cameraNear/Far as the projection gives them (camera_range)
} F = {.jitter_sign = 1.0f};

int vk_ffx_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = cfg_bool("TOTK_DLAA") && !strncmp(cfg_value("TOTK_DLAA_BACKEND"), "fsr", 3);
    return wanted;
}

// The constants of the FSR 3.1 upscaler that ffxConfigure overrides (ffx_upscale.h, FfxApiConfigureUpscaleKey), each by a
// setting of the same name that is unset by default: unset leaves AMD's value in force (the upscaler clamps a value to
// the range it accepts, and a NULL value restores its default). settings.md describes them.
static const struct {
    const char *setting;
    uint64_t key;
} CONSTANTS[] = {
    {"TOTK_FSR_VELOCITY_FACTOR", FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR},
    {"TOTK_FSR_REACTIVENESS_SCALE", FFX_API_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE},
    {"TOTK_FSR_SHADING_CHANGE_SCALE", FFX_API_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE},
    {"TOTK_FSR_ACCUMULATION", FFX_API_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME},
    {"TOTK_FSR_MIN_DISOCCLUSION", FFX_API_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION},
};
#define NCONSTANTS (sizeof CONSTANTS / sizeof *CONSTANTS)

// What vk_ffx_create hands out: the ffx context, the sizes it was created for and the depth convention. All of them are
// part of the context's creation (maxRenderSize, maxUpscaleSize, the depth flags) and cannot change afterwards, so they
// are kept here for vk_ffx_fits and vk_ffx_depth_matches. The rest is what the context has been told since.
struct FfxCtx {
    ffxContext ctx;
    uint32_t max_w, max_h, ow, oh;
    int depth_inverted, depth_infinite;
    int debug_view;                         // TODO(debug-view): made with FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION
    int debug_logged;                       // TODO(debug-view): the first Debug view dispatch was logged
    int uses_reactive, uses_transparency;   // optional inputs the upscaler takes (resource requirements)
    int needs_reactive;                     // it lists the reactive mask as required, not optional
    int warned_reactive;                    // the missing required mask was logged
    int constants_synced;                   // constants_sync has run, for the settings version below
    unsigned constants_seen;
    unsigned constants_warned;              // bit i: constant i's setting or its configure call was complained about
    float applied[NCONSTANTS];              // what ffxConfigure last set for constant i; NaN: the upscaler's own value
};

static int loader_load(void) {
    if (F.tried) return F.loaded;
    F.tried = 1;
    wchar_t path[MAX_PATH + 64];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (len && path[len - 1] != L'\\') len--;
    path[len] = 0;
    wcscat(path, FFX_LOADER);
    // LOAD_WITH_ALTERED_SEARCH_PATH: the loader's own imports and the effect DLL resolve from the executable's folder
    F.dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!F.dll) {
        rt_log("fsr: cannot load amd_fidelityfx_loader_vk.dll next to totk.exe (error %lu): FSR off", GetLastError());
        return 0;
    }
    F.create = (PfnFfxCreateContext)(void *)GetProcAddress(F.dll, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(F.dll, "ffxDestroyContext");
    F.configure = (PfnFfxConfigure)(void *)GetProcAddress(F.dll, "ffxConfigure");   // may be missing: not fatal
    F.query = (PfnFfxQuery)(void *)GetProcAddress(F.dll, "ffxQuery");
    F.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(F.dll, "ffxDispatch");
    if (!F.create || !F.destroy || !F.query || !F.dispatch) {
        rt_log("fsr: amd_fidelityfx_loader_vk.dll lacks the ffx-api entry points: FSR off");
        return 0;
    }
    return F.loaded = 1;
}

static int has(const VkExtensionProperties *have, uint32_t nhave, const char *name) {
    for (uint32_t i = 0; i < nhave; i++)
        if (!strcmp(have[i].extensionName, name)) return 1;
    return 0;
}

static void add(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max,
                const char *name) {
    for (uint32_t i = 0; i < *n; i++)
        if (!strcmp(ext[i], name)) return;
    if (has(have, nhave, name) && *n < max) ext[(*n)++] = name;
}

// The extensions the FSR SDK's own sample enables for its effects (Cauldron2 device_vk.cpp): cooperative matrix and
// 8-bit floats for FSR 4's network where the device has them, and compute shader derivatives. The shader modules that are
// created on this device while an FSR context is made declare SPV_KHR_compute_shader_derivatives, and the validation
// layer reports them as not enabled without it. Which of the upscaler DLL's versions makes those modules is not
// established (the log names no module, and the DLL lists FSR 3.1.6 and 2.3.4 side by side); the device has to offer the
// extension either way. Vulkan accepts that SPIR-V extension with VK_KHR_compute_shader_derivatives or with its older
// VK_NV_compute_shader_derivatives, so the NV one is the fallback where the KHR one is missing. Whether a driver offers
// either is read from the device: vk_device.c logs which one is enabled. The rest FSR uses (16-bit floats and storage,
// integer dot products, subgroup size control) is core Vulkan 1.3, which vk_device.c enables as far as the driver reports it.
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max) {
    if (!vk_ffx_wanted()) return;
    add(have, nhave, ext, n, max, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
#ifdef VK_EXT_SHADER_FLOAT8_EXTENSION_NAME
    add(have, nhave, ext, n, max, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
#endif
    int khr = 0;
#ifdef VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME
    add(have, nhave, ext, n, max, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    for (uint32_t i = 0; i < *n; i++)
        if (!strcmp(ext[i], VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) khr = 1;
#endif
#ifdef VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME
    if (!khr) add(have, nhave, ext, n, max, VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
#endif
    (void)khr;
}

void vk_ffx_init(void) {
    if (!vk_ffx_wanted() || !loader_load()) return;
    const VkCtx *c = vk_ctx();
    F.dev = (VkDeviceContext){c->device, c->phys, vkGetDeviceProcAddr};
    const char *js = cfg_get("TOTK_FSR_JITTER_SIGN");
    if (js && !strcmp(js, "-1")) F.jitter_sign = -1.0f;
    F.camera_plain = rt_knob("FSR_CAMERA_PLAIN", "0") != 0;
    uint64_t count = FFX_MAX_VERSIONS, ids[FFX_MAX_VERSIONS] = {0};
    const char *names[FFX_MAX_VERSIONS] = {0};
    struct ffxQueryDescGetVersions q = {.header = {FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, NULL},
                                        .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .device = &F.dev,
                                        .outputCount = &count, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t r = F.query(NULL, &q.header);
    if (r != FFX_API_RETURN_OK || !count) {
        rt_log("fsr: no upscaler for this device (query %u, %llu versions; is amd_fidelityfx_upscaler_vk.dll next to "
               "totk.exe?): FSR off", (unsigned)r, (unsigned long long)count);
        return;
    }
    if (count > FFX_MAX_VERSIONS) count = FFX_MAX_VERSIONS;
    char list[256] = "";
    // dlaa_backend = fsr takes the newest FSR 4 the DLL offers for this device, fsr3 the newest FSR 3 (3.1 in the SDK
    // 2.4 libraries, on any GPU the DLL supports). Without one the DLL's default stays (FSR 3.1 on GPUs without FSR 4).
    int want3 = !strcmp(cfg_value("TOTK_DLAA_BACKEND"), "fsr3");
    int best = -1, best_major = 0, best_minor = 0, best_patch = 0;
    for (uint64_t i = 0; i < count; i++) {
        const char *nm = names[i] ? names[i] : "?";
        size_t used = strlen(list);
        snprintf(list + used, sizeof list - used, "%s%s", i ? ", " : "", nm);
        int ma = 0, mi = 0, pa = 0;
        if (sscanf(nm, "%d.%d.%d", &ma, &mi, &pa) < 1 || (want3 ? ma != 3 : ma < 4)) continue;
        if (best < 0 || ma > best_major || (ma == best_major && (mi > best_minor || (mi == best_minor && pa > best_patch))))
            best = (int)i, best_major = ma, best_minor = mi, best_patch = pa;
    }
    if (best >= 0) {
        F.version_id = ids[best];
        snprintf(F.version, sizeof F.version, "FSR %s", names[best]);
    } else {
        snprintf(F.version, sizeof F.version, "FSR %s", names[0] ? names[0] : "?");   // the DLL's default
    }
    F.available = 1;
    rt_log("fsr: upscaler versions for this device: %s; using %s%s", list, F.version,
           best >= 0 ? "" : want3 ? " (FSR 3 is not offered for this GPU)" : " (FSR 4 is not offered for this GPU)");
}

int vk_ffx_available(void) { return vk_ffx_wanted() && F.available; }

const char *vk_ffx_version(void) { return F.version; }

// Messages from the upscaler (errors and warnings), in UTF-16; into the log as UTF-8.
static void on_message(uint32_t type, const wchar_t *msg) {
    char s[512];
    if (!msg || !WideCharToMultiByte(CP_UTF8, 0, msg, -1, s, (int)sizeof s, NULL, NULL)) return;
    rt_log("fsr: %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", s);
}

// Which of the optional inputs this context takes, from the upscaler's own list of what it requires and what it uses when
// given (FSR 3.1: reactive mask and transparency and composition mask optional; FSR 4 says for itself). Where the
// query fails (an upscaler that predates it) the reactive mask is taken as used, as before the query was made, and the
// transparency one as not.
static void query_requirements(struct FfxCtx *fc) {
    fc->uses_reactive = 1, fc->uses_transparency = 0, fc->needs_reactive = 0;
    struct ffxQueryDescUpscaleGetResourceRequirements q = {
        .header = {FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS, NULL}};
    ffxReturnCode_t r = F.query(&fc->ctx, &q.header);
    if (r != FFX_API_RETURN_OK) {
        rt_log("fsr: the upscaler does not list its resource requirements (%u): the reactive mask is made as before", (unsigned)r);
        return;
    }
    uint64_t all = q.required_resources | q.optional_resources;
    fc->uses_reactive = (all & FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK) != 0;
    fc->uses_transparency = (all & FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION) != 0;
    fc->needs_reactive = (q.required_resources & FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK) != 0;
    rt_log("fsr: inputs required %#llx, optional %#llx: reactive mask %s, transparency and composition mask %s",
           (unsigned long long)q.required_resources, (unsigned long long)q.optional_resources,
           fc->needs_reactive ? "required" : fc->uses_reactive ? "used" : "not used",
           fc->uses_transparency ? "used" : "not used");
}

void *vk_ffx_create(uint32_t max_w, uint32_t max_h, uint32_t ow, uint32_t oh, int depth_inverted, int depth_infinite,
                    int debug_view) {
    if (!F.available) return NULL;
    struct ffxOverrideVersion ov = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, NULL}, .versionId = F.version_id};
    struct ffxCreateContextDescUpscaleVersion ver = {
        .header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, F.version_id ? &ov.header : NULL},
        .version = FFX_UPSCALER_VERSION};
    struct ffxCreateBackendVKDesc be = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, &ver.header},
                                        .deviceContext = F.dev};
    // Linear HDR scene light with the upscaler's own exposure, as DLSS gets it ("IsHDR | AutoExposure"). The render
    // size is named at every dispatch (DYNAMIC_RESOLUTION): one context serves every scene size up to maxRenderSize.
    uint32_t flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                     FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    if (depth_inverted) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (depth_infinite) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (rt_knob("FSR_DEBUG", "0")) flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;   // TOTK_FSR_DEBUG=1: API checks
    if (debug_view) flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;   // TODO(debug-view)
    struct ffxCreateContextDescUpscale cd = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &be.header},
                                             .flags = flags, .maxRenderSize = {max_w, max_h},
                                             .maxUpscaleSize = {ow, oh}, .fpMessage = on_message};
    struct FfxCtx *fc = calloc(1, sizeof *fc);
    if (!fc) return NULL;
    ffxReturnCode_t r = F.create(&fc->ctx, &cd.header, NULL);
    if (r != FFX_API_RETURN_OK || !fc->ctx) {
        rt_log("fsr: context creation failed (%u) for up to %ux%u -> %ux%u", (unsigned)r, max_w, max_h, ow, oh);
        free(fc);
        return NULL;
    }
    fc->max_w = max_w, fc->max_h = max_h, fc->ow = ow, fc->oh = oh;
    fc->depth_inverted = depth_inverted != 0, fc->depth_infinite = depth_infinite != 0;
    fc->debug_view = debug_view != 0;
    for (size_t i = 0; i < NCONSTANTS; i++) fc->applied[i] = NAN;   // a new context starts with the upscaler's own values
    query_requirements(fc);
    struct ffxQueryGetProviderVersion pv = {.header = {FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, NULL}};
    F.query(&fc->ctx, &pv.header);
    rt_log("fsr: context created for scenes up to %ux%u -> %ux%u with %s (flags %#x)", max_w, max_h, ow, oh,
           pv.versionName ? pv.versionName : F.version, flags);
    return fc;
}

int vk_ffx_fits(const void *ctx, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    const struct FfxCtx *fc = ctx;
    return fc && fc->ctx && fc->ow == ow && fc->oh == oh && w <= fc->max_w && h <= fc->max_h;
}

int vk_ffx_depth_matches(const void *ctx, int depth_inverted, int depth_infinite) {
    const struct FfxCtx *fc = ctx;
    return !fc || (fc->depth_inverted == (depth_inverted != 0) && fc->depth_infinite == (depth_infinite != 0));
}

int vk_ffx_uses_reactive(const void *ctx) {
    const struct FfxCtx *fc = ctx;
    return !fc || fc->uses_reactive;
}

int vk_ffx_uses_transparency(const void *ctx) {   // TODO(debug-view)
    const struct FfxCtx *fc = ctx;
    return fc && fc->uses_transparency;
}

int vk_ffx_debug_matches(const void *ctx, int debug_view) {   // TODO(debug-view)
    const struct FfxCtx *fc = ctx;
    return !fc || fc->debug_view == (debug_view != 0);
}

void vk_ffx_release(void *ctx) {
    struct FfxCtx *fc = ctx;
    if (!fc) return;
    if (fc->ctx) F.destroy(&fc->ctx, NULL);
    free(fc);
}

static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// Every image of the DLAA path stays in VK_IMAGE_LAYOUT_GENERAL: GENERIC_READ and UNORDERED_ACCESS are the states the
// Vulkan backend maps to that layout (ffx_vk.cpp getVKImageLayoutFromResourceState), and it puts each resource back
// into the state it was given at the end of the dispatch.
static struct FfxApiResource resource(const VkFfxImage *im, uint32_t usage, uint32_t state) {
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    if (!im->image) return r;
    r.resource = (void *)im->image;
    r.description = (struct FfxApiResourceDescription){.type = FFX_API_RESOURCE_TYPE_TEXTURE2D,
                                                       .format = surface_format(im->format), .width = im->width,
                                                       .height = im->height, .depth = 1, .mipCount = 1, .usage = usage};
    r.state = state;
    return r;
}

// Brings the context's constants in line with the TOTK_FSR_* settings: a changed or newly set one goes to ffxConfigure, an
// unset one that was set before restores the upscaler's own value (a NULL value). Runs at a context's first dispatch and
// after each settings change (cfg_version), not every frame. An upscaler that does not take a key (FSR 4's) is told so
// once in the log, and a value that is not a number is ignored the same way.
static void constants_sync(struct FfxCtx *fc) {
    unsigned v = cfg_version();
    if (!F.configure || (fc->constants_synced && fc->constants_seen == v)) return;
    fc->constants_synced = 1, fc->constants_seen = v;
    for (size_t i = 0; i < NCONSTANTS; i++) {
        const char *text = cfg_get(CONSTANTS[i].setting);
        float want = NAN;
        if (text && *text) {
            char *end;
            double d = strtod(text, &end);
            if (end != text && !*end && isfinite(d)) {
                want = (float)d;
            } else if (!(fc->constants_warned >> i & 1)) {
                fc->constants_warned |= 1u << i;
                rt_log("fsr: %s=%s is not a number: the upscaler's own value stays", CONSTANTS[i].setting, text);
            }
        }
        if (isnan(want) ? isnan(fc->applied[i]) : want == fc->applied[i]) continue;
        struct ffxConfigureDescUpscaleKeyValue kv = {
            .header = {FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE, NULL}, .key = CONSTANTS[i].key,
            .ptr = isnan(want) ? NULL : &want};
        ffxReturnCode_t r = F.configure(&fc->ctx, &kv.header);
        if (r != FFX_API_RETURN_OK) {
            if (!(fc->constants_warned >> i & 1)) {
                fc->constants_warned |= 1u << i;
                rt_log("fsr: the upscaler did not take %s (%u): it applies to FSR 3.1", CONSTANTS[i].setting, (unsigned)r);
            }
            continue;
        }
        fc->applied[i] = want;
        if (isnan(want)) rt_log("fsr: %s: the upscaler's own value again", CONSTANTS[i].setting);
        else rt_log("fsr: %s set to %g", CONSTANTS[i].setting, (double)want);
    }
}

// cameraNear and cameraFar the way the FSR SDK's sample integration hands them over (FidelityFX-SDK, the upscale
// render module): with depth 1 at the near plane the two swap roles, cameraNear is the far end of the range and
// cameraFar the near plane, and an infinite far plane is FLT_MAX, not a large finite distance. FSR 3.1 takes the
// convention from the DEPTH_INVERTED and DEPTH_INFINITE flags, so the order should make no difference to it; FSR 4's
// signed DLL makes no such statement, and the reference values are the ones it is tested with. TOTK_FSR_CAMERA_PLAIN=1
// compares against the raw values. near_d and far_d are the distances of the depth range's ends as vk_dlaa.c
// cam_params reads them (far_d is not used when the far plane is infinite).
static void camera_range(const struct FfxCtx *fc, float near_d, float far_d, float *cam_near, float *cam_far) {
    if (F.camera_plain) {
        *cam_near = near_d, *cam_far = far_d;
        return;
    }
    float far_end = fc->depth_infinite ? FLT_MAX : far_d;
    if (fc->depth_inverted) *cam_near = far_end, *cam_far = near_d;
    else *cam_near = near_d, *cam_far = far_end;
}

int vk_ffx_dispatch(void *ctx, VkCommandBuffer cb, const VkFfxDispatch *d) {
    struct FfxCtx *fc = ctx;
    if (!fc || !fc->ctx) return 0;
    if (!vk_ffx_fits(fc, d->color.width, d->color.height, d->out.width, d->out.height)) {
        rt_log("fsr: dispatch %ux%u -> %ux%u is outside the context (up to %ux%u -> %ux%u)", d->color.width,
               d->color.height, d->out.width, d->out.height, fc->max_w, fc->max_h, fc->ow, fc->oh);
        return 0;
    }
    constants_sync(fc);
    ffxContext c = fc->ctx;
    struct ffxDispatchDescUpscale u;
    memset(&u, 0, sizeof u);
    u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    u.commandList = (void *)cb;
    u.color = resource(&d->color, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.depth = resource(&d->depth, FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.motionVectors = resource(&d->mv, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    // The optional masks go along only where the context takes them (resource requirements)
    if (fc->uses_reactive)
        u.reactive = resource(&d->reactive, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    if (fc->uses_transparency)
        u.transparencyAndComposition =
            resource(&d->transparency, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    if (fc->needs_reactive && !d->reactive.image && !fc->warned_reactive) {
        fc->warned_reactive = 1;
        rt_log("fsr: the upscaler requires a reactive mask and none was made (dlss_char_mask = 0?)");
    }
    u.output = resource(&d->out, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    u.jitterOffset = (struct FfxApiFloatCoords2D){d->jx * F.jitter_sign, d->jy * F.jitter_sign};
    u.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};
    u.renderSize = (struct FfxApiDimensions2D){d->color.width, d->color.height};
    u.upscaleSize = (struct FfxApiDimensions2D){d->out.width, d->out.height};
    u.enableSharpening = d->sharpness > 0;
    u.sharpness = d->sharpness;
    u.frameTimeDelta = d->dt_ms;
    u.preExposure = 1.0f;
    u.reset = d->reset != 0;
    if (d->debug_view && fc->debug_view) {   // TODO(debug-view)
        u.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
        if (!fc->debug_logged) {
            fc->debug_logged = 1;
            rt_log("fsr: Debug view requested at dispatch (context made with FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION, flags %#x)",
                   (unsigned)u.flags);
        }
    }
    camera_range(fc, d->cam_near, d->cam_far, &u.cameraNear, &u.cameraFar);
    u.cameraFovAngleVertical = d->fov_y;
    u.viewSpaceToMetersFactor = 1.0f;   // TotK's world units are metres
    ffxReturnCode_t r = F.dispatch(&c, &u.header);
    if (r != FFX_API_RETURN_OK) {
        rt_log("fsr: dispatch failed (%u)", (unsigned)r);
        return 0;
    }
    return 1;
}
#endif
