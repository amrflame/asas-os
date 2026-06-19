#ifndef ASAS_GUI_NOTIFY_H
#define ASAS_GUI_NOTIFY_H

/*
 * gui_notify.h — Desktop toast notification system
 *
 * Queues up to GUI_NOTIFY_MAX short-lived toast messages that slide in
 * from the bottom-right corner and auto-dismiss after a timeout.
 * Call gui_notify_tick() once per compositor render frame.
 * Call gui_notify_paint() after the desktop/windows are painted.
 *
 * Usage:
 *   gui_notify_initialize();
 *   gui_notify_push("File saved");           // info (default)
 *   gui_notify_push_level("Disk full", 2);   // 0=info,1=warn,2=error
 *   // In render loop:
 *   gui_notify_tick();
 *   gui_notify_paint();
 */

#include "uefi.h"

#define GUI_NOTIFY_MAX       4   /* max simultaneous toasts  */
#define GUI_NOTIFY_W       220   /* toast width (pixels)     */
#define GUI_NOTIFY_H        38   /* toast height             */
#define GUI_NOTIFY_PAD       8   /* gap between toasts       */
#define GUI_NOTIFY_DURATION 180  /* ticks (~1.8 s at 100 Hz) */
#define GUI_NOTIFY_SLIDE     12  /* slide-in ticks           */

/* Severity levels */
#define GUI_NOTIFY_INFO  0
#define GUI_NOTIFY_WARN  1
#define GUI_NOTIFY_ERROR 2

void gui_notify_initialize(void);

/* Push a new toast.  Long messages are truncated to 40 chars. */
void gui_notify_push(const char *msg);
void gui_notify_push_level(const char *msg, int level);

/* Must be called once per frame (updates timers, handles dismissal). */
void gui_notify_tick(void);

/* Paints active toasts into the framebuffer.
 * Call AFTER all windows have been painted. */
void gui_notify_paint(UINT32 screen_w, UINT32 screen_h);

#endif /* ASAS_GUI_NOTIFY_H */
