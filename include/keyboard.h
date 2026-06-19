#ifndef ASAS_KEYBOARD_H
#define ASAS_KEYBOARD_H

#include "uefi.h"

int keyboard_initialize(void);
int keyboard_has_data(void);
UINT8 keyboard_read_scancode(void);
int keyboard_read_character(char *character);
void keyboard_poll_controller(void);
void keyboard_interrupt_handler(void);
UINT64 keyboard_interrupt_count(void);
void keyboard_inject_character(char character);
void keyboard_inject_scancode(UINT8 scancode);

#endif
