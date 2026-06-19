#ifndef ASAS_GUI_THEME_H
#define ASAS_GUI_THEME_H

/* ======================================================================
 * gui_theme.h — ASAS Desktop Environment colour palette & layout constants
 * ====================================================================== */

/* ---- Desktop background gradient (top → bottom) ---- */
#define C_DESKTOP_TOP       0xFF0D1117
#define C_DESKTOP_BOT       0xFF111827

/* ---- Window surfaces ---- */
#define C_WIN_CHROME        0xFF1A2232   /* titlebar */
#define C_WIN_BODY          0xFF141D2B   /* window body */
#define C_WIN_STATUSBAR     0xFF111827   /* status bar */
#define C_WIN_BORDER        0xFF252F3F   /* inactive border */
#define C_WIN_BORDER_FOCUS  0xFF2D6FBF   /* focused border */
#define C_WIN_CHROME_SEP    0xFF2D4A6F   /* accent underline below titlebar */

/* ---- Window control buttons ---- */
#define C_BTN_CLOSE         0xFFE5534B
#define C_BTN_CLOSE_HOVER   0xFFFF6058
#define C_BTN_MIN           0xFFD4A017
#define C_BTN_MIN_HOVER     0xFFFFBD2E
#define C_BTN_MAX           0xFF3A3A4A   /* greyed — not implemented */
#define C_BTN_ICON          0xFFFFFFFF

/* ---- Taskbar ---- */
#define C_TASKBAR           0xFF0A101C
#define C_TASKBAR_SEP       0xFF1E2D40
#define C_TASKBAR_BTN       0xFF1A2537
#define C_TASKBAR_BTN_ACT   0xFF1E3557
#define C_TASKBAR_DOT       0xFF2D7DD2   /* running-app indicator */
#define C_TASKBAR_OS_BTN    0xFF1E3A5F
#define C_TASKBAR_OS_BTN_H  0xFF2D5A8F

/* ---- Desktop icons ---- */
#define C_ICON_SELECTED_BG  0x402D7DD2   /* translucent blue — paint as blend */
#define C_ICON_SELECTED_BD  0xFF2D7DD2

/* ---- Start menu ---- */
#define C_MENU_BG           0xFF111827
#define C_MENU_HDR          0xFF0A101C
#define C_MENU_HDR_SEP      0xFF1E2D40
#define C_MENU_ITEM_HOVER   0xFF1A2A44
#define C_MENU_FOOTER       0xFF0A101C
#define C_MENU_FOOTER_SEP   0xFF1E2D40
#define C_MENU_SEARCH_BG    0xFF1A2337
#define C_MENU_SEARCH_BD    0xFF2D4A6F

/* ---- Text ---- */
#define C_TEXT_PRIMARY      0xFFE2E8F0
#define C_TEXT_MUTED        0xFF8892A4
#define C_TEXT_LABEL        0xFF4A5A72   /* section labels (ALL CAPS) */
#define C_TEXT_ACCENT       0xFF2D7DD2
#define C_TEXT_GREEN        0xFF3FB950
#define C_TEXT_YELLOW       0xFFD4A017
#define C_TEXT_RED          0xFFE5534B

/* ---- Accent ---- */
#define C_ACCENT            0xFF2D7DD2
#define C_ACCENT_DIM        0xFF1A4A7A
#define C_ACCENT_TEXT       0xFFFFFFFF

/* ---- Borders ---- */
#define C_BORDER            0xFF1E2D40
#define C_DIVIDER           0xFF1A2537

/* ---- Sidebar (file manager bookmarks, settings nav) ---- */
#define C_SIDEBAR_BG        0xFF101520
#define C_SIDEBAR_ITEM      0xFF1A2235
#define C_SIDEBAR_ACTIVE    0xFF1E3557
#define C_SIDEBAR_ACTIVE_BD 0xFF2D7DD2

/* ---- Terminal ---- */
#define C_TERM_BG           0xFF0D1117
#define C_TERM_PROMPT       0xFF3FB950   /* green prompt */
#define C_TERM_OUTPUT       0xFFCDD6F4
#define C_TERM_CURSOR       0xFF2D7DD2
#define C_TERM_ERROR        0xFFE5534B
#define C_TERM_INFO         0xFF2D7DD2

/* ---- Input field ---- */
#define C_INPUT_BG          0xFF0D1117
#define C_INPUT_TEXT        0xFFE2E8F0
#define C_INPUT_CURSOR      0xFFE2E8F0
#define C_INPUT_BORDER      0xFF2D4A6F

/* ---- Wallpaper presets (5 options) ---- */
#define WALLPAPER_COUNT     5
/* Each defined as {top, bottom} — see gui_compositor.c for the table */

/* ---- Layout ---- */
#define TASKBAR_H           32
#define CHROME_H            28   /* window titlebar height */
#define CHROME_BTN_W        16   /* width of each control button */
#define CHROME_BTN_H        14
#define CHROME_BTN_PAD       7   /* top padding for buttons */
#define DESKTOP_ICON_X       8   /* left edge of icon column */
#define DESKTOP_ICON_Y0     16   /* y of first icon */
#define DESKTOP_ICON_W      60   /* total cell width  (≥ DESKTOP_ICON_IMG + 2*6) */
#define DESKTOP_ICON_H      76   /* total cell height (48 icon + ~2 lines label) */
/* DESKTOP_ICON_IMG is defined in gui_icons.h as 48 */
#define DESKTOP_ICON_GAP     6   /* gap between icons */
#define STARTMENU_W        220
#define STARTMENU_HDR_H     56
#define STARTMENU_SEARCH_H  28
#define STARTMENU_FOOTER_H  32
#define STARTMENU_ICON_W    50   /* cell width  (\u2265 STARTMENU_ICON_IMG + padding) */
#define STARTMENU_ICON_H    50   /* cell height (36 icon + 14 label) */
#define STARTMENU_ICON_GAP  12
#define STARTMENU_COLS       2

/* ---- Font cell (5x7 bitmap) ---- */
#define FONT_W   5
#define FONT_H   7
#define CELL_W   6   /* glyph + 1 px gap */
#define CELL_H   9   /* glyph + 2 px gap */

#endif /* ASAS_GUI_THEME_H */
