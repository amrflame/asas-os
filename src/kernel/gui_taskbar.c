/*
 * gui_taskbar.c — Bottom taskbar: OS button, running-app buttons, clock
 */

#include "gui_taskbar.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_compositor.h"
#include "gui_wm.h"
#include "gui_startmenu.h"

#define BTN_W   90
#define BTN_H   24
#define BTN_Y_PAD  4
#define OS_BTN_W   52

static int taskbar_y(void)
{
    return (int)gui_compositor_height() - TASKBAR_H;
}

void gui_taskbar_render(void)
{
    UINT32 sw  = gui_compositor_width();
    UINT32 sh  = gui_compositor_height();
    int    ty  = taskbar_y();
    UINT32 i;
    int    bx;
    char   clk[10];

    /* Background */
    gui_fill_rect(0, ty, sw, (UINT32)TASKBAR_H, C_TASKBAR);
    /* Top separator */
    gui_fill_rect(0, ty, sw, 1, C_TASKBAR_SEP);

    /* OS / Start button */
    {
        UINT32 os_bg = gui_startmenu_visible() ? C_TASKBAR_OS_BTN_H : C_TASKBAR_OS_BTN;
        gui_fill_rounded(4, ty + BTN_Y_PAD, OS_BTN_W, (UINT32)BTN_H, os_bg, 3);
        gui_draw_text_centered(4, ty + BTN_Y_PAD, OS_BTN_W, (UINT32)BTN_H,
                               "ASAS", C_ACCENT_TEXT);
    }

    /* Running-app buttons */
    bx = 4 + OS_BTN_W + 8;
    for (i = 0; i < GUI_WIN_COUNT; i++) {
        const GUI_WIN *w = gui_wm_get(i);
        if (!w) continue;

        {
            int      visible  = gui_wm_visible(i);
            int      focused  = (gui_wm_focused() == (int)i);
            UINT32   bg       = focused  ? C_TASKBAR_BTN_ACT : C_TASKBAR_BTN;
            UINT32   fg       = visible  ? C_TEXT_PRIMARY     : C_TEXT_MUTED;

            gui_fill_rounded(bx, ty + BTN_Y_PAD, (UINT32)BTN_W, (UINT32)BTN_H, bg, 3);
            gui_draw_text_n(bx + 6, ty + BTN_Y_PAD + (BTN_H - FONT_H) / 2,
                            w->title, 10, fg);

            /* Running indicator dot */
            if (visible) {
                gui_fill_rounded(bx + BTN_W / 2 - 2,
                                 ty + TASKBAR_H - 5,
                                 4, 4, C_TASKBAR_DOT, 2);
            }
            bx += BTN_W + 4;
        }
    }

    /* Clock (right side) */
    gui_format_time(gui_compositor_loop_ticks(), clk, 10);
    gui_draw_text_right(0, ty + (TASKBAR_H - FONT_H) / 2, sw - 8, clk, C_TEXT_MUTED);

    (void)sh;
}

int gui_taskbar_hit(int mx, int my)
{
    int ty = taskbar_y();
    return my >= ty && my < ty + TASKBAR_H
        && mx >= 0 && mx < (int)gui_compositor_width();
}

void gui_taskbar_handle_click(int mx, int my)
{
    int    ty  = taskbar_y();
    UINT32 i;
    int    bx;

    if (!gui_taskbar_hit(mx, my)) return;

    /* OS button */
    if (mx >= 4 && mx < 4 + OS_BTN_W && my >= ty + BTN_Y_PAD && my < ty + BTN_Y_PAD + BTN_H) {
        gui_startmenu_toggle();
        return;
    }

    /* App buttons */
    bx = 4 + OS_BTN_W + 8;
    for (i = 0; i < GUI_WIN_COUNT; i++) {
        if (mx >= bx && mx < bx + BTN_W && my >= ty + BTN_Y_PAD && my < ty + BTN_Y_PAD + BTN_H) {
            gui_wm_toggle(i);
            gui_startmenu_close();
            return;
        }
        bx += BTN_W + 4;
    }
}
