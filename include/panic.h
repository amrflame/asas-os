#ifndef ASAS_PANIC_H
#define ASAS_PANIC_H

#include "uefi.h"

void panic_set_uefi_console(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console);
void panic(const char *message);

#endif
