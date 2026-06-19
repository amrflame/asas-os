#ifndef ASAS_MOUSE_H
#define ASAS_MOUSE_H

#include "uefi.h"

int mouse_initialize(void);
void mouse_inject_report(long long delta_x, long long delta_y, UINT8 buttons);
void mouse_inject_absolute(UINT32 x, UINT32 y, UINT8 buttons);
void mouse_consume_delta(long long *delta_x, long long *delta_y, UINT8 *buttons);
int mouse_consume_absolute(UINT32 *x, UINT32 *y, UINT8 *buttons);
long long mouse_x(void);
long long mouse_y(void);
UINT8 mouse_buttons(void);
UINT64 mouse_report_count(void);

/* Scroll wheel: inject positive=up/negative=down, consume clears after read */
void mouse_inject_scroll(int delta);
int  mouse_consume_scroll(void);

#endif
