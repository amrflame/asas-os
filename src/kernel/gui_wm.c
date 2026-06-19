/*
 * gui_wm.c — Window Manager: chrome rendering, dragging, focus, hit-testing
 */

#include "gui_wm.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_compositor.h"
#include "gui_anim.h"

/* ======================================================================
 * Z-order stack — bottom [0] to top [GUI_WIN_COUNT-1]
 * When a window is focused it moves to the top of the stack.
 * ====================================================================== */
static UINT32 s_zstack[GUI_WIN_COUNT];

static void zstack_init(void)
{
    UINT32 i;
    for (i = 0; i < GUI_WIN_COUNT; i++) s_zstack[i] = i;
}

static void zstack_bring_top(UINT32 id)
{
    UINT32 i, pos = GUI_WIN_COUNT - 1;
    /* Find current position */
    for (i = 0; i < GUI_WIN_COUNT; i++) {
        if (s_zstack[i] == id) { pos = i; break; }
    }
    /* Shift everything above pos down by one */
    for (i = pos; i + 1 < GUI_WIN_COUNT; i++) s_zstack[i] = s_zstack[i + 1];
    s_zstack[GUI_WIN_COUNT - 1] = id;
}

/* ======================================================================
 * Window table
 * ====================================================================== */
static GUI_WIN s_wins[GUI_WIN_COUNT];
static int     s_focused = -1;

/* Default window positions/sizes (percentage-based, set at init) */
typedef struct { int xpct, ypct; UINT32 wpct, hpct; } WIN_DEF;

void gui_wm_initialize(void)
{
    UINT32 sw = gui_compositor_width();
    UINT32 sh = gui_compositor_height();
    UINT32 desk_h = sh > (UINT32)TASKBAR_H ? sh - (UINT32)TASKBAR_H : sh;
    UINT32 i;

    /* Default geometry table: x%, y%, w%, h% of desktop area */
    static const struct {
        const char *title;
        UINT32       icon_color;
        int    xp, yp;
        UINT32 wp, hp;
    } defs[GUI_WIN_COUNT] = {
        { "Terminal",    0xFF3A3A4A,   8,  8,  55, 55 },
        { "File Manager",0xFFD4A017,  67,  8,  30, 60 },
        { "Text Editor", 0xFF1E6B2A,   8,  8,  60, 65 },
        { "Calculator",  0xFF2D7DD2,  72, 15,  20, 60 },
        { "Settings",    0xFF555566,  25, 15,  55, 65 },
        { "About",       0xFFD4A017,  38, 25,  22, 45 },
        { "Disk Manager",0xFF2A6F5F,  12, 10,  76, 72 },
    };

    for (i = 0; i < GUI_WIN_COUNT; i++) {
        UINT32 d = i;  /* alias for readability */
        s_wins[i].id         = i;
        s_wins[i].icon_color = defs[d].icon_color;
        s_wins[i].x          = (int)((UINT64)sw * (UINT32)defs[d].xp / 100);
        s_wins[i].y          = (int)((UINT64)desk_h * (UINT32)defs[d].yp / 100);
        s_wins[i].w          = sw * defs[d].wp / 100;
        s_wins[i].h          = desk_h * defs[d].hp / 100;
        s_wins[i].minimized  = 1;   /* all start minimised */
        s_wins[i].dragging   = 0;
        s_wins[i].resizing   = 0;
        s_wins[i].maximized  = 0;
        s_wins[i].on_paint   = 0;
        s_wins[i].on_key     = 0;

        /* Copy title */
        {
            UINT32 ti = 0;
            const char *src = defs[d].title;
            while (src[ti] && ti < 31) { s_wins[i].title[ti] = src[ti]; ti++; }
            s_wins[i].title[ti] = '\0';
        }
    }

    s_focused = -1;
    zstack_init();
}

void gui_wm_set_callbacks(UINT32 id,
                           void (*on_paint)(GUI_WIN *),
                           void (*on_key  )(GUI_WIN *, UINT8))
{
    if (id >= GUI_WIN_COUNT) return;
    s_wins[id].on_paint = on_paint;
    s_wins[id].on_key   = on_key;
}

void gui_wm_show(UINT32 id)
{
    if (id >= GUI_WIN_COUNT) return;
    s_wins[id].minimized = 0;
    s_focused = (int)id;
    zstack_bring_top(id);
    gui_anim_start((int)id, GUI_ANIM_OPEN);
}
void gui_wm_hide(UINT32 id)
{
    if (id >= GUI_WIN_COUNT) return;
    s_wins[id].minimized = 1;
    if (s_focused == (int)id) s_focused = -1;
    gui_anim_start((int)id, GUI_ANIM_NONE);
}
void gui_wm_toggle(UINT32 id) { if (id < GUI_WIN_COUNT) { if (s_wins[id].minimized) gui_wm_show(id); else gui_wm_hide(id); } }
int  gui_wm_visible(UINT32 id) { return (id < GUI_WIN_COUNT) && !s_wins[id].minimized; }
int  gui_wm_focused(void) { return s_focused; }
const GUI_WIN *gui_wm_get(UINT32 id) { return (id < GUI_WIN_COUNT) ? &s_wins[id] : 0; }

/* ======================================================================
 * Chrome rendering
 * ====================================================================== */
static void render_chrome(const GUI_WIN *w)
{
    int focused = (s_focused == (int)w->id);
    UINT32 border_col = focused ? C_WIN_BORDER_FOCUS : C_WIN_BORDER;

    /* Body */
    gui_fill_rect(w->x, w->y + CHROME_H, w->w, w->h - (UINT32)CHROME_H, C_WIN_BODY);

    /* Titlebar */
    gui_fill_rect(w->x, w->y, w->w, (UINT32)CHROME_H, C_WIN_CHROME);

    /* Accent underline on titlebar */
    gui_fill_rect(w->x, w->y + CHROME_H - 2, w->w, 2,
                  focused ? C_WIN_CHROME_SEP : C_BORDER);

    /* Title text — vertically centred in 32px titlebar */
    gui_draw_text(w->x + 8 + 3 * (CHROME_BTN_W + 4), w->y + (CHROME_H - FONT_H) / 2,
                  w->title, focused ? C_TEXT_PRIMARY : C_TEXT_MUTED);

    /* --- Control buttons (right side) --- */
    /* Close */
    {
        int bx = w->x + (int)w->w - CHROME_BTN_W - 4;
        int by = w->y + CHROME_BTN_PAD;
        gui_fill_rounded(bx, by, (UINT32)CHROME_BTN_W, (UINT32)CHROME_BTN_H, C_BTN_CLOSE, 3);
        gui_draw_text_centered(bx, by, (UINT32)CHROME_BTN_W, (UINT32)CHROME_BTN_H, "X", C_BTN_ICON);
    }
    /* Maximise */
    {
        int bx = w->x + (int)w->w - 2 * CHROME_BTN_W - 8;
        int by = w->y + CHROME_BTN_PAD;
        gui_fill_rounded(bx, by, (UINT32)CHROME_BTN_W, (UINT32)CHROME_BTN_H, C_BTN_MAX, 3);
        gui_draw_text_centered(bx, by, (UINT32)CHROME_BTN_W, (UINT32)CHROME_BTN_H, "=", C_BTN_ICON);
    }
    /* Minimise */
    {
        int bx = w->x + (int)w->w - 3 * CHROME_BTN_W - 12;
        int by = w->y + CHROME_BTN_PAD;
        gui_fill_rounded(bx, by, (UINT32)CHROME_BTN_W, (UINT32)CHROME_BTN_H, C_BTN_MIN, 3);
        gui_draw_text_centered(bx, by, (UINT32)CHROME_BTN_W, (UINT32)CHROME_BTN_H, "-", C_BTN_ICON);
    }

    /* Window border */
    gui_draw_border(w->x, w->y, w->w, w->h, border_col);

    /* Resize grip — 3 dots at bottom-right (not shown when maximized) */
    if (!w->maximized) {
        int gx = w->x + (int)w->w - 10;
        int gy = w->y + (int)w->h - 10;
        UINT32 gc = focused ? C_TEXT_LABEL : C_BORDER;
        gui_fill_rect(gx + 5, gy + 3, 3, 3, gc);
        gui_fill_rect(gx + 2, gy + 6, 3, 3, gc);
        gui_fill_rect(gx + 5, gy + 6, 3, 3, gc);
    }
}

/* ======================================================================
 * Render all visible windows
 * ====================================================================== */
void gui_wm_render_all(void)
{
    UINT32 i;

    /* Advance all animations by one tick */
    gui_anim_tick();

    /* Render in z-order: bottom first, skip the focused window */
    for (i = 0; i < GUI_WIN_COUNT; i++) {
        UINT32 wid = s_zstack[i];
        GUI_WIN *w = &s_wins[wid];
        if (w->minimized) continue;
        if ((int)wid == s_focused) continue;
        render_chrome(w);
        if (w->on_paint) w->on_paint(w);
        if (gui_anim_active((int)wid)) {
            UINT32 sc = gui_anim_get_scale((int)wid);
            if (sc < 256u) {
                UINT32 bw = w->w * sc / 256u;
                UINT32 bh = w->h * sc / 256u;
                int    bx = w->x + ((int)w->w - (int)bw) / 2;
                int    by = w->y + ((int)w->h - (int)bh) / 2;
                gui_draw_border(bx, by, bw, bh, C_ACCENT);
            }
        }
    }
    /* Render focused window last (on top) */
    if (s_focused >= 0 && !s_wins[s_focused].minimized) {
        GUI_WIN *w = &s_wins[s_focused];
        render_chrome(w);
        if (w->on_paint) w->on_paint(w);
        if (gui_anim_active(s_focused)) {
            UINT32 sc = gui_anim_get_scale(s_focused);
            if (sc < 256u) {
                UINT32 bw = w->w * sc / 256u;
                UINT32 bh = w->h * sc / 256u;
                int    bx = w->x + ((int)w->w - (int)bw) / 2;
                int    by = w->y + ((int)w->h - (int)bh) / 2;
                gui_draw_border(bx, by, bw, bh, C_ACCENT);
            }
        }
    }
}

/* ======================================================================
 * Hit-testing helpers
 * ====================================================================== */
static int hit_chrome(const GUI_WIN *w, int mx, int my)
{
    return mx >= w->x && mx < w->x + (int)w->w
        && my >= w->y && my < w->y + CHROME_H;
}

static int hit_close(const GUI_WIN *w, int mx, int my)
{
    int bx = w->x + (int)w->w - CHROME_BTN_W - 4;
    int by = w->y + CHROME_BTN_PAD;
    return mx >= bx && mx < bx + CHROME_BTN_W
        && my >= by && my < by + CHROME_BTN_H;
}

static int hit_minimize(const GUI_WIN *w, int mx, int my)
{
    int bx = w->x + (int)w->w - 3 * CHROME_BTN_W - 12;
    int by = w->y + CHROME_BTN_PAD;
    return mx >= bx && mx < bx + CHROME_BTN_W
        && my >= by && my < by + CHROME_BTN_H;
}

static int hit_maximize(const GUI_WIN *w, int mx, int my)
{
    int bx = w->x + (int)w->w - 2 * CHROME_BTN_W - 8;
    int by = w->y + CHROME_BTN_PAD;
    return mx >= bx && mx < bx + CHROME_BTN_W
        && my >= by && my < by + CHROME_BTN_H;
}

static int hit_resize_grip(const GUI_WIN *w, int mx, int my)
{
    if (w->maximized) return 0;
    return mx >= w->x + (int)w->w - 14
        && mx <  w->x + (int)w->w
        && my >= w->y + (int)w->h - 14
        && my <  w->y + (int)w->h;
}

static int hit_body(const GUI_WIN *w, int mx, int my)
{
    return mx >= w->x && mx < w->x + (int)w->w
        && my >= w->y && my < w->y + (int)w->h;
}

/* ======================================================================
 * Mouse event handling
 * ====================================================================== */
void gui_wm_handle_mouse(int mx, int my, UINT8 buttons, UINT8 prev)
{
    int   pressed  = (buttons & 1) && !(prev & 1);
    int   released = !(buttons & 1) && (prev & 1);
    UINT32 i;

    if (pressed) {
        /* Iterate top-to-bottom in z-stack */
        for (i = GUI_WIN_COUNT; i-- > 0; ) {
            UINT32 wid = s_zstack[i];
            GUI_WIN *w = &s_wins[wid];
            if (w->minimized) continue;

            if (hit_close(w, mx, my)) {
                gui_wm_hide(wid); return;
            }
            if (hit_minimize(w, mx, my)) {
                gui_wm_hide(wid); return;
            }
            if (hit_maximize(w, mx, my)) {
                UINT32 sw = gui_compositor_width();
                UINT32 sh = gui_compositor_height();
                if (!w->maximized) {
                    /* Save current geometry */
                    w->restore_x = w->x; w->restore_y = w->y;
                    w->restore_w = w->w; w->restore_h = w->h;
                    /* Fill desktop area */
                    w->x = 0; w->y = 0;
                    w->w = sw; w->h = sh > (UINT32)TASKBAR_H ? sh - (UINT32)TASKBAR_H : sh;
                    w->maximized = 1;
                } else {
                    /* Restore */
                    w->x = w->restore_x; w->y = w->restore_y;
                    w->w = w->restore_w; w->h = w->restore_h;
                    w->maximized = 0;
                }
                s_focused = (int)wid;
                return;
            }
            if (hit_resize_grip(w, mx, my)) {
                w->resizing  = 1;
                w->resize_ox = mx;
                w->resize_oy = my;
                s_focused    = (int)wid;
                return;
            }
            if (hit_chrome(w, mx, my)) {
                w->dragging = 1;
                w->drag_ox  = mx - w->x;
                w->drag_oy  = my - w->y;
                s_focused   = (int)wid;
                zstack_bring_top(wid);
                return;
            }
            if (hit_body(w, mx, my)) {
                s_focused = (int)wid;
                zstack_bring_top(wid);
                return;
            }
        }
    }

    /* Drag (move) */
    if ((buttons & 1) && s_focused >= 0 && s_wins[s_focused].dragging) {
        GUI_WIN *w  = &s_wins[s_focused];
        UINT32   sw = gui_compositor_width();
        UINT32   sh = gui_compositor_height();
        w->x = mx - w->drag_ox;
        w->y = my - w->drag_oy;
        if (w->x < 0)                            w->x = 0;
        if (w->x + (int)w->w > (int)sw)          w->x = (int)sw - (int)w->w;
        if (w->y < 0)                             w->y = 0;
        if (w->y + (int)w->h > (int)sh - TASKBAR_H) w->y = (int)sh - TASKBAR_H - (int)w->h;
    }

    /* Resize drag */
    if ((buttons & 1) && s_focused >= 0 && s_wins[s_focused].resizing) {
        GUI_WIN *w  = &s_wins[s_focused];
        int dx = mx - w->resize_ox;
        int dy = my - w->resize_oy;
        w->resize_ox = mx;
        w->resize_oy = my;
        int nw = (int)w->w + dx;
        int nh = (int)w->h + dy;
        if (nw < 180) nw = 180;
        if (nh < 130) nh = 130;
        w->w = (UINT32)nw;
        w->h = (UINT32)nh;
    }

    if (released) {
        for (i = 0; i < GUI_WIN_COUNT; i++) {
            s_wins[i].dragging = 0;
            s_wins[i].resizing = 0;
        }
    }
}

/* ======================================================================
 * Keyboard
 * ====================================================================== */
void gui_wm_handle_key(UINT8 scancode)
{
    if (s_focused >= 0 && !s_wins[s_focused].minimized && s_wins[s_focused].on_key) {
        s_wins[s_focused].on_key(&s_wins[s_focused], scancode);
    }
}
