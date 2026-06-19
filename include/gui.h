#ifndef ASAS_GUI_H
#define ASAS_GUI_H

#include "framebuffer.h"
#include "uefi.h"

/* ======================================================================
 * gui.h — Unified GUI entry-point header
 *
 * Dispatches to either the Safe Mode GUI (safemode_gui) or
 * the modern ASAS Desktop Environment depending on the mode
 * set via gui_set_mode() before gui_initialize().
 * ====================================================================== */

/* Call BEFORE gui_initialize().
 * safe_mode = 1 → legacy safe mode GUI
 * safe_mode = 0 → modern ASAS Desktop Environment (default)  */
void gui_set_mode(int safe_mode);
int  gui_get_mode(void);  /* returns current safe_mode flag */

/* Initialise the active GUI. Must be called once before gui_thread_entry(). */
void gui_initialize(ASAS_FRAMEBUFFER *framebuffer);

/* Thread entry — pass to scheduler_create_thread(). Loops forever. */
void gui_thread_entry(void);

/* Write a line to the terminal output buffer of the active GUI.
   Called automatically by the logger hook; also callable from shell. */
void gui_terminal_write(const char *text);

/* Update the shell input line displayed in the active GUI's terminal. */
void gui_set_input_line(const char *line, UINT32 length);

/* Returns a monotonically increasing tick counter driven by the GUI loop. */
UINT32 gui_loop_ticks(void);

/* Legacy single-shot render kept for backward compatibility. */
void gui_render_desktop(ASAS_FRAMEBUFFER *framebuffer);

#endif /* ASAS_GUI_H */
