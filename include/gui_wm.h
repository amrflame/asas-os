#ifndef ASAS_GUI_WM_H
#define ASAS_GUI_WM_H

#include "uefi.h"

/* ======================================================================
 * gui_wm.h — Window Manager for ASAS Desktop Environment
 * ====================================================================== */

/* Window IDs — must match order in gui_wm.c initialisation table */
#define GUI_WIN_TERMINAL   0
#define GUI_WIN_FILES      1
#define GUI_WIN_EDITOR     2
#define GUI_WIN_CALC       3
#define GUI_WIN_SETTINGS   4
#define GUI_WIN_ABOUT      5
#define GUI_WIN_DISKMGMT   6
#define GUI_WIN_COUNT      7

typedef struct GUI_WIN {
    UINT32  id;
    char    title[32];
    int     x, y;
    UINT32  w, h;
    UINT32  icon_color;      /* colour for desktop icon square */
    UINT8   minimized;
    UINT8   dragging;
    UINT8   resizing;        /* bottom-right corner drag active */
    UINT8   maximized;       /* window fills desktop area       */
    int     drag_ox, drag_oy;
    int     resize_ox, resize_oy;
    /* saved geometry for restore from maximise */
    int     restore_x, restore_y;
    UINT32  restore_w, restore_h;

    void (*on_paint)(struct GUI_WIN *win);
    void (*on_key)  (struct GUI_WIN *win, UINT8 scancode);
} GUI_WIN;

/* Initialise all windows at default positions. */
void gui_wm_initialize(void);

/* Register app callbacks after app_*_initialize(). */
void gui_wm_set_callbacks(UINT32 id,
                           void (*on_paint)(GUI_WIN *),
                           void (*on_key  )(GUI_WIN *, UINT8));

/* Show / hide a window. */
void gui_wm_show(UINT32 id);
void gui_wm_hide(UINT32 id);
void gui_wm_toggle(UINT32 id);

/* Returns non-zero if window is currently visible. */
int  gui_wm_visible(UINT32 id);

/* Returns the focused window id, or -1. */
int  gui_wm_focused(void);

/* Access window struct directly (read-only). */
const GUI_WIN *gui_wm_get(UINT32 id);

/* Render all visible windows (back-to-front). */
void gui_wm_render_all(void);

/* Handle a mouse event. mx,my = cursor position;
   pressed/released derived from button transition. */
void gui_wm_handle_mouse(int mx, int my, UINT8 buttons, UINT8 prev);

/* Forward key scancode to the focused window. */
void gui_wm_handle_key(UINT8 scancode);

#endif /* ASAS_GUI_WM_H */
