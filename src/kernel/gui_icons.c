/*
 * gui_icons.c — Hybrid icon renderer
 *
 * Desktop icons  : 48x48  (pass size = DESKTOP_ICON_IMG)
 * Start-menu icons : 36x36  (pass size = STARTMENU_ICON_IMG)
 *
 * All icons are drawn with gui_draw primitives only — zero heap,
 * zero file I/O, no bitmaps.  A single S() macro scales every
 * coordinate from a 48-px reference canvas to the actual size.
 */

#include "gui_icons.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_wm.h"

/* Scale a value designed for a 48-px canvas to `size` pixels */
#define S(v) ((v) * size / 48)

/* ======================================================================
 * Terminal  — monitor with screen + neck + base + green prompt
 * ====================================================================== */
static void icon_terminal(int x, int y, int size)
{
    /* Monitor bezel */
    gui_fill_rounded(x + S(2), y + S(2), (UINT32)(size - S(4)), (UINT32)(size - S(18)), 0xFF2A3A4A, S(4));
    gui_draw_border_rounded(x + S(2), y + S(2), (UINT32)(size - S(4)), (UINT32)(size - S(18)), 0xFF3A4A5A, S(4));
    /* Screen panel */
    gui_fill_rect(x + S(5), y + S(5), (UINT32)(size - S(10)), (UINT32)(size - S(24)), 0xFF0D1117);
    /* Green "> _" prompt on screen */
    gui_draw_char(x + S(7),  y + S(10), '>', 0xFF3FB950);
    gui_fill_rect(x + S(16), y + S(10), (UINT32)S(10), (UINT32)FONT_H, 0xFF3FB950);
    /* Output lines */
    gui_fill_rect(x + S(7), y + S(20), (UINT32)(size - S(20)), (UINT32)S(2), 0xFF2D4A6F);
    gui_fill_rect(x + S(7), y + S(25), (UINT32)(size - S(26)), (UINT32)S(2), 0xFF2D4A6F);
    /* Neck */
    gui_fill_rect(x + size/2 - S(3), y + size - S(16), (UINT32)S(6), (UINT32)S(8), 0xFF2A3A4A);
    /* Base */
    gui_fill_rounded(x + size/2 - S(12), y + size - S(8), (UINT32)S(24), (UINT32)S(6), 0xFF2A3A4A, S(2));
}

/* ======================================================================
 * File Manager  — classic yellow folder with tab and depth shadow
 * ====================================================================== */
static void icon_files(int x, int y, int size)
{
    /* Shadow/depth */
    gui_fill_rounded(x + S(6), y + S(17),
                     (UINT32)(size - S(8)), (UINT32)(size - S(20)),
                     0xFFA07800, S(3));

    /* Folder body */
    gui_fill_rounded(x + S(4), y + S(14),
                     (UINT32)(size - S(8)), (UINT32)(size - S(20)),
                     0xFFD4A017, S(3));

    /* Tab (top-left flap) */
    gui_fill_rounded(x + S(4), y + S(8),
                     (UINT32)S(20), (UINT32)S(10), 0xFFE8C000, S(2));

    /* Highlight strip at top of body */
    gui_fill_rect(x + S(5), y + S(14), (UINT32)(size - S(10)), (UINT32)S(3),
                  0xFFFFDC4A);

    /* Page lines (lighter yellow) */
    gui_fill_rect(x + S(9), y + S(22), (UINT32)(size - S(18)), (UINT32)S(2), 0xFFFFE97A);
    gui_fill_rect(x + S(9), y + S(28), (UINT32)(size - S(22)), (UINT32)S(2), 0xFFFFE97A);
    gui_fill_rect(x + S(9), y + S(34), (UINT32)(size - S(26)), (UINT32)S(2), 0xFFFFE97A);
}

/* ======================================================================
 * Text Editor  — document page with dog-ear corner + code lines
 * ====================================================================== */
static void icon_editor(int x, int y, int size)
{
    int px = x + S(6),  py = y + S(3);
    int pw = size - S(12), ph = size - S(8);
    int dogX = px + pw - S(10);          /* dog-ear corner x */

    /* Paper body */
    gui_fill_rounded(px, py, (UINT32)pw, (UINT32)ph, 0xFF162616, S(2));
    gui_draw_border_rounded(px, py, (UINT32)pw, (UINT32)ph, 0xFF3FB950, S(2));

    /* Dog-ear corner (fold) */
    gui_fill_rect(dogX, py, (UINT32)S(10), (UINT32)S(10), 0xFF0D1117);
    gui_fill_rect(dogX, py,               (UINT32)S(10), (UINT32)S(1),  0xFF3FB950);
    gui_fill_rect(dogX, py,               (UINT32)S(1),  (UINT32)S(10), 0xFF3FB950);

    /* Code lines — green first (active), muted rest */
    gui_fill_rect(px + S(4), py + S(14), (UINT32)(pw - S(18)), (UINT32)S(2), 0xFF3FB950);
    gui_fill_rect(px + S(4), py + S(20), (UINT32)(pw - S(14)), (UINT32)S(2), 0xFF4A5A72);
    gui_fill_rect(px + S(4), py + S(26), (UINT32)(pw - S(14)), (UINT32)S(2), 0xFF4A5A72);
    gui_fill_rect(px + S(4), py + S(32), (UINT32)(pw - S(20)), (UINT32)S(2), 0xFF4A5A72);

    /* Blinking cursor at start of line */
    gui_fill_rect(px + S(4), py + S(14), (UINT32)S(2), (UINT32)(FONT_H), 0xFF3FB950);
}

/* ======================================================================
 * Calculator  — device body + display + 4x3 button grid
 * ====================================================================== */
static void icon_calc(int x, int y, int size)
{
    int bx = x + S(4),  by = y + S(4);
    int bw = size - S(8), bh = size - S(8);
    UINT32 r, c;

    /* Device body */
    gui_fill_rounded(bx, by, (UINT32)bw, (UINT32)bh, 0xFF1A2C3A, S(5));
    gui_draw_border_rounded(bx, by, (UINT32)bw, (UINT32)bh, 0xFF2D7DD2, S(2));

    /* Display area */
    gui_fill_rect(bx + S(3), by + S(3), (UINT32)(bw - S(6)), (UINT32)S(11),
                  0xFF2D4A6F);
    /* "0" on display */
    gui_draw_char(bx + bw / 2 - CELL_W / 2, by + S(4), '0', C_TEXT_PRIMARY);

    /* 4 columns × 3 rows of buttons */
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 4; c++) {
            int btnx = bx + S(3) + (int)c * (S(8) + S(1));
            int btny = by + S(17) + (int)r * (S(7) + S(1));
            UINT32 bc = (r == 2 && c == 3) ? C_ACCENT : 0xFF243444;
            gui_fill_rounded(btnx, btny, (UINT32)S(7), (UINT32)S(6), bc, S(1));
        }
    }
}

/* ======================================================================
 * Settings  — gear: outer ring + 8 teeth + inner cutout + centre dot
 * ====================================================================== */
static void icon_settings(int x, int y, int size)
{
    int cx    = x + size / 2;
    int cy    = y + size / 2;
    int outer = S(20);
    int inner = S(10);
    int tooth = S(5);
    int gap   = inner + S(1);

    /* Cardinal teeth (N/S/E/W) */
    gui_fill_rect(cx - S(2), cy - outer,     (UINT32)S(4), (UINT32)(outer - inner), 0xFF8A9AB0);
    gui_fill_rect(cx - S(2), cy + inner,     (UINT32)S(4), (UINT32)(outer - inner), 0xFF8A9AB0);
    gui_fill_rect(cx - outer,     cy - S(2), (UINT32)(outer - inner), (UINT32)S(4), 0xFF8A9AB0);
    gui_fill_rect(cx + inner,     cy - S(2), (UINT32)(outer - inner), (UINT32)S(4), 0xFF8A9AB0);

    /* Diagonal teeth (NE/NW/SE/SW) */
    gui_fill_rect(cx - gap - tooth, cy - gap - tooth, (UINT32)tooth, (UINT32)tooth, 0xFF8A9AB0);
    gui_fill_rect(cx + gap,         cy - gap - tooth, (UINT32)tooth, (UINT32)tooth, 0xFF8A9AB0);
    gui_fill_rect(cx - gap - tooth, cy + gap,         (UINT32)tooth, (UINT32)tooth, 0xFF8A9AB0);
    gui_fill_rect(cx + gap,         cy + gap,         (UINT32)tooth, (UINT32)tooth, 0xFF8A9AB0);

    /* Outer filled circle (gear body) */
    gui_fill_rounded(cx - outer, cy - outer,
                     (UINT32)(outer * 2), (UINT32)(outer * 2), 0xFF6A7A8A, outer);

    /* Inner cutout (hollow centre) */
    gui_fill_rounded(cx - inner, cy - inner,
                     (UINT32)(inner * 2), (UINT32)(inner * 2), 0xFF1A2232, inner);

    /* Centre dot (bolt) */
    gui_fill_rounded(cx - S(3), cy - S(3), (UINT32)S(6), (UINT32)S(6), 0xFF8A9AB0, S(3));
}

/* ======================================================================
 * About  — info circle with "i" letter
 * ====================================================================== */
static void icon_about(int x, int y, int size)
{
    int cx = x + size / 2;
    int cy = y + size / 2;
    int ro = S(22);

    /* Filled circle */
    gui_fill_rounded(cx - ro, cy - ro,
                     (UINT32)(ro * 2), (UINT32)(ro * 2), 0xFF1A3A5A, ro);

    /* Ring border */
    gui_draw_border_rounded(cx - ro, cy - ro,
                            (UINT32)(ro * 2), (UINT32)(ro * 2), 0xFF2D7DD2, ro);

    /* "i" dot */
    gui_fill_rounded(cx - S(2), cy - S(11),
                     (UINT32)S(5), (UINT32)S(5), 0xFF2D7DD2, S(2));

    /* "i" vertical stroke */
    gui_fill_rect(cx - S(2), cy - S(2),  (UINT32)S(5), (UINT32)S(12), 0xFF2D7DD2);

    /* "i" serif base */
    gui_fill_rect(cx - S(5), cy + S(10), (UINT32)S(11), (UINT32)S(2), 0xFF2D7DD2);
}

/* ======================================================================
 * Disk Manager - stacked disk platters with partition bars
 * ====================================================================== */
static void icon_diskmgmt(int x, int y, int size)
{
    int dx = x + S(5);
    int dy = y + S(6);
    int dw = size - S(10);
    gui_fill_rounded(dx, dy, (UINT32)dw, (UINT32)S(12), 0xFF24483F, S(5));
    gui_draw_border_rounded(dx, dy, (UINT32)dw, (UINT32)S(12), 0xFF3FB950, S(5));
    gui_fill_rounded(dx, dy + S(11), (UINT32)dw, (UINT32)S(12), 0xFF1A3A34, S(5));
    gui_draw_border_rounded(dx, dy + S(11), (UINT32)dw, (UINT32)S(12), 0xFF2A6F5F, S(5));
    gui_fill_rounded(dx, dy + S(22), (UINT32)dw, (UINT32)S(14), 0xFF132D2A, S(5));
    gui_draw_border_rounded(dx, dy + S(22), (UINT32)dw, (UINT32)S(14), 0xFF2A6F5F, S(5));
    gui_fill_rect(dx + S(5), dy + S(27), (UINT32)S(12), (UINT32)S(4), 0xFF2D7DD2);
    gui_fill_rect(dx + S(19), dy + S(27), (UINT32)S(9), (UINT32)S(4), 0xFFD4A017);
    gui_fill_rect(dx + S(30), dy + S(27), (UINT32)S(8), (UINT32)S(4), 0xFF3FB950);
}

/* ======================================================================
 * Public dispatcher
 * ====================================================================== */
void gui_icon_draw(int win_id, int x, int y, int size)
{
    switch (win_id) {
        case GUI_WIN_TERMINAL: icon_terminal(x, y, size); break;
        case GUI_WIN_FILES:    icon_files   (x, y, size); break;
        case GUI_WIN_EDITOR:   icon_editor  (x, y, size); break;
        case GUI_WIN_CALC:     icon_calc    (x, y, size); break;
        case GUI_WIN_SETTINGS: icon_settings(x, y, size); break;
        case GUI_WIN_ABOUT:    icon_about   (x, y, size); break;
        case GUI_WIN_DISKMGMT: icon_diskmgmt(x, y, size); break;
        default: break;
    }
}

#undef S
