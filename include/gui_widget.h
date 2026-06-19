#ifndef ASAS_GUI_WIDGET_H
#define ASAS_GUI_WIDGET_H

/*
 * gui_widget.h — Modern widget toolkit for ASAS Desktop Environment
 *
 * Provides a reusable, styled set of UI primitives built on top of gui_draw.h.
 * Each widget is a plain struct — no heap allocation required.
 * Apps define arrays of GUI_WIDGET on the stack or statically, then call
 * gui_widget_render() and gui_widget_handle_click() in their paint/input loops.
 *
 * Widget types:
 *   GUI_WIDGET_BUTTON    — rounded rect + centered label + hover/press states
 *   GUI_WIDGET_LABEL     — plain text line (primary or muted color)
 *   GUI_WIDGET_INPUT     — editable text field with cursor
 *   GUI_WIDGET_PROGRESS  — horizontal progress bar (0-100)
 *   GUI_WIDGET_SEPARATOR — thin horizontal divider line
 *   GUI_WIDGET_PANEL     — filled background rect (container/card)
 *   GUI_WIDGET_TOGGLE    — on/off pill switch
 *   GUI_WIDGET_BADGE     — small colored label (status indicator)
 */

#include "uefi.h"

/* ---- Widget type constants ---- */
#define GUI_WIDGET_BUTTON    1u
#define GUI_WIDGET_LABEL     2u
#define GUI_WIDGET_INPUT     3u
#define GUI_WIDGET_PROGRESS  4u
#define GUI_WIDGET_SEPARATOR 5u
#define GUI_WIDGET_PANEL     6u
#define GUI_WIDGET_TOGGLE    7u
#define GUI_WIDGET_BADGE     8u
#define GUI_WIDGET_SCROLLBAR 9u  /* vertical or horizontal scrollbar */

/* ---- Label variants (stored in value field) ---- */
#define GUI_LABEL_PRIMARY  0u  /* C_TEXT_PRIMARY */
#define GUI_LABEL_MUTED    1u  /* C_TEXT_MUTED   */
#define GUI_LABEL_ACCENT   2u  /* C_ACCENT       */
#define GUI_LABEL_GREEN    3u  /* C_TEXT_GREEN   */
#define GUI_LABEL_YELLOW   4u  /* C_TEXT_YELLOW  */
#define GUI_LABEL_RED      5u  /* C_TEXT_RED     */
#define GUI_LABEL_HEADING  6u  /* C_TEXT_PRIMARY, drawn above a separator */

/* ---- Widget style (colours + corner radius) ---- */
typedef struct {
    UINT32 bg;          /* normal background               */
    UINT32 bg_hover;    /* hover background                */
    UINT32 bg_press;    /* pressed background              */
    UINT32 fg;          /* text / icon colour              */
    UINT32 fg_muted;    /* secondary text colour           */
    UINT32 border;      /* border colour (0 = no border)   */
    UINT32 accent;      /* fill / highlight colour         */
    UINT32 corner_r;    /* border radius 0-4               */
} GUI_WIDGET_STYLE;

/* ---- Widget descriptor ---- */
typedef struct {
    UINT32 type;            /* GUI_WIDGET_* constant         */

    /* Geometry */
    int    x, y;
    UINT32 w, h;

    /* Text */
    const char *text;           /* button caption / label text   */
    const char *placeholder;    /* INPUT: shown when buf is empty */

    /* Editable text (INPUT widget) */
    char   input_buf[128];
    UINT32 input_len;

    /* Numeric value: PROGRESS = 0-100; TOGGLE = 0/1; LABEL = GUI_LABEL_* */
    UINT32 value;

    /* Appearance */
    GUI_WIDGET_STYLE style;

    /* Runtime state */
    int hovered;
    int pressed;
    int focused;
    int enabled;   /* 0 = greyed out, no interaction */

    /* Event callbacks */
    void (*on_click) (void *user_data);
    void (*on_change)(void *user_data, UINT32 new_value);
    void *user_data;
} GUI_WIDGET;

/* ======================================================================
 * Convenience constructors — fill a GUI_WIDGET with sensible defaults
 * ====================================================================== */

/* Primary action button (accent border on focus) */
void gui_widget_button   (GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h,
                           const char *text);
/* Flat secondary button (no strong background) */
void gui_widget_button_flat(GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h,
                             const char *text);
/* Danger/destructive button (red tint) */
void gui_widget_button_danger(GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h,
                               const char *text);

/* Plain text label. variant = GUI_LABEL_* */
void gui_widget_label    (GUI_WIDGET *w, int x, int y, const char *text,
                           UINT32 variant);

/* Editable text field */
void gui_widget_input    (GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h,
                           const char *placeholder);

/* Horizontal progress bar. value 0-100. */
void gui_widget_progress (GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h,
                           UINT32 value);

/* Thin horizontal separator line */
void gui_widget_separator(GUI_WIDGET *w, int x, int y, UINT32 ww);

/* Filled background panel / card */
void gui_widget_panel    (GUI_WIDGET *w, int x, int y, UINT32 ww, UINT32 h);

/* On/off toggle switch (width 38, height 20) */
void gui_widget_toggle   (GUI_WIDGET *w, int x, int y, UINT32 value);

/* Small coloured badge label */
void gui_widget_badge    (GUI_WIDGET *w, int x, int y, const char *text,
                           UINT32 bg, UINT32 fg);

/*
 * Vertical scrollbar.
 *   value     = current scroll position (0 .. max_value inclusive)
 *   max_value = total scrollable range; 0 = scrollbar is disabled
 *   page_size = how many units fit on screen (used for thumb sizing)
 * Use value field to read back after interaction.
 */
void gui_widget_scrollbar(GUI_WIDGET *w, int x, int y, UINT32 h,
                           UINT32 value, UINT32 max_value, UINT32 page_size);

/* Handle a mouse drag on a scrollbar widget. Call on every mouse-move while
 * button is held. Returns 1 if value changed (so caller can re-render). */
int gui_widget_scrollbar_drag(GUI_WIDGET *w, int my_start, int my_current);

/* ======================================================================
 * Rendering
 * ====================================================================== */

/* Draw a single widget. Uses gui_draw_set_target's current buffer. */
void gui_widget_render(const GUI_WIDGET *w);

/* ======================================================================
 * Input handling
 * ====================================================================== */

/* Update w->hovered based on mouse position. Returns 1 if changed. */
int gui_widget_update_hover(GUI_WIDGET *w, int mx, int my);

/* Fire w->on_click if (mx,my) is inside w. Returns 1 if consumed. */
int gui_widget_handle_click(GUI_WIDGET *w, int mx, int my);

/* Clear pressed/focused state on mouse release. */
void gui_widget_handle_release(GUI_WIDGET *w);

/* Handle printable character or backspace (0x08) for INPUT widget.
 * Returns 1 if the widget consumed the key. */
int gui_widget_handle_key(GUI_WIDGET *w, char ch);

/* ======================================================================
 * Batch helpers (for arrays of widgets)
 * ====================================================================== */

/* Render all widgets in array. */
void gui_widgets_render(GUI_WIDGET *widgets, UINT32 count);

/* Update hover for all; returns index of newly hovered widget, -1 = none. */
int gui_widgets_update_hover(GUI_WIDGET *widgets, UINT32 count, int mx, int my);

/* Handle click across array; returns index of clicked widget, -1 = none. */
int gui_widgets_handle_click(GUI_WIDGET *widgets, UINT32 count, int mx, int my);

#endif /* ASAS_GUI_WIDGET_H */
