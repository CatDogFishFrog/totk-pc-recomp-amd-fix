// The sources of FSR's two optional mask inputs, the Reactive mask and the Transparency and composition (T&C) mask
// (fsr_mask.c). Every way a pixel can get into one of the masks is a row of one table, switched by its own setting:
// the Video tab of the menu builds its rows from the table (ui/menu.cpp rows_fsr_masks), and vk_dlaa.c reads the
// frame's plan from it. A source is independent of the others: the enabled ones combine, per pixel by the larger
// value, into the mask of their target.
//
// Methods (what reads the picture to find the pixels):
//   G-buffer class: the green channel of the G-buffer's target 0 carries an object class (Link, creatures, held items,
//     foliage); dlaa_mv.comp compares it with the class's band. Reactive and T&C take it separately.
//   Alpha replay: the alpha-blended draws into the light target are recorded a second time into an R8 image that keeps
//     what stays see-through (vk_exec_draw.c alpha_draw); the shader turns it into coverage. The two blend kinds that
//     qualify are two sources, and each can feed Reactive, T&C or both.
// Which pixels a source marks is a hypothesis to test in the Debug view of the Video tab, not a known property of the
// game's materials: docs/dlaa.md, "What goes into each mask".
#pragma once
#include "ui_text.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FSRM_REACTIVE, FSRM_TC, FSRM_TARGETS } FsrMaskTarget;
typedef enum { FSRM_GBUFFER_CLASS, FSRM_ALPHA_REPLAY } FsrMaskMethod;

// G-buffer classes. The first three are the characters (they share the Character trails strength and the previous-frame
// footprint, dlaa_mv.comp); the order is the shader's, which treats classes 0..2 as characters.
typedef enum { FSRM_CLASS_LINK, FSRM_CLASS_CREATURE, FSRM_CLASS_HELD, FSRM_CLASS_FOLIAGE, FSRM_CLASSES } FsrMaskClass;
#define FSRM_CHARACTER_CLASSES 3

// The blend kinds of the alpha replay (vk_exec_draw.c alpha_blend_class): destination factor ONE_MINUS_SRC_ALPHA (blend
// and premultiplied blend), and source factor SRC_ALPHA with destination ONE (additive scaled by alpha).
typedef enum { FSRM_ALPHA_BLEND, FSRM_ALPHA_ADD, FSRM_ALPHA_CLASSES } FsrMaskAlpha;

// Bits of a destination set (alpha replay: which mask images a draw is recorded into; plan: which masks are written).
#define FSRM_DEST_REACTIVE 1u
#define FSRM_DEST_TC 2u

typedef struct {
    const char *key;           // the on/off setting, with its TOTK_ prefix
    FsrMaskTarget target;
    FsrMaskMethod method;
    int what;                  // FsrMaskClass or FsrMaskAlpha, by method
    UiTextId label, desc;      // the menu row
    const char *level_key;     // the setting for the strength in percent (a second row under it), NULL: none
    UiTextId level_label, level_desc;
} FsrMaskSource;

int fsrmask_source_count(void);
const FsrMaskSource *fsrmask_source(int i);   // NULL past the last

// What one frame's mask pass does. Filled by fsrmask_plan from the settings, for the upscaler in use and for the masks
// its context takes.
typedef struct {
    struct {
        int lo, hi;            // the class's band of the G-buffer's green channel, 0..255, inclusive
        float reactive, tc;    // the value a pixel of the class gives each mask; 0: the class feeds nothing
    } cls[FSRM_CLASSES];
    float footprint;           // the value the previous frame's character footprint gives the Reactive mask; 0: no footprint
    float alpha_cap[FSRM_TARGETS];   // the most the alpha replay puts into each mask
    unsigned alpha_dests;      // union of the destinations the alpha replay's blend kinds feed (FSRM_DEST_*)
    unsigned classes_dests;    // destinations some class feeds: with a class source on, the pass needs the G-buffer
} FsrMaskPlan;

// The plan. fsr 0: DLSS, whose one bias mask takes the characters at char_level (the three character classes, no
// footprint of their own setting) and nothing else; the sources and their settings are FSR's. char_level: the Character
// trails strength, 0..1 (vk_dlaa.c char_bias). take_reactive, take_tc: whether the FSR context takes that input
// (vk_ffx_uses_reactive, vk_ffx_uses_transparency); a mask it does not take gets no source. One thread only (the one
// that records the DLAA frame).
void fsrmask_plan(FsrMaskPlan *p, int fsr, float char_level, int take_reactive, int take_tc);

// The destinations an alpha replay draw of this blend kind is recorded into, from the settings (FSRM_DEST_* bits, 0:
// the draw is not recorded). Any thread: the replay asks this per blend draw, so it reads a cache that follows
// cfg_version. Does not know which masks the FSR context takes (vk_dlaa.c vk_dlaa_alpha_dests masks that).
unsigned fsrmask_alpha_dests(int blend_class);

// For the menu's rows (implemented in vk_dlaa.c): 1 when an FSR context exists, and then whether it takes the Reactive and
// the T&C input. 0, outputs untouched, before the first context.
int vk_dlaa_fsr_inputs(int *reactive, int *tc);

#ifdef __cplusplus
}
#endif
