// The sources of FSR's Reactive and T&C mask inputs: the table, the settings behind it and the per-frame plan the mask
// pass (vk_dlaa.c mv_pass, shaders/dlaa_mv.comp) and the alpha replay (vk_exec_draw.c alpha_draw) work from. The
// description of the idea and of the headers' words is in fsr_mask.h; what the masks hold and what is established
// about it is in docs/dlaa.md.
#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "fsr_mask.h"

// The bands of the G-buffer's green channel (0..255) that select a class. Measured on single frames: Link 253, other
// characters and creatures 125, held items 160, foliage 32, terrain 0 (dlaa_mv.comp, docs/dlaa.md). The three character
// bands are contiguous and together are exactly the single threshold the characters' mask had before (above 64), so
// with all three on the characters' mask is unchanged; the borders between them are the midpoints of the measured
// values. Foliage is a narrow comparison round its one measured value, not a threshold, so that it cannot grow into
// the character bands or into classes nobody measured; if foliage is missing in the Debug view, widen it here.
static const struct { int lo, hi; } BANDS[FSRM_CLASSES] = {
    [FSRM_CLASS_LINK] = {207, 255},
    [FSRM_CLASS_CREATURE] = {65, 142},
    [FSRM_CLASS_HELD] = {143, 206},
    [FSRM_CLASS_FOLIAGE] = {28, 36},
};

// The most each mask is given: FSR's guide advises against 1.0 for a reactive value (it would take the history off those
// pixels entirely); the T&C mask has no such advice, and 1 is its top.
static const float TOP[FSRM_TARGETS] = {0.9f, 1.0f};

#define R FSRM_REACTIVE
#define T FSRM_TC
#define G FSRM_GBUFFER_CLASS
#define A FSRM_ALPHA_REPLAY
#define D FSRM_DRAW_TYPE
// The table. The order is the order of the rows in the menu: each target's sources together. Every key has a row in
// cfg.c (known, groups, metas) and in totk.ini.example; the level keys are percent.
static const FsrMaskSource SRC[] = {
    {"TOTK_FSR_MASK_R_LINK", R, G, FSRM_CLASS_LINK, UI_MENU_FSRM_L_LINK, UI_MENU_FSRM_R_LINK_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_R_CREATURES", R, G, FSRM_CLASS_CREATURE, UI_MENU_FSRM_L_CREATURE, UI_MENU_FSRM_R_CREATURE_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_R_HELD", R, G, FSRM_CLASS_HELD, UI_MENU_FSRM_L_HELD, UI_MENU_FSRM_R_HELD_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_R_FOLIAGE", R, G, FSRM_CLASS_FOLIAGE, UI_MENU_FSRM_L_FOLIAGE, UI_MENU_FSRM_R_FOLIAGE_DESC,
     "TOTK_FSR_MASK_R_FOLIAGE_LEVEL", UI_MENU_FSRM_LEVEL, UI_MENU_FSRM_R_FOLIAGE_LEVEL_DESC},
    {"TOTK_FSR_MASK_R_ALPHA_BLEND", R, A, FSRM_ALPHA_BLEND, UI_MENU_FSRM_L_ALPHA_BLEND, UI_MENU_FSRM_R_ALPHA_BLEND_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_R_ALPHA_ADD", R, A, FSRM_ALPHA_ADD, UI_MENU_FSRM_L_ALPHA_ADD, UI_MENU_FSRM_R_ALPHA_ADD_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_R_DRAW_LINE_LOOP", R, D, FSRM_DRAW_LINE_LOOP, UI_MENU_FSRM_L_DRAW_LINE_LOOP, UI_MENU_FSRM_R_DRAW_LINE_LOOP_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_R_DRAW_BYTE_INDEX", R, D, FSRM_DRAW_BYTE_INDEX, UI_MENU_FSRM_L_DRAW_BYTE_INDEX, UI_MENU_FSRM_R_DRAW_BYTE_INDEX_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_R_DRAW_INDIRECT", R, D, FSRM_DRAW_INDIRECT, UI_MENU_FSRM_L_DRAW_INDIRECT, UI_MENU_FSRM_R_DRAW_INDIRECT_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_TC_FOLIAGE", T, G, FSRM_CLASS_FOLIAGE, UI_MENU_FSRM_L_FOLIAGE, UI_MENU_FSRM_TC_FOLIAGE_DESC,
     "TOTK_FSR_MASK_TC_FOLIAGE_LEVEL", UI_MENU_FSRM_LEVEL, UI_MENU_FSRM_TC_FOLIAGE_LEVEL_DESC},
    {"TOTK_FSR_MASK_TC_ALPHA_BLEND", T, A, FSRM_ALPHA_BLEND, UI_MENU_FSRM_L_ALPHA_BLEND, UI_MENU_FSRM_TC_ALPHA_BLEND_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_TC_ALPHA_ADD", T, A, FSRM_ALPHA_ADD, UI_MENU_FSRM_L_ALPHA_ADD, UI_MENU_FSRM_TC_ALPHA_ADD_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_TC_DRAW_LINE_LOOP", T, D, FSRM_DRAW_LINE_LOOP, UI_MENU_FSRM_L_DRAW_LINE_LOOP, UI_MENU_FSRM_TC_DRAW_LINE_LOOP_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_TC_DRAW_BYTE_INDEX", T, D, FSRM_DRAW_BYTE_INDEX, UI_MENU_FSRM_L_DRAW_BYTE_INDEX, UI_MENU_FSRM_TC_DRAW_BYTE_INDEX_DESC, NULL, 0, 0},
    {"TOTK_FSR_MASK_TC_DRAW_INDIRECT", T, D, FSRM_DRAW_INDIRECT, UI_MENU_FSRM_L_DRAW_INDIRECT, UI_MENU_FSRM_TC_DRAW_INDIRECT_DESC, NULL, 0, 0},
};
#undef R
#undef T
#undef G
#undef A
#undef D
#define NSRC ((int)(sizeof SRC / sizeof SRC[0]))

int fsrmask_source_count(void) { return NSRC; }
const FsrMaskSource *fsrmask_source(int i) { return i >= 0 && i < NSRC ? &SRC[i] : NULL; }

static unsigned dest_bit(FsrMaskTarget t) { return t == FSRM_TC ? FSRM_DEST_TC : FSRM_DEST_REACTIVE; }

// TOTK_FSR_ALPHA=0, the switch of the alpha replay before it had a row per source: both alpha sources off, whatever
// their own settings say. Kept so a totk.ini or a test script that sets it still does.
static int alpha_switched_off(void) {
    const char *v = cfg_get("TOTK_FSR_ALPHA");
    return v && !strcmp(v, "0");
}

static float level_of(const FsrMaskSource *s, float char_level) {
    if (s->method == FSRM_GBUFFER_CLASS && s->what < FSRM_CHARACTER_CLASSES) return char_level;   // Character trails
    if (!s->level_key) return TOP[s->target];
    double v = cfg_double(s->level_key) / 100.0;
    return v <= 0 ? 0.0f : v >= TOP[s->target] ? TOP[s->target] : (float)v;
}

void fsrmask_plan(FsrMaskPlan *p, int fsr, float char_level, int take_reactive, int take_tc) {
    memset(p, 0, sizeof *p);
    for (int i = 0; i < FSRM_CLASSES; i++) p->cls[i].lo = BANDS[i].lo, p->cls[i].hi = BANDS[i].hi;
    p->alpha_cap[FSRM_REACTIVE] = TOP[FSRM_REACTIVE], p->alpha_cap[FSRM_TC] = TOP[FSRM_TC];
    if (!fsr) {   // DLSS has the one bias mask: the characters, all three classes, at the Character trails strength
        for (int i = 0; i < FSRM_CHARACTER_CLASSES; i++) p->cls[i].reactive = char_level;
        p->footprint = char_level;
        p->classes_dests = char_level > 0 ? FSRM_DEST_REACTIVE : 0;
        return;
    }
    // The settings, read again when one changed (this runs on one thread, so the copy needs no lock)
    static unsigned seen = ~0u;
    static int on[NSRC], legacy_off;
    static float lvl[NSRC];
    static float seen_char = -1.0f;
    unsigned v = cfg_version();
    if (v != seen || char_level != seen_char) {
        for (int i = 0; i < NSRC; i++) on[i] = cfg_bool(SRC[i].key), lvl[i] = level_of(&SRC[i], char_level);
        legacy_off = alpha_switched_off();
        seen = v, seen_char = char_level;
    }
    for (int i = 0; i < NSRC; i++) {
        const FsrMaskSource *s = &SRC[i];
        if (!on[i] || !(s->target == FSRM_TC ? take_tc : take_reactive)) continue;
        unsigned d = dest_bit(s->target);
        if (s->method == FSRM_GBUFFER_CLASS) {
            if (lvl[i] <= 0) continue;
            if (s->target == FSRM_TC) p->cls[s->what].tc = lvl[i];
            else p->cls[s->what].reactive = lvl[i];
            p->classes_dests |= d;
        } else if (s->method == FSRM_ALPHA_REPLAY) {
            if (!legacy_off) p->alpha_dests |= d;
        }
        // FSRM_DRAW_TYPE: no coverage of its own, so nothing in the plan; the replay asks fsrmask_draw_dests per draw
    }
    for (int i = 0; i < FSRM_CHARACTER_CLASSES; i++)
        if (p->cls[i].reactive > 0) p->footprint = char_level;   // where an enabled character class just was
}

// The replay's destinations per blend kind and per draw type, from the settings. The replay asks for every blend draw on
// several threads, so it keeps one answer per cfg_version: whoever sees a stale answer works it out again (the same one
// from the same settings) and publishes it. The cache holds the blend kinds first, then the draw types.
#define DEST_SLOTS (FSRM_ALPHA_CLASSES + FSRM_DRAW_TYPES)
static _Atomic unsigned dest_cache[DEST_SLOTS];
static _Atomic unsigned dest_seen = ~0u;

static unsigned dest_slot(int slot) {
    unsigned v = cfg_version();
    if (atomic_load_explicit(&dest_seen, memory_order_acquire) != v) {
        unsigned d[DEST_SLOTS] = {0};
        for (int i = 0; i < NSRC; i++) {
            if (!cfg_bool(SRC[i].key)) continue;
            if (SRC[i].method == FSRM_ALPHA_REPLAY) {
                if (!alpha_switched_off()) d[SRC[i].what] |= dest_bit(SRC[i].target);
            } else if (SRC[i].method == FSRM_DRAW_TYPE) {
                d[FSRM_ALPHA_CLASSES + SRC[i].what] |= dest_bit(SRC[i].target);
            }
        }
        for (int i = 0; i < DEST_SLOTS; i++) atomic_store_explicit(&dest_cache[i], d[i], memory_order_relaxed);
        atomic_store_explicit(&dest_seen, v, memory_order_release);
    }
    return atomic_load_explicit(&dest_cache[slot], memory_order_relaxed);
}

unsigned fsrmask_alpha_dests(int blend_class) {
    return (unsigned)blend_class < FSRM_ALPHA_CLASSES ? dest_slot(blend_class) : 0;
}

unsigned fsrmask_draw_dests(int draw_type) {
    return (unsigned)draw_type < FSRM_DRAW_TYPES ? dest_slot(FSRM_ALPHA_CLASSES + draw_type) : 0;
}
