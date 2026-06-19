#ifndef ASAS_GUI_TASKBAR_H
#define ASAS_GUI_TASKBAR_H

#include "uefi.h"

void gui_taskbar_render      (void);
void gui_taskbar_handle_click(int mx, int my);

/* Returns 1 if (mx,my) is within the taskbar area. */
int  gui_taskbar_hit         (int mx, int my);

#endif
