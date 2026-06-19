/*
 * gui_app_editor.c — Text Editor: toolbar, line numbers, content area, status bar
 */

#include "gui_wm.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_compositor.h"
#include "gui_widget.h"
#include "vfs.h"
#include "keyboard.h"

#define EDITOR_LINES_MAX 128
#define EDITOR_LINE_LEN  120
#define TOOLBAR_H        26
#define STATUSBAR_H      20
#define LINENUM_W        28

static char   s_lines[EDITOR_LINES_MAX][EDITOR_LINE_LEN + 1];
static UINT32 s_line_count;
static UINT32 s_cursor_line, s_cursor_col;
static UINT32 s_scroll;
static char   s_filename[32];
static UINT8  s_dirty_flag;

/* Toolbar widgets */
static GUI_WIDGET s_btn_new, s_btn_save, s_sep_toolbar;
/* Scrollbar widget */
static GUI_WIDGET s_scrollbar;
static int        s_sb_drag_y = -1;   /* y at mouse-down on scrollbar (-1=not dragging) */

/* Forward declaration needed by editor_on_key */
void app_editor_handle_click(int mx, int my);

static void scopy(char *dst, const char *src, UINT32 max)
{
    UINT32 i = 0;
    while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void load_file(const char *path)
{
    UINT64 handle = vfs_open(path);
    static char buf[EDITOR_LINES_MAX * EDITOR_LINE_LEN];
    UINT64 n;
    UINT32 li = 0, ci = 0, i;

    s_line_count  = 1;
    s_lines[0][0] = '\0';

    if (!handle) return;
    n = vfs_read(handle, buf, sizeof(buf) - 1);
    vfs_close(handle);
    if (!n) return;

    buf[n] = '\0';
    for (i = 0; i < n && li < EDITOR_LINES_MAX; i++) {
        if (buf[i] == '\n') {
            s_lines[li][ci] = '\0';
            li++; ci = 0;
            s_lines[li][0] = '\0';
            s_line_count = li + 1;
        } else if (buf[i] != '\r' && ci < EDITOR_LINE_LEN) {
            s_lines[li][ci++] = buf[i];
        }
    }
    s_lines[li][ci] = '\0';
    s_line_count = li + 1;
    s_dirty_flag = 0;
}

static void editor_paint(GUI_WIN *win)
{
    int wx = win->x, wy = win->y + CHROME_H;
    int ww = (int)win->w, wh = (int)win->h - CHROME_H;
    int cx, cy, ch;
    int visible_lines, i;

    /* Toolbar */
    gui_fill_rect(wx, wy, (UINT32)ww, (UINT32)TOOLBAR_H, C_WIN_CHROME);
    gui_fill_rect(wx, wy + TOOLBAR_H - 1, (UINT32)ww, 1, C_BORDER);

    /* Toolbar widgets — reposition each frame to follow window */
    s_btn_new.x = wx + 6;  s_btn_new.y = wy + 4;
    s_btn_save.x = wx + 42; s_btn_save.y = wy + 4;
    gui_widget_render(&s_btn_new);
    gui_widget_render(&s_btn_save);

    /* Filename tab */
    if (s_filename[0]) {
        gui_fill_rounded(wx + 80, wy + 4, 72, 18, C_ACCENT_DIM, 2);
        gui_draw_text_n(wx + 84, wy + 8, s_filename, 10, C_TEXT_PRIMARY);
    }

    /* Dirty indicator dot */
    if (s_dirty_flag)
        gui_fill_rounded(wx + ww - 18, wy + 9, 6, 6, C_ACCENT, 3);

    /* A- A+ cosmetic */
    gui_draw_text_right(wx, wy + 8, (UINT32)ww - 6, "A-  A+", C_TEXT_MUTED);

    wy += TOOLBAR_H;
    wh -= TOOLBAR_H + STATUSBAR_H;

    /* Line numbers background */
    gui_fill_rect(wx, wy, (UINT32)LINENUM_W, (UINT32)wh, C_SIDEBAR_BG);
    gui_fill_rect(wx + LINENUM_W, wy, 1, (UINT32)wh, C_BORDER);

    /* Content area */
    cx = wx + LINENUM_W + 4;
    cy = wy;
    ch = wh;
    /* Leave 12px on the right for the scrollbar */
    gui_fill_rect(cx - 4, cy, (UINT32)(ww - LINENUM_W - 12), (UINT32)ch, C_WIN_BODY);

    /* Sync + render scrollbar */
    {
        UINT32 vis   = (UINT32)(ch / CELL_H);
        UINT32 max_s = (s_line_count > vis) ? s_line_count - vis : 0;
        s_scrollbar.x = wx + ww - 12;
        s_scrollbar.y = wy;
        s_scrollbar.h = (UINT32)wh;
        /* Update packed value if line count changed */
        s_scrollbar.value    = ((max_s & 0xFFFFu) << 16) | (vis & 0xFFFFu);
        s_scrollbar.enabled  = (max_s > 0) ? 1 : 0;
        if (s_scroll > max_s) s_scroll = max_s;
        s_scrollbar.input_len = s_scroll;
        gui_widget_render(&s_scrollbar);
    }

    visible_lines = ch / CELL_H;

    for (i = 0; i < visible_lines; i++) {
        UINT32 line_idx = s_scroll + (UINT32)i;
        int    ly       = cy + i * CELL_H;
        char   num[6];

        if (line_idx >= s_line_count) break;

        /* Line number */
        gui_uint_to_str(line_idx + 1, num, 6);
        gui_draw_text_right(wx, ly + 1, (UINT32)LINENUM_W - 2, num, C_TEXT_LABEL);

        /* Cursor line highlight */
        if (line_idx == s_cursor_line) {
            gui_fill_rect(cx - 4, ly, (UINT32)(ww - LINENUM_W), (UINT32)CELL_H,
                          0xFF1A2640);
        }

        /* Line content */
        gui_draw_text_n(cx, ly + 1, s_lines[line_idx], 64, C_TEXT_PRIMARY);

        /* Cursor */
        if (line_idx == s_cursor_line) {
            int cur_x = cx + (int)(s_cursor_col * CELL_W);
            if ((gui_compositor_loop_ticks() / 30) % 2 == 0) {
                gui_fill_rect(cur_x, ly + 1, 1, FONT_H, C_INPUT_CURSOR);
            }
        }
    }

    /* Status bar */
    {
        int sy = win->y + (int)win->h - STATUSBAR_H;
        char sbuf[48];
        char num[8];
        UINT32 ni = 0;

        gui_fill_rect(win->x, sy, win->w, (UINT32)STATUSBAR_H, C_WIN_STATUSBAR);
        gui_fill_rect(win->x, sy, win->w, 1, C_BORDER);

        /* "Ln X  Col Y" */
        sbuf[0]='L'; sbuf[1]='n'; sbuf[2]=' '; ni=3;
        gui_uint_to_str(s_cursor_line + 1, num, 8);
        { UINT32 j=0; while(num[j]) sbuf[ni++]=num[j++]; }
        sbuf[ni++]=' '; sbuf[ni++]=' '; sbuf[ni++]='C'; sbuf[ni++]='o'; sbuf[ni++]='l'; sbuf[ni++]=' ';
        gui_uint_to_str(s_cursor_col + 1, num, 8);
        { UINT32 j=0; while(num[j]) sbuf[ni++]=num[j++]; }
        sbuf[ni++]=' '; sbuf[ni++]=' ';
        if (!s_dirty_flag) { sbuf[ni++]='S'; sbuf[ni++]='a'; sbuf[ni++]='v'; sbuf[ni++]='e'; sbuf[ni++]='d'; }
        else               { sbuf[ni++]='M'; sbuf[ni++]='o'; sbuf[ni++]='d'; sbuf[ni++]='i'; sbuf[ni++]='f'; sbuf[ni++]='i'; sbuf[ni++]='e'; sbuf[ni++]='d'; }
        sbuf[ni] = '\0';

        gui_draw_text(win->x + 8, sy + (STATUSBAR_H - FONT_H) / 2, sbuf, C_TEXT_MUTED);
        gui_draw_text_right(win->x, sy + (STATUSBAR_H - FONT_H) / 2, win->w - 8, "C Source", C_TEXT_LABEL);
    }
}

static void editor_on_key(GUI_WIN *win, UINT8 scancode)
{
    char ch;
    (void)win;

    /* F2 = Save */
    if (scancode == 0x3C) {
        app_editor_handle_click(s_btn_save.x + 1, s_btn_save.y + 1);
        return;
    }
    /* F5 = New */
    if (scancode == 0x3F) {
        app_editor_handle_click(s_btn_new.x + 1, s_btn_new.y + 1);
        return;
    }
    /* Arrow keys */
    if (scancode == 0x48) { /* UP */
        if (s_cursor_line > 0) {
            s_cursor_line--;
            if (s_cursor_line < s_scroll) s_scroll = s_cursor_line;
        }
        return;
    }
    if (scancode == 0x50) { /* DOWN */
        if (s_cursor_line + 1 < s_line_count) {
            s_cursor_line++;
            UINT32 vis = ((UINT32)(win->h - CHROME_H - TOOLBAR_H - STATUSBAR_H)) / CELL_H;
            if (s_cursor_line >= s_scroll + vis) s_scroll++;
        }
        return;
    }
    if (scancode == 0x4B && s_cursor_col > 0) { s_cursor_col--; return; } /* LEFT */
    if (scancode == 0x4D) { /* RIGHT */
        UINT32 ll = 0;
        while (s_lines[s_cursor_line][ll]) ll++;
        if (s_cursor_col < ll) s_cursor_col++;
        return;
    }
    /* Backspace */
    if (scancode == 0x0E) {
        if (s_cursor_col > 0) {
            char *line = s_lines[s_cursor_line];
            UINT32 li  = s_cursor_col - 1;
            while (line[li]) { line[li] = line[li + 1]; li++; }
            s_cursor_col--;
            s_dirty_flag = 1;
        }
        return;
    }
    /* Enter */
    if (scancode == 0x1C && s_line_count < EDITOR_LINES_MAX) {
        UINT32 nl = s_line_count;
        UINT32 k;
        for (k = nl; k > s_cursor_line + 1; k--) {
            scopy(s_lines[k], s_lines[k - 1], EDITOR_LINE_LEN + 1);
        }
        s_lines[s_cursor_line + 1][0] = '\0';
        s_cursor_line++;
        s_cursor_col = 0;
        s_line_count++;
        s_dirty_flag = 1;
        return;
    }
    /* Printable character */
    if (keyboard_read_character(&ch) && ch >= 0x20 && ch < 0x7F) {
        char *line = s_lines[s_cursor_line];
        UINT32 ll = 0;
        while (line[ll]) ll++;
        if (ll < EDITOR_LINE_LEN) {
            UINT32 k;
            for (k = ll; k > s_cursor_col; k--) line[k] = line[k - 1];
            line[s_cursor_col++] = ch;
            line[ll + 1] = '\0';
            s_dirty_flag = 1;
        }
    }
}

void app_editor_scroll(int delta)
{
    /* delta > 0 = wheel up = scroll text up (increase offset) */
    if (delta > 0) {
        if (s_scroll > 0) s_scroll--;
    } else {
        UINT32 max_s = (s_line_count > 1) ? s_line_count - 1 : 0;
        if (s_scroll < max_s) s_scroll++;
    }
}

void app_editor_update_hover(int mx, int my)
{
    gui_widget_update_hover(&s_btn_new,   mx, my);
    gui_widget_update_hover(&s_btn_save,  mx, my);
    gui_widget_update_hover(&s_scrollbar, mx, my);
}

/* Open a file by path from another app (e.g. Files double-click) */
void app_editor_open_file(const char *path)
{
    /* Copy path to s_filename (last component) */
    UINT32 len = 0, slash = 0, i;
    while (path[len]) len++;
    for (i = 0; i < len; i++) if (path[i] == '/') slash = i + 1;
    scopy(s_filename, path + slash, 32);
    load_file(path);
    s_cursor_line = s_cursor_col = s_scroll = 0;
    gui_wm_show(GUI_WIN_EDITOR);
}

void app_editor_handle_click(int mx, int my)
{
    const GUI_WIN *win = gui_wm_get(GUI_WIN_EDITOR);
    if (!win || win->minimized) return;

    /* New button */
    if (gui_widget_handle_click(&s_btn_new, mx, my)) {
        UINT32 i;
        for (i = 0; i < EDITOR_LINES_MAX; i++) s_lines[i][0] = '\0';
        s_line_count  = 1;
        s_cursor_line = s_cursor_col = s_scroll = 0;
        s_dirty_flag  = 0;
        s_filename[0] = '\0';
        return;
    }

    /* Save button */
    if (gui_widget_handle_click(&s_btn_save, mx, my)) {
        if (s_filename[0]) {
            /* Build full content into a static buffer */
            static char save_buf[EDITOR_LINES_MAX * (EDITOR_LINE_LEN + 2)];
            UINT32 pos = 0, li;
            for (li = 0; li < s_line_count && pos < (UINT32)sizeof(save_buf) - 2; li++) {
                UINT32 ci = 0;
                while (s_lines[li][ci] && pos < (UINT32)sizeof(save_buf) - 2)
                    save_buf[pos++] = s_lines[li][ci++];
                save_buf[pos++] = '\n';
            }
            save_buf[pos] = '\0';
            {
                char full_path[48];
                scopy(full_path, "/", 48);
                {
                    UINT32 k = 1;
                    UINT32 j = 0;
                    while (s_filename[j] && k < 47) full_path[k++] = s_filename[j++];
                    full_path[k] = '\0';
                }
                vfs_write_file(full_path, save_buf, (UINT64)pos);
            }
            s_dirty_flag = 0;
        }
        return;
    }

    /* Scrollbar click — jump to clicked position */
    {
        int sb_x = win->x + (int)win->w - 12;
        int sb_y = win->y + CHROME_H + TOOLBAR_H;
        int sb_h = (int)win->h - CHROME_H - TOOLBAR_H - STATUSBAR_H;
        if (mx >= sb_x && mx < sb_x + 12 && my >= sb_y && my < sb_y + sb_h && sb_h > 0) {
            UINT32 vis   = (UINT32)(sb_h / CELL_H);
            UINT32 max_s = (s_line_count > vis) ? s_line_count - vis : 0;
            s_scroll = (UINT32)((my - sb_y) * (int)(max_s + 1) / sb_h);
            if (s_scroll > max_s) s_scroll = max_s;
            return;
        }
    }
}

void app_editor_initialize(void)
{
    UINT32 i;
    for (i = 0; i < EDITOR_LINES_MAX; i++) s_lines[i][0] = '\0';
    s_line_count   = 1;
    s_cursor_line  = s_cursor_col = s_scroll = 0;
    s_dirty_flag   = 0;
    s_filename[0]  = '\0';

    /* Load HELLO.EXE source as default, fall back to empty */
    scopy(s_filename, "main.c", 32);
    load_file("/main.c");
    if (s_line_count <= 1 && s_lines[0][0] == '\0') {
        scopy(s_lines[0], "/* ASAS OS Text Editor */", EDITOR_LINE_LEN + 1);
        scopy(s_lines[1], "/* type your code here */", EDITOR_LINE_LEN + 1);
        s_line_count = 2;
    }

    /* Create toolbar widgets */
    gui_widget_button(&s_btn_new,  0, 0, 30, 18, "New");
    gui_widget_button(&s_btn_save, 0, 0, 34, 18, "Save");
    s_btn_new.style.bg  = s_btn_save.style.bg  = 0xFF2A3850;
    s_btn_new.style.border = s_btn_save.style.border = C_BORDER;
    s_btn_new.style.fg  = s_btn_save.style.fg  = C_TEXT_MUTED;
    s_btn_new.style.corner_r = s_btn_save.style.corner_r = 2;

    /* Scrollbar (geometry is updated each frame in editor_paint) */
    gui_widget_scrollbar(&s_scrollbar, 0, 0, 100, 0, 0, 24);

    gui_wm_set_callbacks(GUI_WIN_EDITOR, editor_paint, editor_on_key);
}
