#ifndef ASAS_SAFEMODE_GUI_H
#define ASAS_SAFEMODE_GUI_H

#include "framebuffer.h"
#include "uefi.h"

void safemode_gui_initialize(ASAS_FRAMEBUFFER *framebuffer);
void safemode_gui_thread_entry(void);
void safemode_gui_terminal_write(const char *text);
void safemode_gui_set_input_line(const char *line, UINT32 length);
UINT32 safemode_gui_loop_ticks(void);
void safemode_gui_render_desktop(ASAS_FRAMEBUFFER *framebuffer);

#endif
