#include "crash.h"
#include "logger.h"
#include "panic.h"

#pragma intrinsic(__halt)
void __halt(void);

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *panic_console;

static void panic_print_ascii(const char *text)
{
    CHAR16 buffer[128];

    if (panic_console == 0) {
        return;
    }

    while (*text != '\0') {
        UINTN length = 0;

        while (text[length] != '\0' && length < 127) {
            buffer[length] = (CHAR16)(UINT8)text[length];
            length++;
        }

        buffer[length] = 0;
        panic_console->output_string(panic_console, buffer);
        text += length;
    }
}

void panic_set_uefi_console(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console)
{
    panic_console = console;
}

void panic(const char *message)
{
    crash_record("panic", message, 0x50414E4943ULL);
    logger_write("PANIC", message);
    if (panic_console != 0) {
        panic_console->set_attribute(panic_console, 0x0C);
        panic_print_ascii("\r\nASAS KERNEL PANIC: ");
        panic_print_ascii(message);
        panic_print_ascii("\r\n");
    }

    for (;;) {
        __halt();
    }
}
