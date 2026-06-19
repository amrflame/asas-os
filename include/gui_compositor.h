#ifndef ASAS_GUI_COMPOSITOR_H
#define ASAS_GUI_COMPOSITOR_H

#include "framebuffer.h"
#include "uefi.h"

/* ======================================================================
 * gui_compositor.h — back-buffer manager & input state for ADE
 * ====================================================================== */

void   gui_compositor_initialize (ASAS_FRAMEBUFFER *real_fb);
void   gui_compositor_clear      (UINT32 color);
void   gui_compositor_blit       (void);
void   gui_compositor_lock       (void);
void   gui_compositor_unlock     (void);

/* Update cursor from mouse, return 1 if position or buttons changed. */
int    gui_compositor_update_input(void);

/* Render the mouse cursor arrow on top of the back-buffer. */
void   gui_compositor_render_cursor(void);

/* Accessors */
UINT32 gui_compositor_width  (void);
UINT32 gui_compositor_height (void);
int    gui_compositor_cursor_x(void);
int    gui_compositor_cursor_y(void);
UINT8  gui_compositor_buttons (void);
UINT8  gui_compositor_prev_buttons(void);
void   gui_compositor_consume_buttons(void);  /* commit prev = buttons */
UINT32 gui_compositor_loop_ticks(void);
void   gui_compositor_tick(void);  /* call once per GUI loop iteration */

/* Wallpaper index (0–4). Changing triggers a full redraw. */
void   gui_compositor_set_wallpaper(UINT32 index);
UINT32 gui_compositor_get_wallpaper(void);

/* Fill back-buffer with the current wallpaper gradient. */
void   gui_compositor_draw_wallpaper(void);

#endif /* ASAS_GUI_COMPOSITOR_H */
