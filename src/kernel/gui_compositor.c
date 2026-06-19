/*
 * gui_compositor.c — Back-buffer manager, input state, cursor renderer
 */

#include "gui_compositor.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gfx.h"
#include "framebuffer.h"
#include "heap.h"
#include "mouse.h"
#include "keyboard.h"
#include "scheduler.h"

#pragma intrinsic(_InterlockedExchange)
long _InterlockedExchange(long volatile *target, long value);

/* ======================================================================
 * Wallpaper colour table: 5 presets {top, bottom}
 * ====================================================================== */
static const UINT32 s_wallpapers[WALLPAPER_COUNT][2] = {
    { 0xFF0D1117, 0xFF111827 },  /* 0 — Midnight Blue  (default) */
    { 0xFF0D0F1A, 0xFF1A1040 },  /* 1 — Deep Purple                */
    { 0xFF0A1418, 0xFF0D2A28 },  /* 2 — Teal Dark                  */
    { 0xFF111111, 0xFF1E1E1E },  /* 3 — Charcoal Grey              */
    { 0xFF000000, 0xFF0A0A0A },  /* 4 — Pure Black                 */
};

/* ======================================================================
 * Compositor state
 * ====================================================================== */
static ASAS_FRAMEBUFFER  s_real_fb;
static UINT32           *s_back_buf;
static ASAS_FRAMEBUFFER  s_back_fb;
static volatile long     s_lock;
static UINT32            s_loop_ticks;
static UINT32            s_wallpaper_idx;

static int   s_cursor_x,  s_cursor_y;
static UINT8 s_buttons,   s_prev_buttons;

void gui_compositor_initialize(ASAS_FRAMEBUFFER *real_fb)
{
    UINTN buf_size;

    s_real_fb      = *real_fb;
    s_loop_ticks   = 0;
    s_wallpaper_idx = 0;
    s_lock         = 0;

    buf_size  = (UINTN)real_fb->stride * real_fb->height * sizeof(UINT32);
    s_back_buf = (UINT32 *)kmalloc(buf_size);
    if (s_back_buf) {
        s_back_fb      = *real_fb;
        s_back_fb.base = s_back_buf;
        s_back_fb.size = buf_size;
    }

    s_cursor_x = (int)(real_fb->width  / 2);
    s_cursor_y = (int)(real_fb->height / 2);

    gui_draw_set_target(
        s_back_buf ? s_back_buf : real_fb->base,
        real_fb->width, real_fb->height, real_fb->stride
    );

    /* Attach backbuffer to GFX layer; initialises VirtIO GPU if probed. */
    gfx_attach_backbuf(s_back_buf, real_fb->width, real_fb->height, real_fb->stride);
}

void gui_compositor_lock(void)
{
    while (_InterlockedExchange(&s_lock, 1) != 0) scheduler_yield();
}

void gui_compositor_unlock(void)
{
    (void)_InterlockedExchange(&s_lock, 0);
}

void gui_compositor_clear(UINT32 color)
{
    UINT32 r, c;
    UINT32 *buf = s_back_buf ? s_back_buf : s_real_fb.base;
    for (r = 0; r < s_real_fb.height; r++) {
        UINT32 *row = buf + r * s_real_fb.stride;
        for (c = 0; c < s_real_fb.width; c++) row[c] = color;
    }
}

void gui_compositor_blit(void)
{
    UINT32 r, c;
    if (!s_back_buf) return;

    /* GPU path: DMA transfer + scanout flush (QEMU VirtIO GPU) */
    if (gfx_gpu_active()) {
        gfx_flush(0, 0, s_real_fb.width, s_real_fb.height);
        return;
    }

    /* CPU path: row-by-row memcpy to UEFI framebuffer (Hyper-V / bare metal) */
    for (r = 0; r < s_real_fb.height; r++) {
        UINT32 *src = s_back_buf     + r * s_real_fb.stride;
        UINT32 *dst = s_real_fb.base + r * s_real_fb.stride;
        for (c = 0; c < s_real_fb.width; c++) dst[c] = src[c];
    }
}

/* ======================================================================
 * Input update
 * ====================================================================== */
int gui_compositor_update_input(void)
{
    const UINT32 ABS_MAX = 32767U;
    long long dx = 0, dy = 0;
    UINT8 new_buttons = 0;
    UINT32 abs_x = 0, abs_y = 0;
    int moved = 0;

    if (mouse_consume_absolute(&abs_x, &abs_y, &new_buttons)) {
        if (abs_x > ABS_MAX) abs_x = ABS_MAX;
        if (abs_y > ABS_MAX) abs_y = ABS_MAX;
        s_cursor_x = (int)((UINT64)abs_x * (s_real_fb.width  - 1) / ABS_MAX);
        s_cursor_y = (int)((UINT64)abs_y * (s_real_fb.height - 1) / ABS_MAX);
        moved = 1;
    } else {
        mouse_consume_delta(&dx, &dy, &new_buttons);
        s_cursor_x += (int)dx;
        s_cursor_y -= (int)dy;
        moved = (dx != 0 || dy != 0);
    }

    /* Clamp */
    if (s_cursor_x < 0) s_cursor_x = 0;
    if (s_cursor_x + 14 >= (int)s_real_fb.width)  s_cursor_x = (int)s_real_fb.width  - 15;
    if (s_cursor_y < 0) s_cursor_y = 0;
    if (s_cursor_y + 18 >= (int)s_real_fb.height) s_cursor_y = (int)s_real_fb.height - 19;

    s_buttons = new_buttons;
    return moved || (new_buttons != s_prev_buttons);
}

/* ======================================================================
 * Mouse cursor arrow
 * ====================================================================== */
void gui_compositor_render_cursor(void)
{
    UINT32 k;
    int x = s_cursor_x, y = s_cursor_y;
    for (k = 0; k <= 16; k++) gui_put_pixel(x+1, y+(int)k, 0xFF000000);
    for (k = 0; k <= 11; k++) gui_put_pixel(x+(int)k+1, y+(int)k+1, 0xFF000000);
    for (k = 0; k <= 16; k++) gui_put_pixel(x,   y+(int)k, 0xFFFFFFFF);
    for (k = 0; k <= 10; k++) gui_put_pixel(x+(int)k, y+(int)k, 0xFFFFFFFF);
    for (k = 0; k <= 4;  k++) gui_put_pixel(x+(int)k, y+14-(int)k, 0xFFFFFFFF);
}

/* ======================================================================
 * Wallpaper
 * ====================================================================== */
void gui_compositor_set_wallpaper(UINT32 index)
{
    if (index < WALLPAPER_COUNT) s_wallpaper_idx = index;
}

UINT32 gui_compositor_get_wallpaper(void)
{
    return s_wallpaper_idx;
}

/* Draws the desktop background gradient using current wallpaper preset */
void gui_compositor_draw_wallpaper(void)
{
    UINT32 top = s_wallpapers[s_wallpaper_idx][0];
    UINT32 bot = s_wallpapers[s_wallpaper_idx][1];
    /* Leave bottom TASKBAR_H rows for taskbar — drawn separately */
    UINT32 desktop_h = s_real_fb.height > TASKBAR_H
                       ? s_real_fb.height - TASKBAR_H : s_real_fb.height;
    gui_fill_gradient_v(0, 0, s_real_fb.width, desktop_h, top, bot);
}

/* ======================================================================
 * Accessors
 * ====================================================================== */
UINT32 gui_compositor_width (void) { return s_real_fb.width;  }
UINT32 gui_compositor_height(void) { return s_real_fb.height; }
int    gui_compositor_cursor_x(void) { return s_cursor_x; }
int    gui_compositor_cursor_y(void) { return s_cursor_y; }
UINT8  gui_compositor_buttons(void) { return s_buttons; }
UINT8  gui_compositor_prev_buttons(void) { return s_prev_buttons; }

void gui_compositor_consume_buttons(void) { s_prev_buttons = s_buttons; }

UINT32 gui_compositor_loop_ticks(void) { return s_loop_ticks; }

void gui_compositor_tick(void) { s_loop_ticks++; }
