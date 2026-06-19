#ifndef ASAS_GUI_STARTMENU_H
#define ASAS_GUI_STARTMENU_H

#include "uefi.h"

void gui_startmenu_initialize(void);
void gui_startmenu_toggle    (void);
void gui_startmenu_close     (void);
int  gui_startmenu_visible   (void);
void gui_startmenu_render    (void);
void gui_startmenu_handle_click(int mx, int my);
void gui_startmenu_handle_key  (UINT8 scancode);

#endif
