/*
 * gui_widget.c — Modern widget renderer for ASAS Desktop Environment
 *
 * All widgets rendered using gui_draw.h primitives (no GPU dependency).
 * Colours sourced from gui_theme.h for consistent visual style.
 */

#include "gui_widget.h"
#include "gui_draw.h"
#include "gui_theme.h"

/* ======================================================================
 * Default style helpers
 * ====================================================================== */
static GUI_WIDGET_STYLE style_button(void)
{
    GUI_WIDGET_STYLE s;
    s.bg       = C_TASKBAR_BTN;
    s.bg_hover = C_TASKBAR_BTN_ACT;
    s.bg_press = C_ACCENT_DIM;
    s.fg       = C_TEXT_PRIMARY;
    s.fg_muted = C_TEXT_MUTED;
    s.border   = C_WIN_BORDER;
    s.accent   = C_ACCENT;
    s.corner_r = 3;
    return s;
}

static GUI_WIDGET_STYLE style_button_flat(void)
{
    GUI_WIDGET_STYLE s;
    s.bg       = 0;
    s.bg_hover = C_SIDEBAR_ITEM;
    s.bg_press = C_SIDEBAR_ACTIVE;
    s.fg       = C_TEXT_PRIMARY;
    s.fg_muted = C_TEXT_MUTED;
    s.border   = 0;
    s.accent   = C_ACCENT;
    s.corner_r = 2;
    return s;
}

static GUI_WIDGET_STYLE style_button_danger(void)
{
    GUI_WIDGET_STYLE s;
    s.bg       = 0xFF3A1515;
    s.bg_hover = 0xFF5A2020;
    s.bg_press = 0xFF7A2828;
    s.fg       = C_TEXT_PRIMARY;
    s.fg_muted = C_TEXT_MUTED;
    s.border   = C_BTN_CLOSE;
    s.accent   = C_BTN_CLOSE;
    s.corner_r = 3;
    return s;
}

static GUI_WIDGET_STYLE style_input(void)
{
    GUI_WIDGET_STYLE s;
    s.bg       = C_INPUT_BG;
    s.bg_hover = C_INPUT_BG;
    s.bg_press = C_INPUT_BG;
    s.fg       = C_INPUT_TEXT;
    s.fg_muted = C_TEXT_MUTED;
    s.border   = C_INPUT_BORDER;
    s.accent   = C_ACCENT;
    s.corner_r = 2;
    return s;
}

static GUI_WIDGET_STYLE style_panel(void)
{
    GUI_WIDGET_STYLE s;
    s.bg       = C_WIN_BODY;
    s.bg_hover = C_WIN_BODY;
    s.bg_press = C_WIN_BODY;
    s.fg       = C_TEXT_PRIMARY;
    s.fg_muted = C_TEXT_MUTED;
    s.border   = C_WIN_BORDER;
    s.accent   = C_ACCENT;
    s.corner_r = 3;
    return s;
}

static void widget_zero(GUI_WIDGET *w)
{
    UINT32 i;
    UINT8 *b = (UINT8 *)w;
    for (i = 0; i < (UINT32)sizeof(GUI_WIDGET); i++) b[i] = 0;
    w->enabled = 1;
}

/* ======================================================================
 * Constructors
 * ====================================================================== */
void gui_widget_button(GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h,
                        const char *text)
{
    widget_zero(w);
    w->type  = GUI_WIDGET_BUTTON;
    w->x = x; w->y = y; w->w = ww; w->h = h;
    w->text  = text;
    w->style = style_button();
}

void gui_widget_button_flat(GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h,
                              const char *text)
{
    widget_zero(w);
    w->type  = GUI_WIDGET_BUTTON;
    w->x = x; w->y = y; w->w = ww; w->h = h;
    w->text  = text;
    w->style = style_button_flat();
}

void gui_widget_button_danger(GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h,
                                const char *text)
{
    widget_zero(w);
    w->type  = GUI_WIDGET_BUTTON;
    w->x = x; w->y = y; w->w = ww; w->h = h;
    w->text  = text;
    w->style = style_button_danger();
}

void gui_widget_label(GUI_WIDGET *w, int x, int y, const char *text, UINT32 variant)
{
    widget_zero(w);
    w->type  = GUI_WIDGET_LABEL;
    w->x = x; w->y = y;
    w->w = 0; w->h = (UINT32)CELL_H;
    w->text  = text;
    w->value = variant;
    w->style.fg       = C_TEXT_PRIMARY;
    w->style.fg_muted = C_TEXT_MUTED;
    w->style.accent   = C_ACCENT;
}

void gui_widget_input(GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h,
                       const char *placeholder)
{
    widget_zero(w);
    w->type        = GUI_WIDGET_INPUT;
    w->x = x; w->y = y; w->w = ww; w->h = h;
    w->placeholder = placeholder;
    w->style       = style_input();
}

void gui_widget_progress(GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h,
                          UINT32 value)
{
    widget_zero(w);
    w->type  = GUI_WIDGET_PROGRESS;
    w->x = x; w->y = y; w->w = ww; w->h = h;
    w->value = value > 100u ? 100u : value;
    w->style.bg     = C_WIN_BODY;
    w->style.border = C_WIN_BORDER;
    w->style.accent = C_ACCENT;
    w->style.corner_r = 2;
}

void gui_widget_separator(GUI_WIDGET *w, int x, int y, UINT32 ww)
{
    widget_zero(w);
    w->type  = GUI_WIDGET_SEPARATOR;
    w->x = x; w->y = y; w->w = ww; w->h = 1;
    w->style.border = C_DIVIDER;
}

void gui_widget_panel(GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h)
{
    widget_zero(w);
    w->type  = GUI_WIDGET_PANEL;
    w->x = x; w->y = y; w->w = ww; w->h = h;
    w->style = style_panel();
}

void gui_widget_toggle(GUI_WIDGET *w, int x, int y, UINT32 value)
{
    widget_zero(w);
    w->type  = GUI_WIDGET_TOGGLE;
    w->x = x; w->y = y; w->w = 38; w->h = 20;
    w->value = value ? 1u : 0u;
    w->style.bg       = C_WIN_BODY;
    w->style.bg_hover = C_SIDEBAR_ITEM;
    w->style.border   = C_WIN_BORDER;
    w->style.accent   = C_ACCENT;
    w->style.corner_r = 10;
}

void gui_widget_badge(GUI_WIDGET *w, int x, int y, const char *text,
                       UINT32 bg, UINT32 fg)
{
    widget_zero(w);
    w->type  = GUI_WIDGET_BADGE;
    w->x = x; w->y = y;
    w->text  = text;
    w->style.bg  = bg;
    w->style.fg  = fg;
    w->style.corner_r = 3;
    /* Auto-size: text_width + padding */
    {
        UINT32 tw = gui_text_width(text);
        w->w = tw + 8u;
        w->h = (UINT32)CELL_H + 2u;
    }
}

/* ======================================================================
 * Rendering — each widget type
 * ====================================================================== */

static void render_button(const GUI_WIDGET *w)
{
    UINT32 bg;
    UINT32 fg = w->enabled ? w->style.fg : w->style.fg_muted;

    if (!w->enabled) {
        bg = w->style.bg;
    } else if (w->pressed) {
        bg = w->style.bg_press;
    } else if (w->hovered) {
        bg = w->style.bg_hover;
    } else {
        bg = w->style.bg;
    }

    /* Background */
    if (w->style.corner_r > 0) {
        gui_fill_rounded(w->x, w->y, w->w, w->h, bg, w->style.corner_r);
    } else {
        gui_fill_rect(w->x, w->y, w->w, w->h, bg);
    }

    /* Border: focused = accent, otherwise normal */
    if (w->style.border) {
        UINT32 bd = (w->focused && w->enabled) ? w->style.accent : w->style.border;
        if (w->style.corner_r > 0) {
            gui_draw_border_rounded(w->x, w->y, w->w, w->h, bd, w->style.corner_r);
        } else {
            gui_draw_border(w->x, w->y, w->w, w->h, bd);
        }
    }

    /* Label — centered */
    if (w->text) {
        gui_draw_text_centered(w->x, w->y, w->w, w->h, w->text, fg);
    }
}

static void render_label(const GUI_WIDGET *w)
{
    UINT32 col;
    if (!w->text) return;
    switch (w->value) {
    case GUI_LABEL_MUTED:   col = C_TEXT_MUTED;   break;
    case GUI_LABEL_ACCENT:  col = C_ACCENT;        break;
    case GUI_LABEL_GREEN:   col = C_TEXT_GREEN;    break;
    case GUI_LABEL_YELLOW:  col = C_TEXT_YELLOW;   break;
    case GUI_LABEL_RED:     col = C_TEXT_RED;      break;
    case GUI_LABEL_HEADING:
        /* Draw heading: accent text + separator below */
        gui_draw_text(w->x, w->y, w->text, C_TEXT_PRIMARY);
        if (w->w > 0) {
            gui_fill_rect(w->x, w->y + CELL_H + 2, w->w, 1, C_ACCENT_DIM);
        }
        return;
    default:                col = C_TEXT_PRIMARY;  break;
    }
    gui_draw_text(w->x, w->y, w->text, col);
}

static void render_input(const GUI_WIDGET *w)
{
    UINT32 border_col = w->focused ? w->style.accent : w->style.border;
    int text_x = w->x + 5;
    int text_y = w->y + ((int)w->h - FONT_H) / 2;

    /* Background */
    if (w->style.corner_r > 0) {
        gui_fill_rounded(w->x, w->y, w->w, w->h, w->style.bg, w->style.corner_r);
        gui_draw_border_rounded(w->x, w->y, w->w, w->h, border_col, w->style.corner_r);
    } else {
        gui_fill_rect(w->x, w->y, w->w, w->h, w->style.bg);
        gui_draw_border(w->x, w->y, w->w, w->h, border_col);
    }

    if (w->input_len > 0) {
        /* Clip rendered text to widget width */
        UINT32 max_chars = (w->w > 10u) ? (w->w - 10u) / (UINT32)CELL_W : 0u;
        UINT32 start = 0;
        if (w->input_len > max_chars && max_chars > 0) {
            start = w->input_len - max_chars;
        }
        gui_draw_text_n(text_x, text_y,
                        w->input_buf + start,
                        w->input_len - start,
                        w->style.fg);
        /* Cursor */
        if (w->focused) {
            UINT32 shown = (w->input_len - start > max_chars)
                           ? max_chars : w->input_len - start;
            int cx = text_x + (int)(shown * (UINT32)CELL_W);
            gui_fill_rect(cx, text_y, 1, (UINT32)FONT_H, w->style.accent);
        }
    } else {
        /* Placeholder */
        if (w->placeholder) {
            gui_draw_text(text_x, text_y, w->placeholder, w->style.fg_muted);
        }
        if (w->focused) {
            gui_fill_rect(text_x, text_y, 1, (UINT32)FONT_H, w->style.accent);
        }
    }
}

static void render_progress(const GUI_WIDGET *w)
{
    UINT32 fill_w;

    /* Track */
    if (w->style.corner_r > 0) {
        gui_fill_rounded(w->x, w->y, w->w, w->h, w->style.bg, w->style.corner_r);
        gui_draw_border_rounded(w->x, w->y, w->w, w->h, w->style.border, w->style.corner_r);
    } else {
        gui_fill_rect(w->x, w->y, w->w, w->h, w->style.bg);
        gui_draw_border(w->x, w->y, w->w, w->h, w->style.border);
    }

    /* Fill */
    if (w->value > 0 && w->w > 2) {
        fill_w = (w->w - 2u) * w->value / 100u;
        if (fill_w > 0) {
            if (w->style.corner_r > 0) {
                gui_fill_rounded(w->x + 1, w->y + 1,
                                 fill_w, w->h > 2u ? w->h - 2u : 1u,
                                 w->style.accent, w->style.corner_r);
            } else {
                gui_fill_rect(w->x + 1, w->y + 1,
                              fill_w, w->h > 2u ? w->h - 2u : 1u,
                              w->style.accent);
            }
        }
    }

    /* Optional percentage text centered */
    if (w->h >= (UINT32)(FONT_H + 2)) {
        char pct[8];
        UINT32 v = w->value;
        UINT32 i = 0;
        if (v == 100) {
            pct[i++] = '1'; pct[i++] = '0'; pct[i++] = '0';
        } else {
            if (v >= 10) pct[i++] = '0' + (char)(v / 10);
            pct[i++] = '0' + (char)(v % 10);
        }
        pct[i++] = '%'; pct[i] = '\0';
        gui_draw_text_centered(w->x, w->y, w->w, w->h, pct, C_TEXT_PRIMARY);
    }
}

static void render_toggle(const GUI_WIDGET *w)
{
    /* Pill outer */
    UINT32 bg = (w->value) ? w->style.accent : w->style.bg;
    UINT32 bd = w->hovered ? w->style.accent : w->style.border;
    int knob_x;

    gui_fill_rounded(w->x, w->y, w->w, w->h, bg, w->style.corner_r);
    gui_draw_border_rounded(w->x, w->y, w->w, w->h, bd, w->style.corner_r);

    /* Sliding knob — white circle */
    if (w->value) {
        knob_x = w->x + (int)w->w - (int)w->h + 2;
    } else {
        knob_x = w->x + 2;
    }
    {
        int ky = w->y + 2;
        UINT32 ks = w->h > 4u ? w->h - 4u : 1u;
        gui_fill_rounded(knob_x, ky, ks, ks, C_TEXT_PRIMARY, ks / 2u);
    }
}

static void render_badge(const GUI_WIDGET *w)
{
    UINT32 tw;
    int tx, ty;

    gui_fill_rounded(w->x, w->y, w->w, w->h, w->style.bg, w->style.corner_r);

    if (!w->text) return;
    tw = gui_text_width(w->text);
    tx = w->x + ((int)w->w - (int)tw) / 2;
    ty = w->y + ((int)w->h - FONT_H) / 2;
    gui_draw_text(tx, ty, w->text, w->style.fg);
}

/* ---- Dispatch ---- */
void gui_widget_render(const GUI_WIDGET *w)
{
    switch (w->type) {
    case GUI_WIDGET_BUTTON:
    case GUI_WIDGET_BADGE:    /* badge uses button path */
        if (w->type == GUI_WIDGET_BADGE) render_badge(w);
        else                             render_button(w);
        break;
    case GUI_WIDGET_LABEL:    render_label(w);    break;
    case GUI_WIDGET_INPUT:    render_input(w);    break;
    case GUI_WIDGET_PROGRESS: render_progress(w); break;
    case GUI_WIDGET_SEPARATOR:
        gui_fill_rect(w->x, w->y, w->w, 1, w->style.border);
        break;
    case GUI_WIDGET_PANEL:
        if (w->style.corner_r > 0) {
            gui_fill_rounded(w->x, w->y, w->w, w->h, w->style.bg, w->style.corner_r);
            if (w->style.border)
                gui_draw_border_rounded(w->x, w->y, w->w, w->h,
                                        w->style.border, w->style.corner_r);
        } else {
            gui_fill_rect(w->x, w->y, w->w, w->h, w->style.bg);
            if (w->style.border)
                gui_draw_border(w->x, w->y, w->w, w->h, w->style.border);
        }
        break;
    case GUI_WIDGET_TOGGLE:   render_toggle(w);   break;
    case GUI_WIDGET_SCROLLBAR:
        /* Track */
        gui_fill_rounded(w->x, w->y, w->w, w->h, 0xFF1A2436, 3);
        /* Thumb */
        {
            UINT32 max_v  = w->value >> 16;   /* high 16 bits = max_value   */
            UINT32 page_v = w->value & 0xFFFFu; /* low  16 bits = page_size */
            UINT32 cur    = w->input_len;       /* reuse input_len for cur   */
            UINT32 total  = max_v + page_v;
            UINT32 th, ty;
            if (total == 0) total = 1;
            th = (UINT32)w->h * page_v / total;
            if (th < 12) th = 12;
            if (th > w->h) th = w->h;
            ty = (max_v > 0) ? (((UINT32)w->h - th) * cur / max_v) : 0u;
            gui_fill_rounded(w->x + 2, w->y + (int)ty,
                             w->w > 4u ? w->w - 4u : 1u, th,
                             w->hovered ? C_ACCENT : 0xFF3A5070, 3);
        }
        break;
    default: break;
    }
}

/* ======================================================================
 * Input handling
 * ====================================================================== */
static int in_bounds(const GUI_WIDGET *w, int mx, int my)
{
    return mx >= w->x && mx < w->x + (int)w->w
        && my >= w->y && my < w->y + (int)w->h;
}

int gui_widget_update_hover(GUI_WIDGET *w, int mx, int my)
{
    int was = w->hovered;
    w->hovered = w->enabled && in_bounds(w, mx, my) ? 1 : 0;
    return w->hovered != was;
}

int gui_widget_handle_click(GUI_WIDGET *w, int mx, int my)
{
    if (!w->enabled || !in_bounds(w, mx, my)) return 0;

    w->pressed = 1;
    w->focused = 1;

    /* Toggle flips its value on click */
    if (w->type == GUI_WIDGET_TOGGLE) {
        w->value = w->value ? 0u : 1u;
        if (w->on_change) w->on_change(w->user_data, w->value);
        return 1;
    }

    if (w->on_click) w->on_click(w->user_data);
    return 1;
}

void gui_widget_handle_release(GUI_WIDGET *w)
{
    w->pressed = 0;
}

int gui_widget_handle_key(GUI_WIDGET *w, char ch)
{
    if (w->type != GUI_WIDGET_INPUT || !w->focused) return 0;

    if (ch == '\b' || ch == 0x7F) {
        /* Backspace */
        if (w->input_len > 0) {
            w->input_len--;
            w->input_buf[w->input_len] = '\0';
            if (w->on_change) w->on_change(w->user_data, w->input_len);
        }
        return 1;
    }

    if (ch == '\r' || ch == '\n') {
        /* Enter — fire on_click as confirm */
        if (w->on_click) w->on_click(w->user_data);
        return 1;
    }

    if (ch >= 0x20 && ch <= 0x7E && w->input_len < 127) {
        w->input_buf[w->input_len++] = ch;
        w->input_buf[w->input_len]   = '\0';
        if (w->on_change) w->on_change(w->user_data, w->input_len);
        return 1;
    }
    return 0;
}

/* ======================================================================
 * Scrollbar constructor + drag
 * ====================================================================== */
void gui_widget_scrollbar(GUI_WIDGET *w, int x, int y, UINT32 h,
                           UINT32 value, UINT32 max_value, UINT32 page_size)
{
    widget_zero(w);
    w->type      = GUI_WIDGET_SCROLLBAR;
    w->x = x; w->y = y;
    w->w = 10; w->h = h;
    /* pack max_value (high 16) + page_size (low 16) into value field */
    if (max_value  > 0xFFFFu) max_value  = 0xFFFFu;
    if (page_size  > 0xFFFFu) page_size  = 0xFFFFu;
    w->value     = (max_value << 16) | (page_size & 0xFFFFu);
    /* current position stored in input_len (reusing the field) */
    w->input_len = value > max_value ? max_value : value;
    w->enabled   = (max_value > 0) ? 1 : 0;
}

/* Update scroll position from a mouse drag.
 * my_start = y at mouse-down; my_current = current y.
 * Returns new scroll value (0..max_value). */
int gui_widget_scrollbar_drag(GUI_WIDGET *w, int my_start, int my_current)
{
    UINT32 max_v  = w->value >> 16;
    UINT32 page_v = w->value & 0xFFFFu;
    UINT32 total  = max_v + page_v;
    int    dy     = my_current - my_start;
    int    track  = (int)w->h;
    int    new_val;

    if (!w->enabled || total == 0 || track == 0) return (int)w->input_len;

    /* Convert pixel delta to value delta */
    new_val = (int)w->input_len + dy * (int)total / track;
    if (new_val < 0)           new_val = 0;
    if ((UINT32)new_val > max_v) new_val = (int)max_v;
    if (w->input_len != (UINT32)new_val) {
        w->input_len = (UINT32)new_val;
        return new_val;
    }
    return -1; /* no change */
}

/* ======================================================================
 * Batch helpers
 * ====================================================================== */
void gui_widgets_render(GUI_WIDGET *widgets, UINT32 count)
{
    UINT32 i;
    for (i = 0; i < count; i++) gui_widget_render(&widgets[i]);
}

int gui_widgets_update_hover(GUI_WIDGET *widgets, UINT32 count, int mx, int my)
{
    int hover = -1;
    UINT32 i;
    for (i = 0; i < count; i++) {
        if (gui_widget_update_hover(&widgets[i], mx, my)) {
            if (widgets[i].hovered) hover = (int)i;
        }
    }
    return hover;
}

int gui_widgets_handle_click(GUI_WIDGET *widgets, UINT32 count, int mx, int my)
{
    UINT32 i;
    for (i = 0; i < count; i++) {
        if (gui_widget_handle_click(&widgets[i], mx, my)) return (int)i;
    }
    return -1;
}
