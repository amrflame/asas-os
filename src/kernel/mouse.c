#include "mouse.h"
#include "uefi.h"

#pragma intrinsic(__inbyte)
unsigned char __inbyte(unsigned short port);
#pragma intrinsic(__outbyte)
void __outbyte(unsigned short port, unsigned char value);

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL 0x02

static long long current_x;
static long long current_y;
static UINT8 current_buttons;
static UINT64 report_count;
static UINT32 absolute_x;
static UINT32 absolute_y;
static UINT8 absolute_buttons;
static UINT8 absolute_pending;

static int wait_input_empty(void)
{
    UINT32 timeout;

    for (timeout = 0; timeout < 1000000U; timeout++) {
        if ((__inbyte(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0) {
            return 1;
        }
    }

    return 0;
}

static int wait_output_full(void)
{
    UINT32 timeout;

    for (timeout = 0; timeout < 1000000U; timeout++) {
        if ((__inbyte(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
            return 1;
        }
    }

    return 0;
}

int mouse_initialize(void)
{
    if (!wait_input_empty()) {
        return 0;
    }

    __outbyte(PS2_COMMAND_PORT, 0xA8);
    if (!wait_input_empty()) {
        return 0;
    }

    __outbyte(PS2_COMMAND_PORT, 0xD4);
    if (!wait_input_empty()) {
        return 0;
    }

    __outbyte(PS2_DATA_PORT, 0xF4);
    if (!wait_output_full()) {
        return 0;
    }

    return __inbyte(PS2_DATA_PORT) == 0xFA;
}

void mouse_inject_report(long long delta_x, long long delta_y, UINT8 buttons)
{
    current_x += delta_x;
    current_y += delta_y;
    current_buttons = buttons;
    report_count++;
}

void mouse_inject_absolute(UINT32 x, UINT32 y, UINT8 buttons)
{
    absolute_x = x;
    absolute_y = y;
    absolute_buttons = buttons;
    absolute_pending = 1;
    current_buttons = buttons;
    report_count++;
}

void mouse_consume_delta(long long *delta_x, long long *delta_y, UINT8 *buttons)
{
    if (delta_x != 0) {
        *delta_x = current_x;
    }
    if (delta_y != 0) {
        *delta_y = current_y;
    }
    if (buttons != 0) {
        *buttons = current_buttons;
    }

    current_x = 0;
    current_y = 0;
}

int mouse_consume_absolute(UINT32 *x, UINT32 *y, UINT8 *buttons)
{
    if (!absolute_pending) {
        return 0;
    }
    if (x != 0) {
        *x = absolute_x;
    }
    if (y != 0) {
        *y = absolute_y;
    }
    if (buttons != 0) {
        *buttons = absolute_buttons;
    }
    absolute_pending = 0;
    return 1;
}

long long mouse_x(void)
{
    return current_x;
}

long long mouse_y(void)
{
    return current_y;
}

UINT8 mouse_buttons(void)
{
    return current_buttons;
}

UINT64 mouse_report_count(void)
{
    return report_count;
}

/* ======================================================================
 * Scroll wheel support
 * ====================================================================== */
static int s_scroll_accum = 0;

void mouse_inject_scroll(int delta)
{
    s_scroll_accum += delta;
}

int mouse_consume_scroll(void)
{
    int v = s_scroll_accum;
    s_scroll_accum = 0;
    return v;
}
