/*
 * gui.c — AsasGUI window manager
 *
 * Features:
 *   - Double buffering (back buffer via kmalloc, blit at end of frame)
 *   - Event loop with mouse + keyboard state tracking
 *   - Draggable, closeable windows via title-bar hit testing
 *   - Terminal window: real output via logger hook, real input via shell hook
 *   - File Manager window: reads live directory listing from VFS
 *   - Improved 5x7 bitmap font: full printable ASCII coverage
 *   - Taskbar with window toggle buttons
 */

#include "safemode_gui.h"
#include "heap.h"
#include "keyboard.h"
#include "logger.h"
#include "mouse.h"
#include "scheduler.h"
#include "shell.h"
#include "vfs.h"

#pragma intrinsic(_InterlockedExchange)
long _InterlockedExchange(long volatile *target, long value);

/* ======================================================================
 * FONT — 5x7 pixel, column-major bitmap.
 * Each byte encodes one column; bit 0 = top pixel, bit 6 = bottom pixel.
 * ====================================================================== */

#define FONT_W  5
#define FONT_H  7
#define CELL_W  6   /* glyph width + 1px inter-character gap */
#define CELL_H  9   /* glyph height + 2px line gap */

static const UINT8 font_upper[26][5] = {
    {0x1E,0x05,0x05,0x1E,0x00},{0x1F,0x15,0x15,0x0A,0x00},{0x0E,0x11,0x11,0x0A,0x00},
    {0x1F,0x11,0x11,0x0E,0x00},{0x1F,0x15,0x15,0x11,0x00},{0x1F,0x05,0x05,0x01,0x00},
    {0x0E,0x11,0x15,0x1D,0x00},{0x1F,0x04,0x04,0x1F,0x00},{0x11,0x1F,0x11,0x00,0x00},
    {0x08,0x10,0x10,0x0F,0x00},{0x1F,0x04,0x0A,0x11,0x00},{0x1F,0x10,0x10,0x10,0x00},
    {0x1F,0x02,0x04,0x02,0x1F},{0x1F,0x02,0x04,0x1F,0x00},{0x0E,0x11,0x11,0x0E,0x00},
    {0x1F,0x05,0x05,0x02,0x00},{0x0E,0x11,0x19,0x1E,0x00},{0x1F,0x05,0x0D,0x12,0x00},
    {0x12,0x15,0x15,0x09,0x00},{0x01,0x1F,0x01,0x00,0x00},{0x0F,0x10,0x10,0x0F,0x00},
    {0x07,0x08,0x10,0x08,0x07},{0x1F,0x08,0x04,0x08,0x1F},{0x11,0x0A,0x04,0x0A,0x11},
    {0x01,0x02,0x1C,0x02,0x01},{0x19,0x15,0x13,0x00,0x00}
};

static const UINT8 font_digit[10][5] = {
    {0x0E,0x11,0x11,0x0E,0x00},{0x12,0x1F,0x10,0x00,0x00},{0x19,0x15,0x15,0x12,0x00},
    {0x11,0x15,0x15,0x0A,0x00},{0x07,0x04,0x1F,0x04,0x00},{0x17,0x15,0x15,0x09,0x00},
    {0x0E,0x15,0x15,0x08,0x00},{0x01,0x01,0x1D,0x03,0x00},{0x0A,0x15,0x15,0x0A,0x00},
    {0x02,0x15,0x15,0x0E,0x00}
};

/* Returns the column bitmap for a printable ASCII character. */
static UINT8 glyph_col(char ch, UINT32 col)
{
    if (ch >= 'A' && ch <= 'Z') { return font_upper[(int)(ch - 'A')][col]; }
    if (ch >= 'a' && ch <= 'z') { return font_upper[(int)(ch - 'a')][col]; }
    if (ch >= '0' && ch <= '9') { return font_digit[(int)(ch - '0')][col]; }
    switch (ch) {
    case '!': return (col == 1 || col == 2) ? 0x4F : 0;
    case '"': return (col == 0 || col == 2) ? 0x03 : 0;
    case '#': return (col == 1 || col == 3) ? 0x1F : 0x0A;
    case '$': return col==0?0x12:col==1?0x15:col==2?0x1F:col==3?0x15:0x09;
    case '%': return col==0?0x18:col==1?0x08:col==2?0x04:col==3?0x02:0x03;
    case '&': return col==0?0x0A:col==1?0x15:col==2?0x0A:col==3?0x14:0x08;
    case '\'': return col == 0 ? 0x03 : 0;
    case '(': return col==1?0x0E:col==2?0x11:0;
    case ')': return col==0?0x11:col==1?0x0E:0;
    case '*': return col==1?0x0A:col==2?0x1F:col==3?0x0A:0;
    case '+': return col==1?0x04:col==2?0x1F:col==3?0x04:0;
    case ',': return col==1?0x60:0;
    case '-': return col < 4 ? 0x04 : 0;
    case '.': return col==1?0x40:0;
    case '/': return col==0?0x10:col==1?0x08:col==2?0x04:col==3?0x02:0x01;
    case ':': return col==1?0x22:0;
    case ';': return col==1?0x62:0;
    case '<': return col==0?0x04:col==1?0x0A:col==2?0x11:0;
    case '=': return (col==1||col==2||col==3)?0x0A:0;
    case '>': return col==0?0x11:col==1?0x0A:col==2?0x04:0;
    case '?': return col==0?0x01:col==1?0x15:col==2?0x05:col==3?0x02:0;
    case '@': return col==0?0x0E:col==1?0x11:col==2?0x15:col==3?0x1D:0x0E;
    case '[': return col==1?0x7F:col==2?0x41:0;
    case '\\': return col==0?0x01:col==1?0x02:col==2?0x04:col==3?0x08:0x10;
    case ']': return col==0?0x41:col==1?0x7F:0;
    case '^': return col==1?0x02:col==2?0x01:col==3?0x02:0;
    case '_': return col < 5 ? 0x40 : 0;
    case '`': return col==0?0x01:col==1?0x02:0;
    case '{': return col==1?0x04:col==2?0x1B:col==3?0x11:0;
    case '|': return col==2?0x7F:0;
    case '}': return col==0?0x11:col==1?0x1B:col==2?0x04:0;
    case '~': return col==0?0x04:col==1?0x02:col==2?0x04:col==3?0x08:col==4?0x04:0;
    default:  return 0;
    }
}

/* ======================================================================
 * WINDOW DEFINITIONS
 * ====================================================================== */

#define GUI_WINDOW_TERMINAL  0
#define GUI_WINDOW_FILES     1
#define GUI_WINDOW_COUNT     2

#define TITLEBAR_H   18
#define CLOSE_BTN_W  14

typedef struct {
    int      x, y;
    UINT32   width, height;
    char     title[24];
    UINT32   accent;
    UINT8    minimized;
    UINT8    dragging;
    int      drag_off_x, drag_off_y;
} GUI_WINDOW;

/* ======================================================================
 * TERMINAL BUFFER
 * ====================================================================== */

#define TERM_HISTORY  96
#define TERM_COLS     58

static char   g_term_lines[TERM_HISTORY][TERM_COLS + 1];
static UINT32 g_term_head;      /* next write slot (ring) */
static UINT32 g_term_count;     /* lines filled (0..TERM_HISTORY) */
static char   g_input_line[TERM_COLS + 1];
static UINT32 g_input_len;
static int    g_terminal_dirty; /* set to 1 when terminal content changes */
static char   g_render_term_lines[TERM_HISTORY][TERM_COLS + 1];
static char   g_render_input_line[TERM_COLS + 1];

/* ======================================================================
 * FILE MANAGER STATE
 * ====================================================================== */

#define FILES_MAX  28

static VFS_DIRECTORY_ENTRY g_files[FILES_MAX];
static UINT64              g_file_count;

/* ======================================================================
 * WINDOW + CURSOR GLOBALS
 * ====================================================================== */

static GUI_WINDOW g_windows[GUI_WINDOW_COUNT];
static int        g_focused = -1;

static int   g_cursor_x, g_cursor_y;
static UINT8 g_prev_buttons;
static UINT32 g_loop_ticks;
static volatile long g_gui_lock;

/* ======================================================================
 * DOUBLE BUFFER
 * ====================================================================== */

static ASAS_FRAMEBUFFER  g_back_fb;
static UINT32           *g_back_buf;  /* NULL if kmalloc unavailable */
static ASAS_FRAMEBUFFER *g_real_fb;
static ASAS_FRAMEBUFFER *g_fb_ptr;   /* stored for thread entry */

static void gui_lock(void)
{
    while (_InterlockedExchange(&g_gui_lock, 1) != 0) {
        scheduler_yield();
    }
}

static void gui_unlock(void)
{
    (void)_InterlockedExchange(&g_gui_lock, 0);
}

static int gui_consume_terminal_dirty(void)
{
    int dirty;

    gui_lock();
    dirty = g_terminal_dirty;
    g_terminal_dirty = 0;
    gui_unlock();
    return dirty;
}

/* ======================================================================
 * SMALL STRING HELPERS (no stdlib)
 * ====================================================================== */

static int gui_str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void gui_str_copy(char *dst, const char *src, UINT32 max)
{
    UINT32 i;
    for (i = 0; i + 1 < max && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ======================================================================
 * DRAWING HELPERS (write to back buffer when available, else real fb)
 * ====================================================================== */

static ASAS_FRAMEBUFFER *draw_target(void)
{
    return g_back_buf ? &g_back_fb : g_real_fb;
}

static void gui_fill_rect(int x, int y, UINT32 w, UINT32 h, UINT32 color)
{
    ASAS_FRAMEBUFFER *fb = draw_target();

    /* Clip negative offsets */
    if (x < 0) {
        if ((UINT32)(-x) >= w) return;
        w -= (UINT32)(-x);
        x = 0;
    }
    if (y < 0) {
        if ((UINT32)(-y) >= h) return;
        h -= (UINT32)(-y);
        y = 0;
    }
    if ((UINT32)x >= fb->width || (UINT32)y >= fb->height) return;
    if ((UINT32)x + w > fb->width)  w = fb->width  - (UINT32)x;
    if ((UINT32)y + h > fb->height) h = fb->height - (UINT32)y;
    if (w == 0 || h == 0) return;
    framebuffer_fill_rect(fb, (UINT32)x, (UINT32)y, w, h, color);
}

static void gui_put_pixel(int x, int y, UINT32 color)
{
    ASAS_FRAMEBUFFER *fb = draw_target();
    if (x < 0 || y < 0) return;
    if ((UINT32)x >= fb->width || (UINT32)y >= fb->height) return;
    framebuffer_put_pixel(fb, (UINT32)x, (UINT32)y, color);
}

static void gui_draw_text(int x, int y, const char *text, UINT32 color)
{
    UINT32 col, row;
    char ch;

    while (*text) {
        ch = *text++;
        if (ch != ' ') {
            for (col = 0; col < FONT_W; col++) {
                UINT8 bits = glyph_col(ch, col);
                for (row = 0; row < FONT_H; row++) {
                    if (bits & (1U << row)) {
                        gui_put_pixel(x + (int)col, y + (int)row, color);
                    }
                }
            }
        }
        x += CELL_W;
    }
}

static void gui_draw_border(int x, int y, UINT32 w, UINT32 h, UINT32 color)
{
    gui_fill_rect(x,              y,              w, 1, color);
    gui_fill_rect(x,              y + (int)h - 1, w, 1, color);
    gui_fill_rect(x,              y,              1, h, color);
    gui_fill_rect(x + (int)w - 1, y,              1, h, color);
}

/* ======================================================================
 * WINDOW FRAME RENDERING
 * ====================================================================== */

static void render_close_button(int wx, int wy, UINT32 ww)
{
    int bx = wx + (int)ww - CLOSE_BTN_W - 3;
    int by = wy + 2;
    gui_fill_rect(bx, by, CLOSE_BTN_W, 14, 0xC0392B);
    gui_draw_text(bx + 4, by + 4, "X", 0xFFFFFF);
}

static void render_window_chrome(const GUI_WINDOW *w)
{
    UINT32 border_color;

    /* Body */
    gui_fill_rect(w->x, w->y, w->width, w->height, 0x151922);
    /* Titlebar */
    gui_fill_rect(w->x, w->y, w->width, TITLEBAR_H, w->accent);
    /* Title text */
    gui_draw_text(w->x + 8, w->y + 6, w->title, 0xF0F0F0);
    /* Close button */
    render_close_button(w->x, w->y, w->width);
    /* Border — bright when focused */
    border_color = (g_focused >= 0 && g_windows + g_focused == w)
                   ? 0xFFFFFF : 0x4A5568;
    gui_draw_border(w->x, w->y, w->width, w->height, border_color);
}

/* ======================================================================
 * TERMINAL WINDOW CONTENT
 * ====================================================================== */

static void render_terminal_content(const GUI_WINDOW *w)
{
    int cx = w->x + 6;
    int cy = w->y + TITLEBAR_H + 4;
    int cw = (int)w->width - 12;
    int ch = (int)w->height - TITLEBAR_H - 8;
    int max_lines, visible;
    UINT32 first, i;
    int ty;
    char prompt[TERM_COLS + 6];
    UINT32 term_head;
    UINT32 term_count;
    UINT32 input_len;
    UINT32 pi, j;

    /* Dark content background */
    gui_fill_rect(cx, cy, (UINT32)cw, (UINT32)ch, 0x0D1117);

    max_lines = ch / CELL_H;
    if (max_lines <= 0) return;

    gui_lock();
    for (i = 0; i < TERM_HISTORY; i++) {
        gui_str_copy(g_render_term_lines[i], g_term_lines[i], TERM_COLS + 1);
    }
    gui_str_copy(g_render_input_line, g_input_line, TERM_COLS + 1);
    term_head = g_term_head;
    term_count = g_term_count;
    input_len = g_input_len;
    gui_unlock();

    /* Show up to max_lines of the most recent terminal output */
    visible = (term_count < (UINT32)max_lines) ? (int)term_count : max_lines;
    first = (term_head + TERM_HISTORY - (UINT32)visible) % TERM_HISTORY;

    ty = cy;
    for (i = 0; i < (UINT32)visible; i++) {
        UINT32 idx = (first + i) % TERM_HISTORY;
        gui_draw_text(cx, ty, g_render_term_lines[idx], 0xCDD6F4);
        ty += CELL_H;
    }

    /* Current input prompt on the next line */
    if (ty + CELL_H <= cy + ch) {
        pi = 0;
        prompt[pi++] = '>';
        prompt[pi++] = ' ';
        for (j = 0; j < input_len && pi < TERM_COLS + 3; j++) {
            prompt[pi++] = g_render_input_line[j];
        }
        prompt[pi++] = '_'; /* cursor indicator */
        prompt[pi]   = '\0';
        gui_draw_text(cx, ty, prompt, 0x89B4FA);
    }
}

/* ======================================================================
 * FILE MANAGER WINDOW CONTENT
 * ====================================================================== */

static void render_files_content(const GUI_WINDOW *w)
{
    int cx = w->x + 6;
    int cy = w->y + TITLEBAR_H + 4;
    int cw = (int)w->width - 12;
    int ch = (int)w->height - TITLEBAR_H - 8;
    int ty;
    UINT64 i;
    char line[36];
    VFS_DIRECTORY_ENTRY files[FILES_MAX];
    UINT64 file_count;
    UINT32 li, ni;

    gui_fill_rect(cx, cy, (UINT32)cw, (UINT32)ch, 0x0D1117);

    gui_lock();
    file_count = g_file_count;
    if (file_count > FILES_MAX) {
        file_count = FILES_MAX;
    }
    for (i = 0; i < file_count; i++) {
        files[i] = g_files[i];
    }
    gui_unlock();

    /* Path header */
    gui_draw_text(cx, cy, "/", 0x89B4FA);
    ty = cy + CELL_H + 2;

    for (i = 0; i < file_count && ty + CELL_H <= cy + ch; i++) {
        UINT32 color = files[i].is_directory ? 0xF9C74F : 0xCDD6F4;

        li = 0;
        if (files[i].is_directory) { line[li++] = '['; }
        for (ni = 0; files[i].name[ni] && li < 33; ni++) {
            line[li++] = files[i].name[ni];
        }
        if (files[i].is_directory) { line[li++] = ']'; }
        line[li] = '\0';

        gui_draw_text(cx + 4, ty, line, color);
        ty += CELL_H + 1;
    }
}

/* ======================================================================
 * TASKBAR
 * ====================================================================== */

static void render_taskbar(UINT32 sw, UINT32 sh)
{
    int  ty = (int)sh - 30;
    UINT32 i;
    int  bx = 64;

    gui_fill_rect(0, ty, sw, 30, 0x0F1923);
    gui_draw_border(0, ty, sw, 30, 0x2D3748);
    gui_draw_text(8, ty + 11, "ASAS", 0x89B4FA);

    for (i = 0; i < GUI_WINDOW_COUNT; i++) {
        UINT32 bg = g_windows[i].minimized ? 0x1A2234 : 0x253347;
        UINT32 fg = g_windows[i].minimized ? 0x6B7280 : 0xE2E8F0;
        gui_fill_rect(bx, ty + 4, 72, 22, bg);
        gui_draw_border(bx, ty + 4, 72, 22, 0x4A5568);
        gui_draw_text(bx + 6, ty + 11, g_windows[i].title, fg);
        bx += 78;
    }
}

/* ======================================================================
 * MOUSE CURSOR
 * ====================================================================== */

static int update_cursor_pos(ASAS_FRAMEBUFFER *fb)
{
    const UINT32 absolute_max = 32767U;
    long long dx = 0;
    long long dy = 0;
    UINT8 buttons = 0;
    UINT32 absolute_x = 0;
    UINT32 absolute_y = 0;

    if (mouse_consume_absolute(&absolute_x, &absolute_y, &buttons)) {
        if (absolute_x > absolute_max) {
            absolute_x = absolute_max;
        }
        if (absolute_y > absolute_max) {
            absolute_y = absolute_max;
        }
        g_cursor_x = (int)(((UINT64)absolute_x * (UINT64)(fb->width - 1U)) / absolute_max);
        g_cursor_y = (int)(((UINT64)absolute_y * (UINT64)(fb->height - 1U)) / absolute_max);
        if (g_cursor_x < 0) g_cursor_x = 0;
        if (g_cursor_x + 14 >= (int)fb->width)  g_cursor_x = (int)fb->width  - 15;
        if (g_cursor_y < 0) g_cursor_y = 0;
        if (g_cursor_y + 18 >= (int)fb->height) g_cursor_y = (int)fb->height - 19;
        return 1;
    }

    mouse_consume_delta(&dx, &dy, &buttons);

    g_cursor_x += (int)dx;
    g_cursor_y -= (int)dy;
    if (g_cursor_x < 0) g_cursor_x = 0;
    if (g_cursor_x + 14 >= (int)fb->width)  g_cursor_x = (int)fb->width  - 15;
    if (g_cursor_y < 0) g_cursor_y = 0;
    if (g_cursor_y + 18 >= (int)fb->height) g_cursor_y = (int)fb->height - 19;

    return dx != 0 || dy != 0 || buttons != g_prev_buttons;
}

static void render_cursor(void)
{
    UINT32 k;
    int x = g_cursor_x, y = g_cursor_y;

    /* Arrow outline (black shadow) */
    for (k = 0; k <= 16; k++) gui_put_pixel(x + 1, y + (int)k, 0x000000);
    for (k = 0; k <= 11; k++) gui_put_pixel(x + (int)k + 1, y + (int)k + 1, 0x000000);

    /* Arrow fill (white) */
    for (k = 0; k <= 16; k++) gui_put_pixel(x, y + (int)k, 0xFFFFFF);
    for (k = 0; k <= 10; k++) gui_put_pixel(x + (int)k, y + (int)k, 0xFFFFFF);
    for (k = 0; k <= 4;  k++) gui_put_pixel(x + (int)k, y + 14 - (int)k, 0xFFFFFF);
}

/* ======================================================================
 * MOUSE EVENT HANDLING
 * ====================================================================== */

static int hit_titlebar(const GUI_WINDOW *w, int mx, int my)
{
    if (w->minimized) return 0;
    return mx >= w->x && mx < w->x + (int)w->width
        && my >= w->y && my < w->y + TITLEBAR_H;
}

static int hit_close_btn(const GUI_WINDOW *w, int mx, int my)
{
    int bx = w->x + (int)w->width - CLOSE_BTN_W - 3;
    if (w->minimized) return 0;
    return mx >= bx && mx < bx + CLOSE_BTN_W
        && my >= w->y + 2 && my < w->y + TITLEBAR_H - 2;
}

static int hit_window_body(const GUI_WINDOW *w, int mx, int my)
{
    if (w->minimized) return 0;
    return mx >= w->x && mx < w->x + (int)w->width
        && my >= w->y && my < w->y + (int)w->height;
}

static void handle_taskbar_click(UINT32 sw, UINT32 sh, int mx, int my)
{
    int ty = (int)sh - 30;
    UINT32 i;
    int bx = 64;
    (void)sw;

    for (i = 0; i < GUI_WINDOW_COUNT; i++) {
        if (mx >= bx && mx < bx + 72 && my >= ty + 4 && my < ty + 26) {
            g_windows[i].minimized = !g_windows[i].minimized;
            if (!g_windows[i].minimized) {
                g_focused = (int)i;
            }
        }
        bx += 78;
    }
}

static void handle_mouse_events(ASAS_FRAMEBUFFER *fb)
{
    UINT8  buttons  = mouse_buttons();
    int    mx = g_cursor_x, my = g_cursor_y;
    int    pressed  = (buttons & 1) && !(g_prev_buttons & 1);
    int    released = !(buttons & 1) && (g_prev_buttons & 1);
    UINT32 i;

    if (pressed) {
        /* Taskbar click (check first — it sits at the bottom) */
        if (my >= (int)fb->height - 30) {
            handle_taskbar_click(fb->width, fb->height, mx, my);
        } else {
            /* Window interactions (front window first = higher index) */
            for (i = GUI_WINDOW_COUNT; i-- > 0; ) {
                GUI_WINDOW *w = &g_windows[i];
                if (hit_close_btn(w, mx, my)) {
                    w->minimized = 1;
                    if (g_focused == (int)i) g_focused = -1;
                    break;
                }
                if (hit_titlebar(w, mx, my)) {
                    w->dragging    = 1;
                    w->drag_off_x  = mx - w->x;
                    w->drag_off_y  = my - w->y;
                    g_focused      = (int)i;
                    break;
                }
                if (hit_window_body(w, mx, my)) {
                    g_focused = (int)i;
                    break;
                }
            }
        }
    }

    /* Drag in progress */
    if ((buttons & 1) && g_focused >= 0 && g_windows[g_focused].dragging) {
        GUI_WINDOW *w = &g_windows[g_focused];
        w->x = mx - w->drag_off_x;
        w->y = my - w->drag_off_y;
        /* Keep titlebar on screen */
        if (w->x < 0) w->x = 0;
        if (w->x + (int)w->width  > (int)fb->width)  w->x = (int)fb->width  - (int)w->width;
        if (w->y < 28) w->y = 28;
        if (w->y > (int)fb->height - 32) w->y = (int)fb->height - 32;
    }

    if (released) {
        for (i = 0; i < GUI_WINDOW_COUNT; i++) {
            g_windows[i].dragging = 0;
        }
    }

    g_prev_buttons = buttons;
}

/* ======================================================================
 * FILE LIST REFRESH
 * ====================================================================== */

static void refresh_files(void)
{
    VFS_DIRECTORY_ENTRY files[FILES_MAX];
    UINT64 count;
    UINT64 i;

    count = vfs_list_directory("/", files, FILES_MAX);

    gui_lock();
    g_file_count = count;
    if (g_file_count > FILES_MAX) {
        g_file_count = FILES_MAX;
    }
    for (i = 0; i < g_file_count; i++) {
        g_files[i] = files[i];
    }
    gui_unlock();
}

/* ======================================================================
 * FULL FRAME RENDER + BLIT
 * ====================================================================== */

static void render_frame(ASAS_FRAMEBUFFER *fb)
{
    ASAS_FRAMEBUFFER *target = draw_target();
    UINT32 row, col;

    /* Desktop background */
    framebuffer_clear(target, 0x1A2332);

    /* Windows back-to-front */
    if (!g_windows[GUI_WINDOW_FILES].minimized) {
        render_window_chrome(&g_windows[GUI_WINDOW_FILES]);
        render_files_content(&g_windows[GUI_WINDOW_FILES]);
    }
    if (!g_windows[GUI_WINDOW_TERMINAL].minimized) {
        render_window_chrome(&g_windows[GUI_WINDOW_TERMINAL]);
        render_terminal_content(&g_windows[GUI_WINDOW_TERMINAL]);
    }

    /* Taskbar and cursor always on top */
    render_taskbar(fb->width, fb->height);
    render_cursor();

    /* Blit back buffer → real framebuffer (row-by-row, respects stride) */
    if (g_back_buf) {
        for (row = 0; row < fb->height; row++) {
            UINT32 *src_row = g_back_buf + row * fb->stride;
            UINT32 *dst_row = fb->base   + row * fb->stride;
            for (col = 0; col < fb->width; col++) {
                dst_row[col] = src_row[col];
            }
        }
    }
}

/* ======================================================================
 * LOGGER HOOK — forwards SHELL/FILE/DIR messages to the terminal buffer
 * ====================================================================== */

static void gui_logger_hook(const char *level, const char *message)
{
    char line[TERM_COLS + 1];
    UINT32 li = 0;
    UINT32 mi = 0;

    /* Forward shell messages and directory listings */
    if (!gui_str_eq(level, "SHELL") &&
        !gui_str_eq(level, "FILE")  &&
        !gui_str_eq(level, "DIR")) {
        return;
    }

    /* Indent file/dir entries for readability */
    if (!gui_str_eq(level, "SHELL")) {
        line[li++] = ' ';
        line[li++] = ' ';
        if (gui_str_eq(level, "DIR")) {
            line[li++] = '['; line[li++] = 'D'; line[li++] = ']'; line[li++] = ' ';
        }
    }

    while (message[mi] && li < TERM_COLS) {
        line[li++] = message[mi++];
    }
    line[li] = '\0';

    safemode_gui_terminal_write(line);
}

/* ======================================================================
 * PUBLIC API
 * ====================================================================== */

void safemode_gui_terminal_write(const char *text)
{
    UINT32 i = 0;
    char  *dst;

    gui_lock();
    dst = g_term_lines[g_term_head];

    while (text[i] && i < TERM_COLS) {
        dst[i] = text[i];
        i++;
    }
    dst[i]      = '\0';
    g_term_head = (g_term_head + 1) % TERM_HISTORY;
    if (g_term_count < TERM_HISTORY) g_term_count++;
    g_terminal_dirty = 1;
    gui_unlock();
}

void safemode_gui_set_input_line(const char *line, UINT32 length)
{
    UINT32 i;

    gui_lock();
    for (i = 0; i < length && i < TERM_COLS; i++) {
        g_input_line[i] = line[i];
    }
    g_input_line[i] = '\0';
    g_input_len      = i;
    g_terminal_dirty = 1;
    gui_unlock();
}

void safemode_gui_initialize(ASAS_FRAMEBUFFER *framebuffer)
{
    UINTN  buf_size;
    UINT32 i;

    g_fb_ptr  = framebuffer;
    g_real_fb = framebuffer;

    /* Allocate back buffer (stride*height covers any row padding) */
    buf_size   = (UINTN)framebuffer->stride * framebuffer->height * sizeof(UINT32);
    g_back_buf = (UINT32 *)kmalloc(buf_size);
    if (g_back_buf) {
        g_back_fb      = *framebuffer;
        g_back_fb.base = g_back_buf;
        g_back_fb.size = buf_size;
    }

    /* Terminal window: left 60% x 62% */
    g_windows[GUI_WINDOW_TERMINAL].x       = 18;
    g_windows[GUI_WINDOW_TERMINAL].y       = 36;
    g_windows[GUI_WINDOW_TERMINAL].width   = framebuffer->width * 60 / 100;
    g_windows[GUI_WINDOW_TERMINAL].height  = framebuffer->height * 62 / 100;
    gui_str_copy(g_windows[GUI_WINDOW_TERMINAL].title, "TERMINAL", 24);
    g_windows[GUI_WINDOW_TERMINAL].accent    = 0x1E6091;
    g_windows[GUI_WINDOW_TERMINAL].minimized = 0;
    g_windows[GUI_WINDOW_TERMINAL].dragging  = 0;

    /* Files window: right 34% x 56% */
    g_windows[GUI_WINDOW_FILES].x       = (int)(framebuffer->width * 64 / 100);
    g_windows[GUI_WINDOW_FILES].y       = 36;
    g_windows[GUI_WINDOW_FILES].width   = framebuffer->width * 34 / 100;
    g_windows[GUI_WINDOW_FILES].height  = framebuffer->height * 56 / 100;
    gui_str_copy(g_windows[GUI_WINDOW_FILES].title, "FILES", 24);
    g_windows[GUI_WINDOW_FILES].accent    = 0x6B3FA0;
    g_windows[GUI_WINDOW_FILES].minimized = 0;
    g_windows[GUI_WINDOW_FILES].dragging  = 0;

    /* Clear terminal ring buffer */
    for (i = 0; i < TERM_HISTORY; i++) {
        g_term_lines[i][0] = '\0';
    }
    g_term_head      = 0;
    g_term_count     = 0;
    g_input_line[0]  = '\0';
    g_input_len      = 0;
    g_terminal_dirty = 1;

    /* Cursor */
    g_cursor_x    = (int)framebuffer->width  / 2;
    g_cursor_y    = (int)framebuffer->height / 2;
    g_prev_buttons = 0;
    g_focused      = GUI_WINDOW_TERMINAL;

    /* Initial file listing */
    refresh_files();

    /* Welcome lines */
    safemode_gui_terminal_write("Asas OS ready.");
    safemode_gui_terminal_write("type 'help' for commands");
    safemode_gui_terminal_write("----------------------------");

    /* Register logger hook — now SHELL/FILE/DIR output appears in terminal */
    logger_set_gui_callback(gui_logger_hook);

    logger_write("INFO", "AsasGUI initialized");
}

void safemode_gui_thread_entry(void)
{
    UINT32 file_refresh_timer = 0;

    for (;;) {
        int    dirty = 0;

        g_loop_ticks++;
        shell_poll_input_once();

        /* Update cursor position from raw mouse deltas */
        if (update_cursor_pos(g_fb_ptr)) {
            handle_mouse_events(g_fb_ptr);
            dirty = 1;
        }

        /* Terminal / input line changes */
        if (gui_consume_terminal_dirty()) {
            dirty = 1;
        }

        /* Refresh file list every ~1500 scheduler yields */
        file_refresh_timer++;
        if (file_refresh_timer >= 1500) {
            file_refresh_timer = 0;
            refresh_files();
            dirty = 1;
        }

        if (dirty) {
            render_frame(g_fb_ptr);
        }

        scheduler_yield();
    }
}

UINT32 safemode_gui_loop_ticks(void)
{
    return g_loop_ticks;
}

void safemode_gui_render_desktop(ASAS_FRAMEBUFFER *framebuffer)
{
    /* Legacy entry point kept for backward compatibility.
       safemode_gui_initialize() + safemode_gui_thread_entry() is the preferred path. */
    (void)framebuffer;
}
