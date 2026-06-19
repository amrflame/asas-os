#ifndef ASAS_GUI_ANIM_H
#define ASAS_GUI_ANIM_H

/*
 * gui_anim.h — Window animation system for ASAS Desktop Environment
 *
 * Provides open/close/minimize animations using CPU-side easing at 100Hz.
 * Each window has an independent animation state. During animation the WM
 * calls gui_anim_get_scale() to render a scaled/clipped version of the window.
 *
 * Animation types:
 *   OPEN     — scale from 80%→100% + fade-in (15 ticks = 150ms)
 *   CLOSE    — scale from 100%→0%  + fade-out (10 ticks = 100ms)
 *   MINIMIZE — shrink toward taskbar (12 ticks = 120ms)
 *   RESTORE  — expand from taskbar  (12 ticks = 120ms)
 */

#include "uefi.h"

/* ---- Animation type IDs ---- */
#define GUI_ANIM_NONE     0u
#define GUI_ANIM_OPEN     1u
#define GUI_ANIM_CLOSE    2u
#define GUI_ANIM_MINIMIZE 3u
#define GUI_ANIM_RESTORE  4u

/* ---- Per-window animation state ---- */
typedef struct {
    UINT32 type;        /* GUI_ANIM_* */
    UINT32 tick;        /* current frame 0..total */
    UINT32 total;       /* total frames for this animation */
    int    win_id;      /* which window (-1 = unused) */
} GUI_ANIM_STATE;

/* Initialise the animation system (zero all states). */
void gui_anim_initialize(void);

/* Advance all running animations by one tick (call once per render loop). */
void gui_anim_tick(void);

/* Start an animation for a window.
 * Replaces any existing animation for that window. */
void gui_anim_start(int win_id, UINT32 type);

/* Returns non-zero if the window currently has an active animation. */
int gui_anim_active(int win_id);

/*
 * Returns the animation scale factor for a window as a fixed-point value
 * in the range [0, 256]: 256 = 100%, 0 = 0%, 205 = 80%.
 * Returns 256 if no animation is running (full size).
 */
UINT32 gui_anim_get_scale(int win_id);

/*
 * Returns the animation alpha (opacity) 0–255 for the window.
 * Used for future blending. Returns 255 if no animation running.
 */
UINT32 gui_anim_get_alpha(int win_id);

/*
 * Returns 1 if the window animation has completed and the window
 * should be hidden (end of CLOSE or MINIMIZE animation).
 */
int gui_anim_should_hide(int win_id);

#endif /* ASAS_GUI_ANIM_H */
