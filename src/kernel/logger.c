#include "logger.h"
#include "uefi.h"

#pragma intrinsic(__outbyte)
void __outbyte(unsigned short port, unsigned char data);

#define COM1_PORT 0x3F8

static LOGGER_GUI_CALLBACK     gui_callback;
static LOGGER_CAPTURE_CALLBACK capture_callback;

static void serial_write(const char *text)
{
    while (*text != '\0') {
        __outbyte(COM1_PORT, (UINT8)*text);
        text++;
    }
}

void logger_initialize(void)
{
    __outbyte(COM1_PORT + 1, 0x00);
    __outbyte(COM1_PORT + 3, 0x80);
    __outbyte(COM1_PORT + 0, 0x03);
    __outbyte(COM1_PORT + 1, 0x00);
    __outbyte(COM1_PORT + 3, 0x03);
    __outbyte(COM1_PORT + 2, 0xC7);
    __outbyte(COM1_PORT + 4, 0x0B);
}

void logger_set_gui_callback(LOGGER_GUI_CALLBACK callback)
{
    gui_callback = callback;
}

void logger_set_capture_callback(LOGGER_CAPTURE_CALLBACK callback)
{
    capture_callback = callback;
}

void logger_write(const char *level, const char *message)
{
    serial_write("[Asas][");
    serial_write(level);
    serial_write("] ");
    serial_write(message);
    serial_write("\r\n");
    if (gui_callback != 0) {
        gui_callback(level, message);
    }
    if (capture_callback != 0) {
        capture_callback(level, message);
    }
}

void logger_write_hex(const char *level, const char *message, UINT64 value)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[96];
    UINT32 index;
    UINT32 output_index = 0;

    while (message[output_index] != '\0' && output_index + 22 < sizeof(text)) {
        text[output_index] = message[output_index];
        output_index++;
    }
    text[output_index++] = ':';
    text[output_index++] = ' ';

    text[output_index++] = '0';
    text[output_index++] = 'x';
    for (index = 0; index < 16; index++) {
        text[output_index++] = digits[(value >> ((15 - index) * 4)) & 0xF];
    }
    text[output_index] = '\0';
    logger_write(level, text);
}
