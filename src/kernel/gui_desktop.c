/*
 * gui_desktop.c — Desktop icon grid (left column), dynamic add/remove
 */

#include "gui_desktop.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_wm.h"
#include "gui_icons.h"
#include "gui_compositor.h"

typedef struct {
    char   line1[16];
    char   line2[16];
    UINT32 icon_color;
    UINT32 win_id;
    UINT8  visible;
    UINT8  selected;
} DESKTOP_ICON;

static DESKTOP_ICON s_icons[DESKTOP_ICONS_MAX];
static UINT32       s_icon_count;

/* Last single-click state for double-click detection */
static UINT32 s_last_icon   = 0xFFFFFFFF;
static UINT32 s_last_tick   = 0;
#define DBLCLICK_TICKS  40

static void str_copy_n(char *dst, const char *src, UINT32 max)
{
    UINT32 i = 0;
    while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int gui_desktop_icon_add(const char *line1, const char *line2,
                          UINT32 icon_color, UINT32 win_id)
{
    UINT32 i;
    if (s_icon_count >= DESKTOP_ICONS_MAX) return 0;
    /* Reuse a hidden slot if possible */
    for (i = 0; i < s_icon_count; i++) {
        if (!s_icons[i].visible && s_icons[i].win_id == win_id) {
            s_icons[i].visible = 1;
            return 1;
        }
    }
    i = s_icon_count++;
    str_copy_n(s_icons[i].line1, line1, 16);
    str_copy_n(s_icons[i].line2, line2, 16);
    s_icons[i].icon_color = icon_color;
    s_icons[i].win_id     = win_id;
    s_icons[i].visible    = 1;
    s_icons[i].selected   = 0;
    return 1;
}

void gui_desktop_icon_remove(UINT32 win_id)
{
    UINT32 i;
    for (i = 0; i < s_icon_count; i++) {
        if (s_icons[i].win_id == win_id) { s_icons[i].visible = 0; return; }
    }
}

void gui_desktop_initialize(void)
{
    s_icon_count = 0;
    /* Add default icons matching GUI_WIN_* order */
    gui_desktop_icon_add("Terminal",  "",       0xFF3A3A4A, GUI_WIN_TERMINAL);
    gui_desktop_icon_add("File",      "Manager",0xFFD4A017, GUI_WIN_FILES);
    gui_desktop_icon_add("Text",      "Editor", 0xFF1E6B2A, GUI_WIN_EDITOR);
    gui_desktop_icon_add("Calc-",     "ulator", 0xFF2D7DD2, GUI_WIN_CALC);
    gui_desktop_icon_add("Settings",  "",       0xFF555566, GUI_WIN_SETTINGS);
    gui_desktop_icon_add("About",     "",       0xFFD4A017, GUI_WIN_ABOUT);
    gui_desktop_icon_add("Disk",      "Manager",0xFF2A6F5F, GUI_WIN_DISKMGMT);
}

/* ---- Geometry helpers ---- */
static int icon_x(void) { return DESKTOP_ICON_X; }

static int icon_y(UINT32 slot)
{
    return DESKTOP_ICON_Y0 + (int)(slot * (DESKTOP_ICON_H + DESKTOP_ICON_GAP));
}

void gui_desktop_render(void)
{
    UINT32 slot = 0, i;

    for (i = 0; i < s_icon_count; i++) {
        DESKTOP_ICON *ic = &s_icons[i];
        int ix, iy;

        if (!ic->visible) continue;

        ix = icon_x();
        iy = icon_y(slot++);

        /* Selected highlight */
        if (ic->selected) {
            /* Simulate translucent blue with a dark tinted rect */
            gui_fill_rect(ix - 2, iy - 2, DESKTOP_ICON_W + 4, DESKTOP_ICON_H + 4,
                          0xFF1A3050);
            gui_draw_border_rounded(ix - 2, iy - 2, DESKTOP_ICON_W + 4, DESKTOP_ICON_H + 4,
                                    C_ICON_SELECTED_BD, 3);
        }

        /* Icon — drawn by the hybrid icon renderer (48x48) */
        gui_icon_draw((int)ic->win_id,
                      ix + (DESKTOP_ICON_W - DESKTOP_ICON_IMG) / 2, iy,
                      DESKTOP_ICON_IMG);

        /* Label lines below icon */
        {
            int ty = iy + DESKTOP_ICON_IMG + 3;
            int cx = ix + DESKTOP_ICON_W / 2;
            UINT32 tw = gui_text_width(ic->line1);
            gui_draw_text(cx - (int)tw / 2, ty, ic->line1, C_TEXT_PRIMARY);
            if (ic->line2[0]) {
                ty += CELL_H;
                tw  = gui_text_width(ic->line2);
                gui_draw_text(cx - (int)tw / 2, ty, ic->line2, C_TEXT_PRIMARY);
            }
        }
    }
}

void gui_desktop_handle_click(int mx, int my, int double_click_hint)
{
    UINT32 slot = 0, i;
    UINT32 loop_ticks = gui_compositor_loop_ticks();
    (void)double_click_hint;

    for (i = 0; i < s_icon_count; i++) {
        DESKTOP_ICON *ic = &s_icons[i];
        if (!ic->visible) continue;

        {
            int ix = icon_x();
            int iy = icon_y(slot);
            slot++;

            if (mx >= ix - 2 && mx < ix + DESKTOP_ICON_W + 2
             && my >= iy - 2 && my < iy + DESKTOP_ICON_H + 2) {
                UINT32 j;
                for (j = 0; j < s_icon_count; j++) s_icons[j].selected = 0;
                ic->selected = 1;

                /* Double-click detection via loop_ticks counter */
                if (s_last_icon == i && loop_ticks - s_last_tick < DBLCLICK_TICKS) {
                    gui_wm_show(ic->win_id);
                    s_last_icon = 0xFFFFFFFF;
                } else {
                    s_last_icon = i;
                    s_last_tick = loop_ticks;
                }
                return;
            }
        }
    }

    /* Click on empty desktop — deselect all */
    for (i = 0; i < s_icon_count; i++) s_icons[i].selected = 0;
}
