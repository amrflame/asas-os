/*
 * gui_app_terminal.c — Terminal application window
 *
 * Hooks into the logger (SHELL/FILE/DIR) and shell input-line updates.
 * Renders a dark terminal with green prompt and scrollback history.
 */

#include "gui_wm.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_compositor.h"
#include "logger.h"
#include "scheduler.h"
#include "shell.h"
#include "keyboard.h"

#pragma intrinsic(_InterlockedExchange)
long _InterlockedExchange(long volatile *target, long value);

/* Forward declaration — defined later in this file */
void app_terminal_write(const char *text);

#define TERM_HISTORY 96
#define TERM_COLS    58

static char   s_lines[TERM_HISTORY][TERM_COLS + 1];
static UINT32 s_head, s_count;
static char   s_input[TERM_COLS + 1];
static UINT32 s_input_len;
static volatile long s_lock;
static int    s_dirty;
static int    s_scroll_offset;  /* 0 = bottom (live), >0 = scrolled up */

static void term_lock  (void) { while (_InterlockedExchange(&s_lock, 1) != 0) scheduler_yield(); }
static void term_unlock(void) { (void)_InterlockedExchange(&s_lock, 0); }

/* ---- Copy helpers (no stdlib) ---- */
static void scopy(char *dst, const char *src, UINT32 max)
{
    UINT32 i = 0;
    while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static int sequ(const char *a, const char *b)
{
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

/* ---- Logger hook ---- */
static void term_logger_hook(const char *level, const char *msg)
{
    char line[TERM_COLS + 1];
    UINT32 li = 0, mi = 0;

    if (!sequ(level, "SHELL") && !sequ(level, "FILE") && !sequ(level, "DIR")) return;

    if (!sequ(level, "SHELL")) {
        line[li++] = ' '; line[li++] = ' ';
        if (sequ(level, "DIR")) {
            line[li++]='['; line[li++]='D'; line[li++]=']'; line[li++]=' ';
        }
    }
    while (msg[mi] && li < TERM_COLS) line[li++] = msg[mi++];
    line[li] = '\0';

    app_terminal_write(line);
}

/* ---- Public write functions (called from gui_main.c dispatcher) ---- */
void app_terminal_write(const char *text)
{
    UINT32 i = 0;
    char  *dst;

    term_lock();
    dst = s_lines[s_head];
    while (text[i] && i < TERM_COLS) { dst[i] = text[i]; i++; }
    dst[i]   = '\0';
    s_head   = (s_head + 1) % TERM_HISTORY;
    if (s_count < TERM_HISTORY) s_count++;
    s_dirty  = 1;
    term_unlock();
}

void app_terminal_set_input(const char *line, UINT32 length)
{
    UINT32 i;
    term_lock();
    for (i = 0; i < length && i < TERM_COLS; i++) s_input[i] = line[i];
    s_input[i]   = '\0';
    s_input_len  = i;
    s_dirty      = 1;
    term_unlock();
}

int app_terminal_consume_dirty(void)
{
    int d;
    term_lock();
    d = s_dirty;
    s_dirty = 0;
    term_unlock();
    return d;
}

void app_terminal_scroll(int delta)
{
    /* delta > 0 = scroll up (older history), delta < 0 = scroll down */
    s_scroll_offset -= delta;   /* wheel up gives +1, which means go back = increase offset */
    if (s_scroll_offset < 0) s_scroll_offset = 0;
    s_dirty = 1;
}

/* ---- Paint callback ---- */
static char s_snap_lines[TERM_HISTORY][TERM_COLS + 1];
static char s_snap_input[TERM_COLS + 1];
static UINT32 s_snap_head, s_snap_count, s_snap_ilen;

static void terminal_paint(GUI_WIN *win)
{
    int    cx = win->x + 4;
    int    cy = win->y + CHROME_H + 4;
    int    cw = (int)win->w - 8;
    int    ch = (int)win->h - CHROME_H - 8;
    int    max_lines, visible_n;
    UINT32 first, i;
    int    ty;

    /* Content background */
    gui_fill_rect(cx, cy, (UINT32)cw, (UINT32)ch, C_TERM_BG);

    max_lines = ch / CELL_H;
    if (max_lines <= 0) return;

    /* Snapshot under lock */
    term_lock();
    for (i = 0; i < TERM_HISTORY; i++) scopy(s_snap_lines[i], s_lines[i], TERM_COLS + 1);
    scopy(s_snap_input, s_input, TERM_COLS + 1);
    s_snap_head  = s_head;
    s_snap_count = s_count;
    s_snap_ilen  = s_input_len;
    term_unlock();

    /* Clamp scroll offset */
    {
        int max_scroll = (int)s_snap_count - max_lines;
        if (max_scroll < 0) max_scroll = 0;
        if (s_scroll_offset > max_scroll) s_scroll_offset = max_scroll;
        if (s_scroll_offset < 0) s_scroll_offset = 0;
    }

    /* When scrolled up, show older lines and hide input prompt */
    visible_n = ((int)s_snap_count < max_lines) ? (int)s_snap_count : max_lines;
    first     = (s_snap_head + TERM_HISTORY
                 - (UINT32)visible_n
                 - (UINT32)s_scroll_offset) % TERM_HISTORY;

    ty = cy;
    for (i = 0; i < (UINT32)visible_n; i++) {
        UINT32 idx = (first + i) % TERM_HISTORY;
        UINT32 col = C_TERM_OUTPUT;
        /* Colour hints */
        if (s_snap_lines[idx][0] == '>' && s_snap_lines[idx][1] == ' ')
            col = C_TERM_PROMPT;
        gui_draw_text(cx + 2, ty, s_snap_lines[idx], col);
        ty += CELL_H;
    }

    /* Input prompt — only show when at bottom (not scrolled up) */
    if (s_scroll_offset == 0 && ty + CELL_H <= cy + ch) {
        /* Build prompt string in two colors:
         * "root@asas:/home" in green, "$" in white, then input text */
        static const char pfx[] = "root@asas:/home";
        int px2 = cx + 2;
        UINT32 k = 0;

        /* Green username@host:path */
        gui_draw_text(px2, ty, pfx, C_TERM_PROMPT);
        px2 += (int)gui_text_width(pfx);

        /* White "$" */
        gui_draw_char(px2, ty, '$', C_TEXT_PRIMARY);
        px2 += CELL_W;
        gui_draw_char(px2, ty, ' ', C_TEXT_PRIMARY);
        px2 += CELL_W;

        /* Input text */
        for (k = 0; k < s_snap_ilen; k++) {
            gui_draw_char(px2, ty, s_snap_input[k], C_INPUT_TEXT);
            px2 += CELL_W;
        }
        /* Blinking cursor block */
        if ((gui_compositor_loop_ticks() / 30) % 2 == 0) {
            gui_fill_rect(px2, ty, (UINT32)CELL_W, (UINT32)FONT_H, C_TERM_CURSOR);
        }
    }

    /* Scroll indicator (when scrolled up) */
    if (s_scroll_offset > 0) {
        char sbuf[16];
        UINT32 si = 0;
        sbuf[si++]='['; sbuf[si++]='+';
        gui_uint_to_str((UINT32)s_scroll_offset, sbuf + si, 6);
        while (sbuf[si]) si++;
        sbuf[si++]=']'; sbuf[si] = '\0';
        gui_fill_rect(cx + cw - 42, cy + 1, 40, (UINT32)CELL_H + 2, 0xFF2A3A4A);
        gui_draw_text(cx + cw - 40, cy + 2, sbuf, C_ACCENT);
    }
}

/* ---- Key callback — forward to shell, handle scrollback ---- */
static void terminal_on_key(GUI_WIN *win, UINT8 scancode)
{
    (void)win;
    /* PgUp (0x49) — scroll up */
    if (scancode == 0x49) {
        s_scroll_offset += 4;
        s_dirty = 1;
        return;
    }
    /* PgDn (0x51) — scroll down */
    if (scancode == 0x51) {
        s_scroll_offset -= 4;
        if (s_scroll_offset < 0) s_scroll_offset = 0;
        s_dirty = 1;
        return;
    }
    /* Home (0x47) — jump to top */
    if (scancode == 0x47) {
        s_scroll_offset = (int)s_count;
        s_dirty = 1;
        return;
    }
    /* End (0x4F) — jump to bottom */
    if (scancode == 0x4F) {
        s_scroll_offset = 0;
        s_dirty = 1;
        return;
    }
    /* All other keys: scroll back to live view and forward to shell */
    s_scroll_offset = 0;
    keyboard_inject_scancode(scancode);
}

/* ---- Initialise ---- */
void app_terminal_initialize(void)
{
    UINT32 i;
    for (i = 0; i < TERM_HISTORY; i++) s_lines[i][0] = '\0';
    s_head = s_count = s_input_len = s_dirty = 0;
    s_input[0] = '\0';
    s_lock = 0;
    s_scroll_offset = 0;

    gui_wm_set_callbacks(GUI_WIN_TERMINAL, terminal_paint, terminal_on_key);
    logger_set_gui_callback(term_logger_hook);

    app_terminal_write("ASAS OS v1.0 - ASAS Desktop Environment");
    app_terminal_write("type 'help' for available commands");
    app_terminal_write("--------------------------------------");
}
