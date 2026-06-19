#ifndef ASAS_CONSOLE_H
#define ASAS_CONSOLE_H

#include "framebuffer.h"

typedef struct {
    ASAS_FRAMEBUFFER *framebuffer;
    UINT32 cursor_x;
    UINT32 cursor_y;
    UINT32 foreground;
    UINT32 background;
    UINT32 scale;
} ASAS_CONSOLE;

void console_initialize(ASAS_CONSOLE *console, ASAS_FRAMEBUFFER *framebuffer);
void console_clear(ASAS_CONSOLE *console);
void console_write(ASAS_CONSOLE *console, const char *text);

#endif

