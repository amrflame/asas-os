#ifndef ASAS_LOGGER_H
#define ASAS_LOGGER_H

#include "uefi.h"

void logger_initialize(void);
void logger_write(const char *level, const char *message);
void logger_write_hex(const char *level, const char *message, UINT64 value);

/* Optional callback invoked on every logger_write call.
   Registered by the GUI subsystem to display SHELL output in the terminal. */
typedef void (*LOGGER_GUI_CALLBACK)(const char *level, const char *message);
void logger_set_gui_callback(LOGGER_GUI_CALLBACK callback);

/* Secondary capture callback — used by shell pipe/redirect to capture output. */
typedef void (*LOGGER_CAPTURE_CALLBACK)(const char *level, const char *message);
void logger_set_capture_callback(LOGGER_CAPTURE_CALLBACK callback);

#endif
