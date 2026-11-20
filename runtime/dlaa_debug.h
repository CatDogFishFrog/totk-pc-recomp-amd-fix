// TODO(debug-view): remove this whole file, its users and the "Debug" rows of the Video tab after the tests.
// Debug view of the AMD FSR upscaler (vk_dlaa.c, vk_ffx.c) and its labels (ui/menu.cpp). Not a cfg setting: the state is
// held in memory, set from the Video tab of the menu, and is off at every start.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// mode
#define DLAA_DEBUG_OFF 0
#define DLAA_DEBUG_FSR 1      // FSR 3.1's own Debug view (FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW): FSR's internal surfaces
#define DLAA_DEBUG_INPUTS 2   // the inputs this project hands to FSR, drawn over the picture
#define DLAA_DEBUG_BOTH 3     // FSR's Debug view, with the inputs in the place of its "Upscaled frame" band
// color (inputs only)
#define DLAA_DEBUG_COLOR_NO 0     // the technical buffers only
#define DLAA_DEBUG_COLOR_ONLY 1   // the Color buffer (input) only
#define DLAA_DEBUG_COLOR_BOTH 2   // each tile split: the technical buffer left, the Color buffer (input) right

// view (inputs view): the grid of the four technical buffers, or one of them over the whole area
#define DLAA_DEBUG_VIEW_GRID 0
#define DLAA_DEBUG_VIEW_REACTIVE 1   // Reactive mask (input)
#define DLAA_DEBUG_VIEW_TRANSP 2     // T&C mask (input)
#define DLAA_DEBUG_VIEW_MV 3         // Motion vectors (input)
#define DLAA_DEBUG_VIEW_DEPTH 4      // Depth buffer (input)
#define DLAA_DEBUG_VIEWS 5

int dlaa_debug_mode(void);
int dlaa_debug_view(void);
int dlaa_debug_color(void);
void dlaa_debug_set(int mode, int color, int view);
int dlaa_debug_available(void);   // AMD FSR is the upscaler at work (the rows of the menu say so when not)

typedef struct {
    const char *label;     // a static string
    float x, y, w, h;      // the rectangle the label belongs to, as fractions of the upscaled picture
} DlaaDebugLabel;

// The labels of the debug picture the last frame really drew (none when no debug picture was drawn, so a label never
// stands over a normal frame). Returns the count (at most max).
int dlaa_debug_labels(DlaaDebugLabel *out, int max);

#ifdef __cplusplus
}
#endif
