/*
 * gui_app_about.c — About dialog rebuilt with gui_widget toolkit
 */

#include "gui_wm.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_compositor.h"
#include "gui_widget.h"

static GUI_WIDGET s_btn_ok;
static GUI_WIDGET s_btn_github;
static GUI_WIDGET s_sep;

static void about_paint(GUI_WIN *win)
{
    int wbx = win->x;
    int wy  = win->y + CHROME_H;
    int ww  = (int)win->w;
    int wh  = (int)win->h - CHROME_H;
    int cx  = wbx + ww / 2;
    int cy  = wy + 14;
    int boty = win->y + (int)win->h;

    /* Background */
    gui_fill_rect(wbx, wy, (UINT32)ww, (UINT32)wh, C_WIN_BODY);

    /* ---- Computer monitor icon ---- */
    gui_fill_rounded(cx - 34, cy,      68, 48, 0xFF2A3A4A, 5);
    gui_draw_border_rounded(cx - 34, cy, 68, 48, 0xFF3D7DD2, 5);
    gui_fill_rect(cx - 28,  cy + 5,   56, 35, 0xFF0D1117);
    gui_fill_rect(cx - 22,  cy + 12,  36,  2, 0xFF3FB950);
    gui_fill_rect(cx - 22,  cy + 18,  28,  2, 0xFF2D7DD2);
    gui_fill_rect(cx - 22,  cy + 24,  32,  2, 0xFF2D7DD2);
    gui_fill_rect(cx - 3,   cy + 48,   6, 10, 0xFF2A3A4A);
    gui_fill_rounded(cx - 16, cy + 56, 32,  6, 0xFF2A3A4A, 3);
    cy += 72;

    /* ---- Title ---- */
    gui_draw_text_centered(wbx, cy, (UINT32)ww, (UINT32)CELL_H,
                           "ASAS OS", C_TEXT_PRIMARY);
    cy += CELL_H + 2;
    gui_draw_text_centered(wbx, cy, (UINT32)ww, (UINT32)CELL_H,
                           "v1.0 — 2026", C_TEXT_MUTED);
    cy += CELL_H + 12;

    /* ---- Separator ---- */
    s_sep.x = wbx + 20;
    s_sep.y = cy;
    s_sep.w = (UINT32)(ww - 40);
    gui_widget_render(&s_sep);
    cy += 10;

    /* ---- Feature badges row ---- */
    {
        const char *badges[] = { "C", "UEFI", "SMP", "TCP", "GPU" };
        UINT32       colors[] = {
            0xFF1A3557, 0xFF2A1A57, 0xFF1A3A1A, 0xFF3A2A0A, 0xFF1A2A3A
        };
        UINT32       fgs[]    = {
            C_ACCENT, 0xFFAA88FF, C_TEXT_GREEN, C_TEXT_YELLOW, 0xFF88CCFF
        };
        int bx = wbx + 12;
        int k;
        for (k = 0; k < 5; k++) {
            GUI_WIDGET badge;
            gui_widget_badge(&badge, bx, cy, badges[k], colors[k], fgs[k]);
            gui_widget_render(&badge);
            bx += (int)badge.w + 6;
        }
        cy += CELL_H + 10;
    }

    /* ---- Description lines ---- */
    gui_draw_text_centered(wbx, cy, (UINT32)ww, (UINT32)CELL_H,
                           "Built entirely in C (no stdlib)", C_TEXT_MUTED);
    cy += CELL_H + 2;
    gui_draw_text_centered(wbx, cy, (UINT32)ww, (UINT32)CELL_H,
                           "Bare-metal x86_64  UEFI + FAT16", C_TEXT_MUTED);
    cy += CELL_H + 2;
    gui_draw_text_centered(wbx, cy, (UINT32)ww, (UINT32)CELL_H,
                           "4-level paging  SMP  Preemptive", C_TEXT_MUTED);
    cy += CELL_H + 16;

    /* ---- Buttons at bottom ---- */
    s_btn_ok.x     = cx - 36;
    s_btn_ok.y     = boty - 36;
    s_btn_github.x = cx + 4;
    s_btn_github.y = boty - 36;
    gui_widget_render(&s_btn_ok);
    gui_widget_render(&s_btn_github);
    (void)cy;
}

static void about_on_key(GUI_WIN *win, UINT8 scancode)
{
    if (scancode == 0x1C || scancode == 0x01) gui_wm_hide(GUI_WIN_ABOUT);
    (void)win;
}

void app_about_handle_click(int mx, int my)
{
    const GUI_WIN *win = gui_wm_get(GUI_WIN_ABOUT);
    if (!win || win->minimized) return;

    if (gui_widget_handle_click(&s_btn_ok, mx, my)) {
        gui_wm_hide(GUI_WIN_ABOUT);
        return;
    }
    (void)gui_widget_handle_click(&s_btn_github, mx, my);
}

void app_about_update_hover(int mx, int my)
{
    gui_widget_update_hover(&s_btn_ok,     mx, my);
    gui_widget_update_hover(&s_btn_github, mx, my);
}

void app_about_initialize(void)
{
    gui_widget_button(&s_btn_ok,     0, 0, 32, 24, "OK");
    s_btn_ok.style.bg      = C_ACCENT;
    s_btn_ok.style.bg_hover = 0xFF3D8AE0;
    s_btn_ok.style.bg_press = C_ACCENT_DIM;
    s_btn_ok.style.border   = 0;
    s_btn_ok.style.fg       = C_ACCENT_TEXT;

    gui_widget_button_flat(&s_btn_github, 0, 0, 56, 24, "Details");

    gui_widget_separator(&s_sep, 0, 0, 0);

    gui_wm_set_callbacks(GUI_WIN_ABOUT, about_paint, about_on_key);
}


