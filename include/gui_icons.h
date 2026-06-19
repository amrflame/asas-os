#ifndef ASAS_GUI_ICONS_H
#define ASAS_GUI_ICONS_H

/* ======================================================================
 * gui_icons.h — Hybrid icon renderer
 *
 * Draws a recognisable icon for each GUI_WIN_* application.
 * Desktop icons are drawn at size=DESKTOP_ICON_IMG (48).
 * Start-menu icons are drawn at size=STARTMENU_ICON_IMG (36).
 *
 * All icons are 100% primitive-drawn — no bitmaps, no heap allocs.
 * ====================================================================== */

/* Size constants (kept here so callers need only one include) */
#define DESKTOP_ICON_IMG   48   /* desktop icon square size */
#define STARTMENU_ICON_IMG 36   /* start-menu icon square size */

/* Draw the icon for win_id at pixel position (x, y) with given size.
 * win_id : one of GUI_WIN_* constants from gui_wm.h
 * size   : DESKTOP_ICON_IMG or STARTMENU_ICON_IMG               */
void gui_icon_draw(int win_id, int x, int y, int size);

#endif /* ASAS_GUI_ICONS_H */
