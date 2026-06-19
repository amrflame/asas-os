/*
 * gui_main.c — Unified GUI dispatcher
 *
 * Routes gui_initialize / gui_thread_entry / gui_terminal_write /
 * gui_set_input_line to either safemode_gui or the modern ADE.
 *
 * The modern GUI render loop runs entirely here; it drives all
 * sub-systems (compositor, wm, desktop, taskbar, startmenu, apps).
 */

#include "gui.h"
#include "safemode_gui.h"
#include "gui_compositor.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_wm.h"
#include "gui_desktop.h"
#include "gui_taskbar.h"
#include "gui_startmenu.h"
#include "gui_anim.h"
#include "gui_notify.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "scheduler.h"
#include "shell.h"
#include "mouse.h"

/* Internal modern-GUI helpers (defined later in this file) */
static void gui_modern_initialize  (ASAS_FRAMEBUFFER *framebuffer);
static void gui_modern_thread_entry(void);

/* Forward declarations for app init / helpers */
void app_terminal_initialize(void);
void app_terminal_write     (const char *text);
void app_terminal_set_input (const char *line, UINT32 length);
int  app_terminal_consume_dirty(void);
void app_terminal_scroll    (int delta);
void app_files_initialize   (void);
void app_files_refresh      (void);
void app_files_handle_click (int mx, int my);
void app_files_update_hover (int mx, int my);
void app_editor_initialize  (void);
void app_editor_handle_click(int mx, int my);
void app_editor_open_file   (const char *path);
void app_editor_scroll      (int delta);
void app_calc_initialize    (void);
void app_calc_handle_click  (int mx, int my);
void app_calc_update_hover  (int mx, int my);
void app_settings_initialize(void);
void app_settings_handle_click(int mx, int my);
void app_settings_update_hover(int mx, int my);
void app_diskmgmt_initialize(void);
void app_diskmgmt_handle_click(int mx, int my);
void app_diskmgmt_update_hover(int mx, int my);
void app_about_initialize   (void);
void app_about_handle_click (int mx, int my);
void app_about_update_hover (int mx, int my);
void app_editor_update_hover(int mx, int my);

/* ======================================================================
 * Mode flag
 * ====================================================================== */
static int s_safe_mode = 0;

void gui_set_mode(int safe_mode) { s_safe_mode = safe_mode; }
int  gui_get_mode(void)          { return s_safe_mode;      }

/* ======================================================================
 * Public API — dispatcher
 * ====================================================================== */
void gui_initialize(ASAS_FRAMEBUFFER *framebuffer)
{
    if (s_safe_mode) {
        safemode_gui_initialize(framebuffer);
    } else {
        gui_modern_initialize(framebuffer);
    }
}

void gui_thread_entry(void)
{
    if (s_safe_mode) {
        safemode_gui_thread_entry();
    } else {
        gui_modern_thread_entry();
    }
}

void gui_terminal_write(const char *text)
{
    if (s_safe_mode) {
        safemode_gui_terminal_write(text);
    } else {
        app_terminal_write(text);
    }
}

void gui_set_input_line(const char *line, UINT32 length)
{
    if (s_safe_mode) {
        safemode_gui_set_input_line(line, length);
    } else {
        app_terminal_set_input(line, length);
    }
}

UINT32 gui_loop_ticks(void)
{
    if (s_safe_mode) return safemode_gui_loop_ticks();
    return gui_compositor_loop_ticks();
}

void gui_render_desktop(ASAS_FRAMEBUFFER *framebuffer)
{
    if (s_safe_mode) safemode_gui_render_desktop(framebuffer);
    (void)framebuffer;
}

/* ======================================================================
 * Modern GUI — initialise all subsystems
 * ====================================================================== */
static void gui_modern_initialize(ASAS_FRAMEBUFFER *framebuffer)
{
    gui_compositor_initialize(framebuffer);
    gui_anim_initialize();
    gui_notify_initialize();
    gui_wm_initialize();
    gui_desktop_initialize();
    gui_startmenu_initialize();

    /* Initialise all apps (they register their own paint/key callbacks) */
    app_terminal_initialize();  /* also installs logger hook */
    app_files_initialize();
    app_editor_initialize();
    app_calc_initialize();
    app_settings_initialize();
    app_diskmgmt_initialize();
    app_about_initialize();

    /* Show terminal by default */
    gui_wm_show(GUI_WIN_TERMINAL);
    gui_wm_show(GUI_WIN_FILES);
}

/* ======================================================================
 * Modern GUI — main render loop (runs as a scheduler thread)
 * ====================================================================== */
static void gui_modern_thread_entry(void)
{
    UINT32 file_refresh_timer = 0;
    UINT32 prev_ticks         = 0;

    for (;;) {
        int    dirty  = 0;
        UINT8  buttons, prev;
        int    mx, my;
        int    pressed;

        gui_compositor_tick();

        /* --- Input --- */
        shell_poll_input_once();

        if (gui_compositor_update_input()) dirty = 1;

        buttons = gui_compositor_buttons();
        prev    = gui_compositor_prev_buttons();
        mx      = gui_compositor_cursor_x();
        my      = gui_compositor_cursor_y();
        pressed = (buttons & 1) && !(prev & 1);

        /* Hover updates — always, not just on press */
        app_settings_update_hover(mx, my);
        app_about_update_hover(mx, my);
        app_calc_update_hover(mx, my);
        app_editor_update_hover(mx, my);
        app_files_update_hover(mx, my);
        app_diskmgmt_update_hover(mx, my);

        if (pressed) {
            /* Priority: start menu → taskbar → window manager → desktop */
            if (gui_startmenu_visible()) {
                gui_startmenu_handle_click(mx, my);
            } else if (gui_taskbar_hit(mx, my)) {
                gui_taskbar_handle_click(mx, my);
            } else {
                gui_wm_handle_mouse(mx, my, buttons, prev);
                /* Per-app click handlers */
                app_calc_handle_click(mx, my);
                app_settings_handle_click(mx, my);
                app_diskmgmt_handle_click(mx, my);
                app_about_handle_click(mx, my);
                app_files_handle_click(mx, my);
                app_editor_handle_click(mx, my);
                /* Desktop icon click */
                gui_desktop_handle_click(mx, my, 0);
            }
        } else if (buttons & 1) {
            /* Drag */
            gui_wm_handle_mouse(mx, my, buttons, prev);
        } else if (!(buttons & 1) && (prev & 1)) {
            /* Release */
            gui_wm_handle_mouse(mx, my, buttons, prev);
        }
        gui_compositor_consume_buttons();

        /* --- Keyboard --- */
        if (keyboard_has_data()) {
            UINT8 sc = keyboard_read_scancode();
            /* ESC while no focused window → toggle start menu */
            if (sc == 0x01 && gui_wm_focused() < 0) {
                gui_startmenu_toggle();
            } else if (gui_startmenu_visible()) {
                gui_startmenu_handle_key(sc);
            } else {
                gui_wm_handle_key(sc);
            }
            dirty = 1;
        }

        /* --- Mouse wheel scroll --- */
        {
            int scroll = mouse_consume_scroll();
            if (scroll != 0) {
                int foc = gui_wm_focused();
                if (foc == GUI_WIN_TERMINAL) app_terminal_scroll(scroll);
                else if (foc == GUI_WIN_EDITOR) app_editor_scroll(scroll);
                dirty = 1;
            }
        }

        /* --- Terminal dirty --- */
        if (app_terminal_consume_dirty()) dirty = 1;

        /* --- File list refresh (~15s) --- */
        file_refresh_timer++;
        if (file_refresh_timer >= 1500) {
            file_refresh_timer = 0;
            app_files_refresh();
            dirty = 1;
        }

        /* --- Cursor blink (~0.5s) --- */
        {
            UINT32 t = gui_compositor_loop_ticks();
            if ((t / 30) != (prev_ticks / 30)) dirty = 1;
            prev_ticks = t;
        }

        /* --- Render frame --- */
        if (dirty) {
            /* 1. Desktop background */
            gui_compositor_draw_wallpaper();

            /* 2. Desktop icons (left column) */
            gui_desktop_render();

            /* 3. All windows (back-to-front) */
            gui_wm_render_all();

            /* 4. Taskbar (always on top) */
            gui_taskbar_render();

            /* 5. Start menu if open */
            if (gui_startmenu_visible()) gui_startmenu_render();

            /* 6. Toast notifications (above windows, below cursor) */
            gui_notify_tick();
            gui_notify_paint(gui_compositor_width(), gui_compositor_height());

            /* 7. Mouse cursor */
            gui_compositor_render_cursor();

            /* 8. Blit to real framebuffer */
            gui_compositor_blit();
        }

        scheduler_yield();
    }
}
