/*
 * gui_app_settings.c — Settings: Appearance / System / About tabs
 * Rebuilt with gui_widget toolkit for hover effects and consistent styling.
 */

#include "gui_wm.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_compositor.h"
#include "gui_widget.h"
#include "virtual_disk.h"
#include "logger.h"

#define SIDEBAR_W   90
#define TAB_H       28

typedef enum { TAB_APPEARANCE = 0, TAB_SYSTEM = 1, TAB_ABOUT = 2 } SETTINGS_TAB;

static SETTINGS_TAB s_tab;
static UINT32       s_wallpaper;

/* ---- Sidebar tab widgets ---- */
static GUI_WIDGET s_tab_btns[3];

/* ---- Appearance tab widgets ---- */
static GUI_WIDGET s_dark_toggle;
static GUI_WIDGET s_wpaper_sep;
static GUI_WIDGET s_display_sep;

/* ---- System tab widgets ---- */
static GUI_WIDGET s_sys_sep;
static GUI_WIDGET s_vdisk_detach_btn;

/* ---- Wallpaper swatches (top, bottom colour pairs) ---- */
static const UINT32 s_swatches[WALLPAPER_COUNT][2] = {
    { 0xFF0D1117, 0xFF111827 },
    { 0xFF0D0F1A, 0xFF1A1040 },
    { 0xFF0A1418, 0xFF0D2A28 },
    { 0xFF111111, 0xFF1E1E1E },
    { 0xFF000000, 0xFF0A0A0A },
};

static const char *s_tab_labels[3] = { "Appearance", "System", "About" };

/* ======================================================================
 * Paint
 * ====================================================================== */
static void settings_paint(GUI_WIN *win)
{
    int wx = win->x, wy = win->y + CHROME_H;
    int ww = (int)win->w, wh = (int)win->h - CHROME_H;
    int cx = wx + SIDEBAR_W + 1;   /* content x */
    int cw = ww - SIDEBAR_W - 1;   /* content width */
    int cy, i;

    /* ---- Sidebar background ---- */
    gui_fill_rect(wx, wy, (UINT32)SIDEBAR_W, (UINT32)wh, C_SIDEBAR_BG);
    gui_fill_rect(wx + SIDEBAR_W, wy, 1, (UINT32)wh, C_BORDER);

    /* ---- Sidebar tab buttons ---- */
    for (i = 0; i < 3; i++) {
        int active = ((int)i == (int)s_tab);
        GUI_WIDGET *b = &s_tab_btns[i];

        b->x = wx + 1;
        b->y = wy + 8 + i * (TAB_H + 4);
        b->w = (UINT32)(SIDEBAR_W - 2);
        b->h = (UINT32)TAB_H;

        /* Active tab: accent left stripe + distinct bg */
        if (active) {
            gui_fill_rect(wx, b->y, 3, (UINT32)TAB_H, C_ACCENT);
            b->style.bg       = C_SIDEBAR_ACTIVE;
            b->style.bg_hover = C_SIDEBAR_ACTIVE;
            b->style.fg       = C_TEXT_PRIMARY;
        } else {
            b->style.bg       = C_SIDEBAR_BG;
            b->style.bg_hover = C_SIDEBAR_ITEM;
            b->style.fg       = C_TEXT_MUTED;
        }
        b->style.border   = 0;
        b->style.corner_r = 0;
        gui_widget_render(b);
    }

    /* ---- Content area ---- */
    gui_fill_rect(cx, wy, (UINT32)cw, (UINT32)wh, C_WIN_BODY);
    cy = wy + 14;

    /* ================================================================
     * TAB: APPEARANCE
     * ============================================================== */
    if (s_tab == TAB_APPEARANCE) {

        /* THEME section label */
        gui_draw_text(cx + 12, cy, "THEME", C_TEXT_LABEL);
        cy += CELL_H + 6;

        /* Dark mode row */
        gui_draw_text(cx + 12, cy + (20 - FONT_H) / 2, "Dark mode", C_TEXT_PRIMARY);
        s_dark_toggle.x = cx + cw - 50;
        s_dark_toggle.y = cy;
        gui_widget_render(&s_dark_toggle);
        cy += 28;

        /* Separator */
        s_wpaper_sep.x = cx + 12;
        s_wpaper_sep.y = cy;
        s_wpaper_sep.w = (UINT32)(cw - 24);
        gui_widget_render(&s_wpaper_sep);
        cy += 10;

        /* WALLPAPER section */
        gui_draw_text(cx + 12, cy, "WALLPAPER", C_TEXT_LABEL);
        cy += CELL_H + 8;

        for (i = 0; i < WALLPAPER_COUNT; i++) {
            int sx = cx + 12 + i * 46;
            gui_fill_gradient_v(sx, cy, 38, 26, s_swatches[i][0], s_swatches[i][1]);
            if ((UINT32)i == s_wallpaper) {
                gui_draw_border_rounded(sx - 2, cy - 2, 42, 30, C_ACCENT, 2);
            } else {
                gui_draw_border_rounded(sx - 1, cy - 1, 40, 28, C_BORDER, 2);
            }
        }
        cy += 40;

        /* Separator */
        s_display_sep.x = cx + 12;
        s_display_sep.y = cy;
        s_display_sep.w = (UINT32)(cw - 24);
        gui_widget_render(&s_display_sep);
        cy += 10;

        /* DISPLAY section */
        gui_draw_text(cx + 12, cy, "DISPLAY", C_TEXT_LABEL);
        cy += CELL_H + 6;

        gui_draw_text(cx + 12, cy, "Resolution", C_TEXT_PRIMARY);
        {
            char res[24];
            UINT32 n = 0;
            gui_uint_to_str(gui_compositor_width(), res, 8);
            while (res[n]) n++;
            res[n++] = ' '; res[n++] = 'x'; res[n++] = ' ';
            gui_uint_to_str(gui_compositor_height(), res + n, 8);
            gui_draw_text_right(cx, cy, (UINT32)cw - 12, res, C_TEXT_MUTED);
        }
        cy += CELL_H + 4;
        gui_draw_text(cx + 12, cy, "Refresh rate", C_TEXT_PRIMARY);
        gui_draw_text_right(cx, cy, (UINT32)cw - 12, "100 Hz", C_TEXT_MUTED);
        cy += CELL_H + 4;
        gui_draw_text(cx + 12, cy, "Pixel format", C_TEXT_PRIMARY);
        gui_draw_text_right(cx, cy, (UINT32)cw - 12, "BGRX8888", C_TEXT_MUTED);

    /* ================================================================
     * TAB: SYSTEM
     * ============================================================== */
    } else if (s_tab == TAB_SYSTEM) {

        gui_draw_text(cx + 12, cy, "SYSTEM INFO", C_TEXT_LABEL);
        cy += CELL_H + 8;

        /* Info rows */
        {
            const char *keys[]   = { "OS", "Kernel", "Architecture", "Build", "Uptime" };
            const char *vals[5];
            char clk[10];
            int k;
            gui_format_time(gui_compositor_loop_ticks(), clk, 10);
            vals[0] = "ASAS OS v1.0";
            vals[1] = "ASAS-KERNEL";
            vals[2] = "x86_64 UEFI";
            vals[3] = "2026";
            vals[4] = clk;

            for (k = 0; k < 5; k++) {
                /* Alternating row shade */
                if (k % 2 == 0) {
                    gui_fill_rect(cx + 8, cy - 2, (UINT32)(cw - 16),
                                  (UINT32)(CELL_H + 4), 0xFF131D2B);
                }
                gui_draw_text(cx + 14, cy, keys[k], C_TEXT_PRIMARY);
                gui_draw_text_right(cx, cy, (UINT32)cw - 14, vals[k], C_TEXT_MUTED);
                cy += CELL_H + 6;
            }
        }

        cy += 4;
        /* Separator */
        s_sys_sep.x = cx + 12;
        s_sys_sep.y = cy;
        s_sys_sep.w = (UINT32)(cw - 24);
        gui_widget_render(&s_sys_sep);
        cy += 10;

        /* Memory progress bar */
        gui_draw_text(cx + 12, cy, "MEMORY", C_TEXT_LABEL);
        cy += CELL_H + 6;
        gui_draw_text(cx + 12, cy, "Heap usage", C_TEXT_PRIMARY);
        cy += CELL_H + 4;
        {
            GUI_WIDGET prog;
            gui_widget_progress(&prog, cx + 12, cy, (UINT32)(cw - 24), 14, 23u);
        gui_widget_render(&prog);

        cy += 26;
        gui_draw_text(cx + 12, cy, "VIRTUAL DISKS", C_TEXT_LABEL);
        cy += CELL_H + 6;
        {
            UINT32 count = virtual_disk_count();
            UINT32 index;
            char count_text[16];
            gui_uint_to_str(count, count_text, sizeof(count_text));
            gui_draw_text(cx + 12, cy, "Attached", C_TEXT_PRIMARY);
            gui_draw_text_right(cx, cy, (UINT32)cw - 14, count_text, C_TEXT_MUTED);
            cy += CELL_H + 4;
            for (index = 0; index < count && index < 3U; index++) {
                const ASAS_VDISK_INFO *info = virtual_disk_get(index);
                if (info == 0) continue;
                gui_draw_text(cx + 12, cy, info->name, C_TEXT_PRIMARY);
                gui_draw_text_right(cx, cy, (UINT32)cw - 14, info->format, C_TEXT_MUTED);
                cy += CELL_H + 4;
            }
            if (count != 0) {
                s_vdisk_detach_btn.x = cx + 12;
                s_vdisk_detach_btn.y = cy + 2;
                s_vdisk_detach_btn.w = 86;
                s_vdisk_detach_btn.h = 20;
                gui_widget_render(&s_vdisk_detach_btn);
            }
        }
        }

    /* ================================================================
     * TAB: ABOUT
     * ============================================================== */
    } else {

        gui_draw_text(cx + 12, cy, "ABOUT ASAS OS", C_TEXT_LABEL);
        cy += CELL_H + 8;

        {
            const char *lines[] = {
                "Version   1.0",
                "Built in  C (no stdlib)",
                "Boot      UEFI + FAT16",
                "Memory    4-level paging",
                "Net       TCP/IP stack",
                "SMP       Preemptive",
                "GPU       VirtIO GPU",
            };
            int k;
            for (k = 0; k < 7; k++) {
                GUI_WIDGET badge;
                /* Green dot badge */
                gui_widget_badge(&badge, cx + 12, cy + 1, ".", 0xFF1A3A1A, C_TEXT_GREEN);
                badge.w = 6; badge.h = (UINT32)CELL_H;
                gui_widget_render(&badge);
                gui_draw_text(cx + 22, cy, lines[k], C_TEXT_PRIMARY);
                cy += CELL_H + 4;
            }
        }
    }
    (void)i;
    (void)cy;
}

/* ======================================================================
 * Key handling
 * ====================================================================== */
static void settings_on_key(GUI_WIN *win, UINT8 scancode)
{
    (void)win;
    if (scancode == 0x0F) {   /* Tab key cycles tabs */
        s_tab = (SETTINGS_TAB)(((int)s_tab + 1) % 3);
    }
}

/* ======================================================================
 * Click handling
 * ====================================================================== */
void app_settings_handle_click(int mx, int my)
{
    const GUI_WIN *win = gui_wm_get(GUI_WIN_SETTINGS);
    int i;
    if (!win || win->minimized) return;

    /* Sidebar tab buttons */
    for (i = 0; i < 3; i++) {
        if (gui_widget_handle_click(&s_tab_btns[i], mx, my)) {
            s_tab = (SETTINGS_TAB)i;
            return;
        }
    }

    /* Dark mode toggle */
    if (gui_widget_handle_click(&s_dark_toggle, mx, my)) return;

    /* Wallpaper swatches (Appearance tab) */
    if (s_tab == TAB_APPEARANCE) {
        int wy  = win->y + CHROME_H;
        int cx  = win->x + SIDEBAR_W + 1;
        int cw  = (int)win->w - SIDEBAR_W - 1;
        int cy  = wy + 14 + CELL_H + 6 + 28 + 10 + CELL_H + 8;
        for (i = 0; i < WALLPAPER_COUNT; i++) {
            int sx = cx + 12 + i * 46;
            if (mx >= sx - 2 && mx < sx + 40 && my >= cy - 2 && my < cy + 30) {
                s_wallpaper = (UINT32)i;
                gui_compositor_set_wallpaper(s_wallpaper);
                return;
            }
        }
        (void)cw;
    }
    if (s_tab == TAB_SYSTEM && virtual_disk_count() != 0 &&
        gui_widget_handle_click(&s_vdisk_detach_btn, mx, my)) {
        const ASAS_VDISK_INFO *info = virtual_disk_get(0);
        if (info != 0 && virtual_disk_detach(info->name)) {
            logger_write("GUI", "virtual disk detached");
        } else {
            logger_write("GUI", "detach failed; unmount volume first");
        }
        return;
    }
}

/* ======================================================================
 * Hover update (call from render loop)
 * ====================================================================== */
void app_settings_update_hover(int mx, int my)
{
    int i;
    for (i = 0; i < 3; i++) gui_widget_update_hover(&s_tab_btns[i], mx, my);
    gui_widget_update_hover(&s_dark_toggle, mx, my);
    gui_widget_update_hover(&s_vdisk_detach_btn, mx, my);
}

/* ======================================================================
 * Initialize
 * ====================================================================== */
void app_settings_initialize(void)
{
    int i;
    s_tab      = TAB_APPEARANCE;
    s_wallpaper = 0;

    /* Sidebar tab buttons — geometry set at paint time */
    for (i = 0; i < 3; i++) {
        gui_widget_button_flat(&s_tab_btns[i], 0, 0, 0, 0, s_tab_labels[i]);
    }

    /* Dark mode toggle */
    gui_widget_toggle(&s_dark_toggle, 0, 0, 1u);

    /* Separators */
    gui_widget_separator(&s_wpaper_sep,  0, 0, 0);
    gui_widget_separator(&s_display_sep, 0, 0, 0);
    gui_widget_separator(&s_sys_sep,     0, 0, 0);
    gui_widget_button_flat(&s_vdisk_detach_btn, 0, 0, 86, 20, "Detach");

    gui_wm_set_callbacks(GUI_WIN_SETTINGS, settings_paint, settings_on_key);
}
