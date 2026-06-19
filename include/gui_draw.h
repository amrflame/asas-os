#ifndef ASAS_GUI_DRAW_H
#define ASAS_GUI_DRAW_H

#include "uefi.h"

/* ======================================================================
 * gui_draw.h — 2-D drawing API for ASAS Desktop Environment
 *
 * All coordinates are absolute screen pixels relative to the top-left
 * corner of the back-buffer managed by gui_compositor.
 * ====================================================================== */

/* Initialise: store framebuffer pointer used by all draw calls. */
void gui_draw_set_target(UINT32 *buf, UINT32 width, UINT32 height, UINT32 stride);

/* ---- Primitives ---- */
void gui_fill_rect   (int x, int y, UINT32 w, UINT32 h, UINT32 color);
void gui_draw_border (int x, int y, UINT32 w, UINT32 h, UINT32 color);
void gui_put_pixel   (int x, int y, UINT32 color);

/* Vertical gradient from color_top to color_bot across h rows. */
void gui_fill_gradient_v(int x, int y, UINT32 w, UINT32 h,
                          UINT32 color_top, UINT32 color_bot);

/* Rounded rect: corner_r in {1,2,3,4}. */
void gui_fill_rounded (int x, int y, UINT32 w, UINT32 h,
                        UINT32 color, UINT32 corner_r);
void gui_draw_border_rounded(int x, int y, UINT32 w, UINT32 h,
                              UINT32 color, UINT32 corner_r);

/* ---- Text ---- */
void   gui_draw_char  (int x, int y, char ch, UINT32 color);
void   gui_draw_text  (int x, int y, const char *text, UINT32 color);
/* Draw at most 'max_len' characters. */
void   gui_draw_text_n(int x, int y, const char *text, UINT32 max_len, UINT32 color);
/* Return pixel width of null-terminated string. */
UINT32 gui_text_width (const char *text);

/* Horizontally centred text within a rect. */
void gui_draw_text_centered(int rx, int ry, UINT32 rw, UINT32 rh,
                             const char *text, UINT32 color);

/* Right-aligned text at right edge rx+rw. */
void gui_draw_text_right(int rx, int ry, UINT32 rw, const char *text, UINT32 color);

/* ---- Integer-to-string helpers (no stdlib) ---- */
void gui_uint_to_str (UINT32 v, char *buf, UINT32 buf_sz);
void gui_uint_to_hex (UINT32 v, char *buf, UINT32 buf_sz);
void gui_format_time (UINT32 ticks_100hz, char *buf, UINT32 buf_sz);

#endif /* ASAS_GUI_DRAW_H */
