/*
 * gui_startmenu.c — Start Menu (C/OS–style: header, search, app grid, footer)
 */

#include "gui_startmenu.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_compositor.h"
#include "gui_wm.h"
#include "gui_icons.h"
#include "keyboard.h"
#include "power.h"

#define MENU_X_PAD   4     /* from left edge of taskbar button */
#define MENU_COLS    STARTMENU_COLS

typedef struct {
    const char *title;
    UINT32      icon_color;
    UINT32      win_id;
} MENU_APP;

static const MENU_APP s_apps[GUI_WIN_COUNT] = {
    { "Terminal",     0xFF3A3A4A, GUI_WIN_TERMINAL },
    { "File Manager", 0xFFD4A017, GUI_WIN_FILES    },
    { "Text Editor",  0xFF1E6B2A, GUI_WIN_EDITOR   },
    { "Calculator",   0xFF2D7DD2, GUI_WIN_CALC     },
    { "Settings",     0xFF555566, GUI_WIN_SETTINGS },
    { "About",        0xFFD4A017, GUI_WIN_ABOUT    },
    { "Disk Manager", 0xFF2A6F5F, GUI_WIN_DISKMGMT },
};

static UINT8  s_visible;
static char   s_search[32];
static UINT32 s_search_len;
static UINT32 s_hovered;

/* ---- String helpers ---- */
static UINT8 str_match(const char *name, const char *q)
{
    UINT32 qi = 0, ni = 0;
    if (!q[0]) return 1;
    while (q[qi] && name[ni]) {
        char qc = q[qi] >= 'A' && q[qi] <= 'Z' ? q[qi]+32 : q[qi];
        char nc = name[ni] >= 'A' && name[ni] <= 'Z' ? name[ni]+32 : name[ni];
        if (qc != nc) return 0;
        qi++; ni++;
    }
    return q[qi] == '\0';
}

void gui_startmenu_initialize(void)
{
    s_visible    = 0;
    s_search[0]  = '\0';
    s_search_len = 0;
    s_hovered    = 0xFFFF;
}

void gui_startmenu_toggle(void) { s_visible = !s_visible; s_search[0]='\0'; s_search_len=0; }
void gui_startmenu_close (void) { s_visible = 0; }
int  gui_startmenu_visible(void) { return s_visible; }

void gui_startmenu_render(void)
{
    UINT32 sh  = gui_compositor_height();
    int    mx  = MENU_X_PAD;           /* left edge */
    int    my  = (int)sh - TASKBAR_H - 4; /* bottom edge = just above taskbar */
    int    row, col;
    UINT32 apps_shown = 0;
    UINT32 i;

    if (!s_visible) return;

    /* Count visible apps for height calculation */
    for (i = 0; i < GUI_WIN_COUNT; i++) {
        if (str_match(s_apps[i].title, s_search)) apps_shown++;
    }
    {
        UINT32 grid_rows = (apps_shown + MENU_COLS - 1) / MENU_COLS;
        UINT32 menu_h    = (UINT32)STARTMENU_HDR_H
                         + (UINT32)STARTMENU_SEARCH_H + 8
                         + grid_rows * ((UINT32)STARTMENU_ICON_H + (UINT32)STARTMENU_ICON_GAP) + 8
                         + (UINT32)STARTMENU_FOOTER_H;
        my -= (int)menu_h;
        if (my < 0) my = 0;

        /* Background */
        gui_fill_rounded(mx, my, (UINT32)STARTMENU_W, menu_h, C_MENU_BG, 4);
        gui_draw_border_rounded(mx, my, (UINT32)STARTMENU_W, menu_h, C_BORDER, 4);

        /* Header */
        gui_fill_rect(mx, my, (UINT32)STARTMENU_W, (UINT32)STARTMENU_HDR_H, C_MENU_HDR);
        gui_fill_rounded(mx + 8, my + 10, 32, 32, C_ACCENT_DIM, 4);
        gui_draw_text(mx + 10, my + 18, "OS", C_ACCENT);
        gui_draw_text(mx + 48, my + 10, "ASAS OS", C_TEXT_PRIMARY);
        gui_draw_text(mx + 48, my + 22, "v1.0 - Built in C", C_TEXT_MUTED);
        gui_fill_rect(mx, my + STARTMENU_HDR_H - 1, (UINT32)STARTMENU_W, 1, C_MENU_HDR_SEP);

        /* Search box */
        {
            int sy = my + STARTMENU_HDR_H + 6;
            gui_fill_rounded(mx + 6, sy, (UINT32)STARTMENU_W - 12,
                              (UINT32)STARTMENU_SEARCH_H, C_MENU_SEARCH_BG, 3);
            gui_draw_border_rounded(mx + 6, sy, (UINT32)STARTMENU_W - 12,
                                    (UINT32)STARTMENU_SEARCH_H, C_MENU_SEARCH_BD, 3);
            if (s_search_len > 0) {
                gui_draw_text(mx + 12, sy + (STARTMENU_SEARCH_H - FONT_H) / 2,
                              s_search, C_INPUT_TEXT);
            } else {
                gui_draw_text(mx + 12, sy + (STARTMENU_SEARCH_H - FONT_H) / 2,
                              "Search apps...", C_TEXT_MUTED);
            }
        }

        /* App grid */
        {
            int gy = my + STARTMENU_HDR_H + STARTMENU_SEARCH_H + 16;
            UINT32 slot = 0;
            for (i = 0; i < GUI_WIN_COUNT; i++) {
                if (!str_match(s_apps[i].title, s_search)) continue;
                col = (int)(slot % MENU_COLS);
                row = (int)(slot / MENU_COLS);
                slot++;

                {
                    int ix = mx + 8 + col * (STARTMENU_ICON_W + STARTMENU_ICON_GAP + 8);
                    int iy = gy + row * (STARTMENU_ICON_H + STARTMENU_ICON_GAP);

                    /* Hover highlight */
                    if (s_hovered == i) {
                        gui_fill_rounded(ix - 4, iy - 4,
                                         (UINT32)STARTMENU_ICON_W + 8,
                                         (UINT32)STARTMENU_ICON_H + CELL_H + 8,
                                         C_MENU_ITEM_HOVER, 3);
                    }

                    /* Icon — hybrid renderer at 36x36 */
                    gui_icon_draw((int)s_apps[i].win_id,
                                  ix + (STARTMENU_ICON_W - STARTMENU_ICON_IMG) / 2, iy,
                                  STARTMENU_ICON_IMG);

                    /* App name below icon */
                    {
                        UINT32 tw = gui_text_width(s_apps[i].title);
                        int    tx = ix + ((STARTMENU_ICON_W - (int)tw) / 2);
                        gui_draw_text(tx, iy + STARTMENU_ICON_IMG + 3, s_apps[i].title, C_TEXT_PRIMARY);
                    }
                }
            }
        }

        /* Footer */
        {
            int fy = my + (int)menu_h - STARTMENU_FOOTER_H;
            gui_fill_rect(mx, fy, (UINT32)STARTMENU_W, 1, C_MENU_FOOTER_SEP);
            gui_fill_rect(mx, fy + 1, (UINT32)STARTMENU_W, (UINT32)STARTMENU_FOOTER_H - 1, C_MENU_FOOTER);
            gui_draw_text(mx + 10, fy + (STARTMENU_FOOTER_H - FONT_H) / 2,
                          "Settings", C_TEXT_MUTED);
            gui_draw_text_right(mx, fy + (STARTMENU_FOOTER_H - FONT_H) / 2,
                                (UINT32)STARTMENU_W - 10, "Power", C_TEXT_MUTED);
        }
    }
}

void gui_startmenu_handle_click(int mx, int my)
{
    UINT32 sh   = gui_compositor_height();
    int    smx  = MENU_X_PAD;
    UINT32 apps_shown = 0;
    UINT32 i;

    if (!s_visible) return;

    for (i = 0; i < GUI_WIN_COUNT; i++) {
        if (str_match(s_apps[i].title, s_search)) apps_shown++;
    }
    {
        UINT32 grid_rows = (apps_shown + MENU_COLS - 1) / MENU_COLS;
        UINT32 menu_h = (UINT32)STARTMENU_HDR_H + (UINT32)STARTMENU_SEARCH_H + 8
                      + grid_rows * ((UINT32)STARTMENU_ICON_H + (UINT32)STARTMENU_ICON_GAP) + 8
                      + (UINT32)STARTMENU_FOOTER_H;
        int smy = (int)sh - TASKBAR_H - 4 - (int)menu_h;
        if (smy < 0) smy = 0;

        /* Click outside menu → close */
        if (mx < smx || mx > smx + STARTMENU_W || my < smy || my > smy + (int)menu_h) {
            gui_startmenu_close();
            return;
        }

        /* App grid */
        {
            int    gy   = smy + STARTMENU_HDR_H + STARTMENU_SEARCH_H + 16;
            UINT32 slot = 0;
            for (i = 0; i < GUI_WIN_COUNT; i++) {
                int    col, row;
                if (!str_match(s_apps[i].title, s_search)) continue;
                col = (int)(slot % MENU_COLS);
                row = (int)(slot / MENU_COLS);
                slot++;
                {
                    int ix = smx + 8 + col * (STARTMENU_ICON_W + STARTMENU_ICON_GAP + 8);
                    int iy = gy  + row * (STARTMENU_ICON_H + STARTMENU_ICON_GAP);
                    if (mx >= ix - 4 && mx < ix + STARTMENU_ICON_W + 4
                     && my >= iy - 4 && my < iy + STARTMENU_ICON_H + CELL_H + 4) {
                        gui_wm_show(s_apps[i].win_id);
                        gui_startmenu_close();
                        return;
                    }
                }
            }
        }

        /* Footer: Settings */
        {
            int fy = smy + (int)menu_h - STARTMENU_FOOTER_H;
            if (my >= fy && mx >= smx + 10 && mx < smx + 70) {
                gui_wm_show(GUI_WIN_SETTINGS);
                gui_startmenu_close();
                return;
            }
            /* Footer: Power (right side) */
            if (my >= fy && mx >= smx + STARTMENU_W - 60 && mx < smx + STARTMENU_W - 5) {
                power_shutdown();
                return;
            }
        }
    }
}

void gui_startmenu_handle_key(UINT8 scancode)
{
    if (!s_visible) return;
    /* ESC closes menu */
    if (scancode == 0x01) { gui_startmenu_close(); return; }
    /* Printable ASCII (scan → char via simple table) */
    if (scancode >= 0x10 && scancode <= 0x19) {
        static const char row1[] = "qwertyuiop";
        char c = row1[scancode - 0x10];
        if (s_search_len < 31) { s_search[s_search_len++] = c; s_search[s_search_len] = '\0'; }
    } else if (scancode >= 0x1E && scancode <= 0x26) {
        static const char row2[] = "asdfghjkl";
        char c = row2[scancode - 0x1E];
        if (s_search_len < 31) { s_search[s_search_len++] = c; s_search[s_search_len] = '\0'; }
    } else if (scancode >= 0x2C && scancode <= 0x32) {
        static const char row3[] = "zxcvbnm";
        char c = row3[scancode - 0x2C];
        if (s_search_len < 31) { s_search[s_search_len++] = c; s_search[s_search_len] = '\0'; }
    } else if (scancode == 0x0E && s_search_len > 0) { /* Backspace */
        s_search[--s_search_len] = '\0';
    }
}
