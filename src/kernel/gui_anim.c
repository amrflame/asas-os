/*
 * gui_anim.c — Window animation system for ASAS Desktop Environment
 *
 * Easing: ease_out_cubic  for OPEN/RESTORE  (fast start, smooth landing)
 *         ease_in_quad    for CLOSE/MINIMIZE (smooth start, fast exit)
 *
 * All arithmetic is integer-only (no floats) to stay freestanding.
 * Fixed-point: Q8 scale (256 = 1.0), Q8 alpha (255 = opaque).
 *
 * tick/total ratio mapped through easing table at 16 points (0–15).
 */

#include "gui_anim.h"
#include "gui_wm.h"

/* ======================================================================
 * Easing look-up tables (index 0..15 → output 0..256)
 * ease_out_cubic:  t^3 curve, decelerating
 * ease_in_quad:    t^2 curve, accelerating
 * ====================================================================== */

/* ease_out_cubic(t) = 1 - (1-t)^3, scaled 0..256 at 16 steps */
static const UINT32 s_ease_out[16] = {
     0,  36,  68,  96, 120, 140, 158, 172,
   184, 195, 205, 213, 220, 226, 232, 238
};

/* ease_in_quad(t) = t^2, scaled 0..256 at 16 steps */
static const UINT32 s_ease_in[16] = {
     0,   2,   7,  16,  28,  44,  64,  87,
   114, 144, 177, 213, 252, 256, 256, 256
};

/* ======================================================================
 * State table — one entry per window
 * ====================================================================== */
static GUI_ANIM_STATE s_anims[GUI_WIN_COUNT];

/* ======================================================================
 * Internal: map tick/total → easing table value (0..256)
 * ====================================================================== */
static UINT32 ease_lookup(const UINT32 *table, UINT32 tick, UINT32 total)
{
    UINT32 idx;
    if (total == 0 || tick >= total) return 256;
    /* Map [0, total) → [0, 15] */
    idx = tick * 15u / (total - 1u > 0u ? total - 1u : 1u);
    if (idx > 15u) idx = 15u;
    return table[idx];
}

/* ======================================================================
 * Public API
 * ====================================================================== */

void gui_anim_initialize(void)
{
    UINT32 i;
    for (i = 0; i < GUI_WIN_COUNT; i++) {
        s_anims[i].type   = GUI_ANIM_NONE;
        s_anims[i].tick   = 0;
        s_anims[i].total  = 0;
        s_anims[i].win_id = (int)i;
    }
}

void gui_anim_tick(void)
{
    UINT32 i;
    for (i = 0; i < GUI_WIN_COUNT; i++) {
        GUI_ANIM_STATE *a = &s_anims[i];
        if (a->type == GUI_ANIM_NONE) continue;
        if (a->tick < a->total) {
            a->tick++;
        }
        /* Animation finished? */
        if (a->tick >= a->total) {
            if (a->type == GUI_ANIM_CLOSE || a->type == GUI_ANIM_MINIMIZE) {
                /* WM will call gui_anim_should_hide() and hide the window */
            } else {
                /* OPEN / RESTORE done — clear state */
                a->type = GUI_ANIM_NONE;
            }
        }
    }
}

void gui_anim_start(int win_id, UINT32 type)
{
    GUI_ANIM_STATE *a;
    if (win_id < 0 || win_id >= GUI_WIN_COUNT) return;
    a = &s_anims[win_id];
    a->type  = type;
    a->tick  = 0;
    switch (type) {
    case GUI_ANIM_OPEN:     a->total = 15; break;
    case GUI_ANIM_CLOSE:    a->total = 10; break;
    case GUI_ANIM_MINIMIZE: a->total = 12; break;
    case GUI_ANIM_RESTORE:  a->total = 12; break;
    default:                a->total = 0;  a->type = GUI_ANIM_NONE; break;
    }
}

int gui_anim_active(int win_id)
{
    if (win_id < 0 || win_id >= GUI_WIN_COUNT) return 0;
    return s_anims[win_id].type != GUI_ANIM_NONE
        && s_anims[win_id].tick < s_anims[win_id].total;
}

UINT32 gui_anim_get_scale(int win_id)
{
    GUI_ANIM_STATE *a;
    UINT32 base;

    if (win_id < 0 || win_id >= GUI_WIN_COUNT) return 256;
    a = &s_anims[win_id];
    if (a->type == GUI_ANIM_NONE || a->tick >= a->total) return 256;

    switch (a->type) {
    case GUI_ANIM_OPEN:
    case GUI_ANIM_RESTORE:
        /* 80%→100% (205→256), ease_out_cubic */
        base = ease_lookup(s_ease_out, a->tick, a->total);
        /* Remap 0..256 → 205..256 */
        return 205u + (base * 51u / 256u);

    case GUI_ANIM_CLOSE:
    case GUI_ANIM_MINIMIZE:
        /* 100%→0% (256→0), ease_in_quad (inverted) */
        base = ease_lookup(s_ease_in, a->tick, a->total);
        /* Invert: 256 - base gives 256→0 */
        return (base <= 256u) ? 256u - base : 0u;

    default:
        return 256;
    }
}

UINT32 gui_anim_get_alpha(int win_id)
{
    GUI_ANIM_STATE *a;
    UINT32 base;

    if (win_id < 0 || win_id >= GUI_WIN_COUNT) return 255;
    a = &s_anims[win_id];
    if (a->type == GUI_ANIM_NONE || a->tick >= a->total) return 255;

    switch (a->type) {
    case GUI_ANIM_OPEN:
    case GUI_ANIM_RESTORE:
        /* Fade in 0→255 */
        base = ease_lookup(s_ease_out, a->tick, a->total);
        return base; /* 0..256 ≈ 0..255 */

    case GUI_ANIM_CLOSE:
    case GUI_ANIM_MINIMIZE:
        /* Fade out 255→0 */
        base = ease_lookup(s_ease_in, a->tick, a->total);
        return (base <= 255u) ? 255u - base : 0u;

    default:
        return 255;
    }
}

int gui_anim_should_hide(int win_id)
{
    GUI_ANIM_STATE *a;
    if (win_id < 0 || win_id >= GUI_WIN_COUNT) return 0;
    a = &s_anims[win_id];
    return (a->type == GUI_ANIM_CLOSE || a->type == GUI_ANIM_MINIMIZE)
        && a->tick >= a->total;
}
