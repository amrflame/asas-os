/*
 * gui_notify.c — Desktop toast notification system for ASAS Desktop Environment
 *
 * Maintains a ring of up to GUI_NOTIFY_MAX toast entries.
 * Each toast slides in from the right, holds, then fades out (opacity via
 * stipple dithering — no alpha blending in this layer).
 */

#include "gui_notify.h"
#include "gui_draw.h"
#include "gui_theme.h"

/* ======================================================================
 * Internal types
 * ====================================================================== */
typedef struct {
    char   msg[48];          /* message text (truncated)        */
    int    level;            /* 0=info 1=warn 2=error           */
    UINT32 tick;             /* frames since spawned            */
    UINT8  active;
} NOTIFY_ENTRY;

/* ======================================================================
 * State
 * ====================================================================== */
static NOTIFY_ENTRY s_entries[GUI_NOTIFY_MAX];
static UINT32       s_count;

/* ======================================================================
 * Helpers
 * ====================================================================== */
static UINT32 snlen(const char *s)
{
    UINT32 n = 0;
    while (s[n]) n++;
    return n;
}

static void sncopy(char *dst, const char *src, UINT32 max)
{
    UINT32 i = 0;
    while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ======================================================================
 * Public API
 * ====================================================================== */

void gui_notify_initialize(void)
{
    UINT32 i;
    for (i = 0; i < GUI_NOTIFY_MAX; i++) {
        s_entries[i].active = 0;
        s_entries[i].tick   = 0;
    }
    s_count = 0;
}

void gui_notify_push_level(const char *msg, int level)
{
    UINT32 i, slot = GUI_NOTIFY_MAX;

    /* Find a free slot (prefer oldest first) */
    for (i = 0; i < GUI_NOTIFY_MAX; i++) {
        if (!s_entries[i].active) { slot = i; break; }
    }
    /* If all busy, evict the oldest */
    if (slot == GUI_NOTIFY_MAX) {
        UINT32 oldest = 0;
        for (i = 1; i < GUI_NOTIFY_MAX; i++) {
            if (s_entries[i].tick > s_entries[oldest].tick) oldest = i;
        }
        slot = oldest;
    }

    s_entries[slot].active = 1;
    s_entries[slot].tick   = 0;
    s_entries[slot].level  = level;
    sncopy(s_entries[slot].msg, msg, 48);
    if (s_count < GUI_NOTIFY_MAX) s_count++;
}

void gui_notify_push(const char *msg)
{
    gui_notify_push_level(msg, GUI_NOTIFY_INFO);
}

void gui_notify_tick(void)
{
    UINT32 i;
    for (i = 0; i < GUI_NOTIFY_MAX; i++) {
        if (!s_entries[i].active) continue;
        s_entries[i].tick++;
        if (s_entries[i].tick >= (UINT32)(GUI_NOTIFY_DURATION + GUI_NOTIFY_SLIDE)) {
            s_entries[i].active = 0;
            if (s_count > 0) s_count--;
        }
    }
}

void gui_notify_paint(UINT32 screen_w, UINT32 screen_h)
{
    UINT32 i, row = 0;
    UINT32 right_x, base_y;

    if (!screen_w || !screen_h) return;

    right_x = screen_w;
    /* Stack toasts above the taskbar (TASKBAR_H) with a small gap */
    base_y = screen_h - (UINT32)TASKBAR_H;

    for (i = 0; i < GUI_NOTIFY_MAX; i++) {
        int tx, ty;
        UINT32 accent, bg, icon_color;
        const char *icon;

        if (!s_entries[i].active) continue;

        /* Slide-in: for first SLIDE ticks, move from right edge inward */
        {
            UINT32 t = s_entries[i].tick;
            int    offset = 0; /* pixels from right edge */
            if (t < (UINT32)GUI_NOTIFY_SLIDE) {
                /* linear slide: starts at GUI_NOTIFY_W, ends at 0 */
                offset = (int)((GUI_NOTIFY_W * ((UINT32)GUI_NOTIFY_SLIDE - t))
                               / (UINT32)GUI_NOTIFY_SLIDE);
            }
            tx = (int)right_x - GUI_NOTIFY_W - GUI_NOTIFY_PAD + offset;
        }

        ty = (int)base_y - (int)(row + 1) * (GUI_NOTIFY_H + GUI_NOTIFY_PAD);

        /* Fade-out: last 20 ticks, skip every other row of pixels */
        UINT32 total = (UINT32)(GUI_NOTIFY_DURATION + GUI_NOTIFY_SLIDE);
        UINT32 fade_start = total - 20u;
        UINT8  fading = (s_entries[i].tick >= fade_start) ? 1 : 0;

        /* Per-level colors */
        if (s_entries[i].level == GUI_NOTIFY_ERROR) {
            bg = 0xFF2D1010; accent = 0xFFBB3333; icon = "X"; icon_color = 0xFFEE6666;
        } else if (s_entries[i].level == GUI_NOTIFY_WARN) {
            bg = 0xFF2D2A10; accent = 0xFFBBAA33; icon = "!"; icon_color = 0xFFEECC55;
        } else {
            bg = 0xFF102030; accent = C_ACCENT;   icon = "i"; icon_color = C_ACCENT;
        }

        /* Main toast body */
        if (!fading) {
            gui_fill_rounded(tx, ty, GUI_NOTIFY_W, GUI_NOTIFY_H, bg, 4);
            /* Left accent stripe */
            gui_fill_rect(tx, ty + 4, 3, GUI_NOTIFY_H - 8, accent);
            /* Icon circle background */
            gui_fill_rounded(tx + 8, ty + 9, 20, 20, accent, 10);
            gui_draw_text_centered(tx + 8, ty + 9, 20, 20, icon, 0xFFFFFFFF);
            /* Message text */
            gui_draw_text_n(tx + 32, ty + (GUI_NOTIFY_H - FONT_H) / 2,
                            s_entries[i].msg, 28, C_TEXT_PRIMARY);
        } else {
            /* Simple stipple fade: paint only if (px+py) is even */
            UINT32 px_w = (UINT32)GUI_NOTIFY_W;
            UINT32 px_h = (UINT32)GUI_NOTIFY_H;
            UINT32 cx, cy;
            for (cy = 0; cy < px_h; cy++) {
                for (cx = 0; cx < px_w; cx++) {
                    if (((cx + cy) & 1) == 0)
                        gui_put_pixel(tx + (int)cx, ty + (int)cy, bg);
                }
            }
        }

        row++;
    }
}
