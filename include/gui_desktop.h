#ifndef ASAS_GUI_DESKTOP_H
#define ASAS_GUI_DESKTOP_H

#include "uefi.h"

#define DESKTOP_ICONS_MAX  16

/* Add a desktop icon. Returns 0 on failure (table full). */
int  gui_desktop_icon_add   (const char *line1, const char *line2,
                              UINT32 icon_color, UINT32 win_id);
void gui_desktop_icon_remove(UINT32 win_id);
void gui_desktop_initialize (void);
void gui_desktop_render     (void);
void gui_desktop_handle_click(int mx, int my, int double_click);

#endif
