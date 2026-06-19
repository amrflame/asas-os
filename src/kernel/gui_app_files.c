/*
 * gui_app_files.c — File Manager: bookmarks sidebar + icon/list view
 */

#include "gui_wm.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_compositor.h"
#include "gui_widget.h"
#include "vfs.h"
#include "virtual_disk.h"
#include "partition.h"
#include "logger.h"
#include "scheduler.h"

/* Forward declaration for cross-app open */
void app_editor_open_file(const char *path);
void app_files_refresh(void);

#pragma intrinsic(_InterlockedExchange)
long _InterlockedExchange(long volatile *target, long value);

#define FILES_MAX      128
#define SIDEBAR_W      90
#define ICON_CELL_W    56
#define ICON_CELL_H    52
#define ICON_IMG_W     32
#define ICON_IMG_H     32
#define TOOLBAR_H      26
#define STATUSBAR_H    20

typedef struct {
    char  label[16];
    char  path[32];
} BOOKMARK;

static const BOOKMARK s_bookmarks[] = {
    { "Home",   "/" },
    { "System", "/ASAS" },
    { "Trash",  "/trash" },
};
#define BOOKMARK_COUNT 3

static VFS_DIRECTORY_ENTRY s_files[FILES_MAX];
static UINT64              s_file_count;
static char                s_path[64];
static UINT32              s_selected_bm;
static UINT32              s_selected_file;
static volatile long       s_lock;

/* Double-click state */
static UINT32 s_last_click_file = 0xFFFFFFFFu;
static UINT32 s_last_click_tick = 0;
#define FILES_DBLCLICK_TICKS 40
static UINT32 s_auto_refresh_tick = 0;
#define FILES_AUTOREFRESH_TICKS 180   /* ~3 s at 60 fps */

/* Toolbar widgets */
static GUI_WIDGET s_btn_up, s_btn_refresh;
/* Sidebar bookmark buttons */
static GUI_WIDGET s_bm_btns[BOOKMARK_COUNT];

static void files_lock  (void) { while (_InterlockedExchange(&s_lock, 1) != 0) scheduler_yield(); }
static void files_unlock(void) { (void)_InterlockedExchange(&s_lock, 0); }

static void scopy(char *dst, const char *src, UINT32 max)
{
    UINT32 i = 0;
    while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static char lower_char(char value)
{
    if (value >= 'A' && value <= 'Z') return (char)(value - 'A' + 'a');
    return value;
}

static int name_has_suffix(const char *name, const char *suffix)
{
    UINT32 name_len = 0;
    UINT32 suffix_len = 0;
    UINT32 index;
    while (name[name_len]) name_len++;
    while (suffix[suffix_len]) suffix_len++;
    if (suffix_len == 0 || name_len < suffix_len) return 0;
    for (index = 0; index < suffix_len; index++) {
        if (lower_char(name[name_len - suffix_len + index]) !=
            lower_char(suffix[index])) return 0;
    }
    return 1;
}

static int file_is_virtual_disk_image(const char *name)
{
    return name_has_suffix(name, ".img") ||
           name_has_suffix(name, ".raw") ||
           name_has_suffix(name, ".vhd") ||
           name_has_suffix(name, ".qcow2") ||
           name_has_suffix(name, ".vhdx");
}

static int files_attach_virtual_disk(const char *path)
{
    ASAS_BLOCK_DEVICE *device;
    if (name_has_suffix(path, ".qcow2") || name_has_suffix(path, ".vhdx")) {
        if (virtual_disk_validate_image(path, "qcow2") ||
            virtual_disk_validate_image(path, "vhdx")) {
            logger_write("GUI", "virtual disk metadata valid; attach needs sparse manager");
        } else {
            logger_write("GUI", "virtual disk metadata invalid");
        }
        return 0;
    }
    device = virtual_disk_attach_auto(path, 0);
    if (device == 0) {
        logger_write("GUI", "virtual disk attach failed");
        return 0;
    }
    (void)partition_scan_device(device);
    (void)vfs_mount_all_volumes();
    app_files_refresh();
    logger_write("GUI", "virtual disk attached");
    logger_write("GUI", device->name);
    return 1;
}

void app_files_refresh(void)
{
    VFS_DIRECTORY_ENTRY tmp[FILES_MAX];
    UINT64 cnt = vfs_list_directory(s_path, tmp, FILES_MAX);
    UINT64 i;
    files_lock();
    s_file_count = cnt > FILES_MAX ? (UINT64)FILES_MAX : cnt;
    for (i = 0; i < s_file_count; i++) s_files[i] = tmp[i];
    files_unlock();
}

static void files_paint(GUI_WIN *win)
{
    int    wx = win->x, wy = win->y + CHROME_H;
    int    ww = (int)win->w, wh = (int)win->h - CHROME_H;
    int    cx, cy, cw, ch;
    UINT32 i, row, col;
    VFS_DIRECTORY_ENTRY snap[FILES_MAX];
    UINT64 snap_count;
    char path_snap[64];
    int slot;

    /* Auto-refresh the directory listing periodically */
    s_auto_refresh_tick++;
    if (s_auto_refresh_tick >= FILES_AUTOREFRESH_TICKS) {
        s_auto_refresh_tick = 0;
        app_files_refresh();
    }

    /* Snapshot */
    files_lock();
    snap_count = s_file_count;
    for (i = 0; i < snap_count; i++) snap[i] = s_files[i];
    scopy(path_snap, s_path, 64);
    files_unlock();

    /* Toolbar — breadcrumb + Up + Refresh buttons */
    gui_fill_rect(wx, wy, (UINT32)ww, (UINT32)TOOLBAR_H, C_WIN_CHROME);
    gui_fill_rect(wx, wy + TOOLBAR_H - 1, (UINT32)ww, 1, C_BORDER);

    /* Reposition toolbar widgets each frame */
    s_btn_up.x      = wx + 4;       s_btn_up.y      = wy + 4;
    s_btn_refresh.x = wx + 28;      s_btn_refresh.y = wy + 4;
    gui_widget_render(&s_btn_up);
    gui_widget_render(&s_btn_refresh);

    /* Breadcrumb path */
    gui_draw_text(wx + 56, wy + (TOOLBAR_H - FONT_H) / 2, "/",       C_TEXT_MUTED);
    gui_draw_text(wx + 64, wy + (TOOLBAR_H - FONT_H) / 2, ">",       C_TEXT_LABEL);
    gui_draw_text(wx + 72, wy + (TOOLBAR_H - FONT_H) / 2, path_snap, C_TEXT_PRIMARY);

    wy += TOOLBAR_H;
    wh -= TOOLBAR_H + STATUSBAR_H;

    /* Sidebar */
    gui_fill_rect(wx, wy, (UINT32)SIDEBAR_W, (UINT32)wh, C_SIDEBAR_BG);
    gui_fill_rect(wx + SIDEBAR_W, wy, 1, (UINT32)wh, C_BORDER);

    /* Bookmarks section label */
    gui_draw_text(wx + 6, wy + 6, "BOOKMARKS", C_TEXT_LABEL);

    /* Bookmark buttons */
    for (i = 0; i < BOOKMARK_COUNT; i++) {
        static const UINT32 bm_colors[] = { 0xFFD4A017, 0xFF2D7DD2, 0xFFE5534B };
        int by = wy + 18 + (int)i * (CELL_H + 6);

        /* Active stripe */
        if (i == s_selected_bm) {
            gui_fill_rect(wx, by - 2, (UINT32)SIDEBAR_W - 1, (UINT32)(CELL_H + 6), C_SIDEBAR_ACTIVE);
            gui_fill_rect(wx, by - 2, 2, (UINT32)(CELL_H + 6), C_SIDEBAR_ACTIVE_BD);
        }
        /* Reposition bookmark button */
        s_bm_btns[i].x = wx + 2;
        s_bm_btns[i].y = by - 2;
        gui_widget_render(&s_bm_btns[i]);

        /* Dot overlay (drawn after button so it's on top) */
        gui_fill_rounded(wx + 6, by + 1, 6, 6, bm_colors[i], 3);
    }

    /* ---- DRIVES section ---- */
    {
        int dvol_count = vfs_get_volume_count();
        if (dvol_count > 0) {
            int drives_label_y = wy + 18 + BOOKMARK_COUNT * (CELL_H + 6) + 8;
            int drives_first_y = drives_label_y + CELL_H + 2;
            int di;

            gui_draw_text(wx + 6, drives_label_y, "DRIVES", C_TEXT_LABEL);

            for (di = 0; di < dvol_count; di++) {
                const VFS_VOLUME_INFO *vol = vfs_get_volume(di);
                int dvy = drives_first_y + di * (CELL_H + 6);

                if (!vol || !vol->valid) continue;

                /* Active stripe */
                if (s_selected_bm == (UINT32)(BOOKMARK_COUNT + di)) {
                    gui_fill_rect(wx, dvy - 2, (UINT32)SIDEBAR_W - 1,
                                  (UINT32)(CELL_H + 6), C_SIDEBAR_ACTIVE);
                    gui_fill_rect(wx, dvy - 2, 2,
                                  (UINT32)(CELL_H + 6), C_SIDEBAR_ACTIVE_BD);
                }

                /* Drive icon */
                if (vol->is_cdrom) {
                    /* CD-ROM: cyan ring */
                    gui_fill_rounded(wx + 5, dvy, 8, 8, 0xFF00BFFF, 4);
                    gui_fill_rounded(wx + 7, dvy + 2, 4, 4, 0xFF1A1A2E, 2);
                } else {
                    /* HDD: blue rectangle with highlight */
                    gui_fill_rounded(wx + 5, dvy, 10, 8, 0xFF3A6EA8, 2);
                    gui_fill_rect(wx + 6, dvy + 1, 8, 1, 0xFF7BB8F0);
                }

                /* Volume label */
                gui_draw_text(wx + 18, dvy + 1, vol->label, C_TEXT_MUTED);
            }
        }
    }

    /* File icon grid */
    cx = wx + SIDEBAR_W + 4;
    cy = wy + 4;
    cw = ww - SIDEBAR_W - 8;
    ch = wh - 4;

    col = 0; row = 0;
    slot = 0;
    for (i = 0; i < snap_count && (int)(cy + (int)row * ICON_CELL_H) < cy + ch; i++) {
        int    fx = cx + (int)col * ICON_CELL_W;
        int    fy = cy + (int)row * ICON_CELL_H;
        UINT32 icon_col = snap[i].is_directory ? 0xFFD4A017 : 0xFF4A5568;
        UINT32 name_col = snap[i].is_directory ? C_TEXT_YELLOW : C_TEXT_PRIMARY;

        /* Hover/selected highlight */
        if (i == s_selected_file) {
            gui_fill_rounded(fx + 2, fy + 2, (UINT32)ICON_CELL_W - 4, (UINT32)ICON_CELL_H - 4,
                             C_SIDEBAR_ACTIVE, 3);
            gui_draw_border_rounded(fx + 2, fy + 2, (UINT32)ICON_CELL_W - 4, (UINT32)ICON_CELL_H - 4,
                                    C_SIDEBAR_ACTIVE_BD, 3);
        }

        /* Icon — proper folder or file shape */
        if (snap[i].is_directory) {
            /* Folder: tab + body */
            gui_fill_rounded(fx + (ICON_CELL_W - ICON_IMG_W) / 2,
                             fy + 2 + 4, (UINT32)ICON_IMG_W, (UINT32)(ICON_IMG_H - 4),
                             icon_col, 3);
            gui_fill_rounded(fx + (ICON_CELL_W - ICON_IMG_W) / 2,
                             fy + 2, (UINT32)(ICON_IMG_W / 2), (UINT32)6,
                             0xFFE8C000, 2);
        } else {
            /* File: document with dog-ear */
            int fx2 = fx + (ICON_CELL_W - ICON_IMG_W) / 2;
            int fy2 = fy + 2;
            gui_fill_rounded(fx2, fy2, (UINT32)ICON_IMG_W, (UINT32)ICON_IMG_H, 0xFF2A3A4A, 2);
            gui_draw_border_rounded(fx2, fy2, (UINT32)ICON_IMG_W, (UINT32)ICON_IMG_H, 0xFF4A5A72, 2);
            /* dog-ear */
            gui_fill_rect(fx2 + ICON_IMG_W - 8, fy2,     8, 8, C_WIN_BODY);
            gui_fill_rect(fx2 + ICON_IMG_W - 8, fy2,     8, 1, 0xFF4A5A72);
            gui_fill_rect(fx2 + ICON_IMG_W - 8, fy2,     1, 8, 0xFF4A5A72);
            /* lines */
            gui_fill_rect(fx2 + 4, fy2 + 11, (UINT32)(ICON_IMG_W - 10), 2, 0xFF4A5A72);
            gui_fill_rect(fx2 + 4, fy2 + 17, (UINT32)(ICON_IMG_W - 10), 2, 0xFF4A5A72);
            gui_fill_rect(fx2 + 4, fy2 + 23, (UINT32)(ICON_IMG_W - 14), 2, 0xFF4A5A72);
        }

        /* Name */
        gui_draw_text_n(fx + 2, fy + ICON_IMG_H + 6,
                        snap[i].name, 8, name_col);

        col++;
        if ((int)col * ICON_CELL_W + cx > cx + cw - ICON_CELL_W) {
            col = 0; row++;
        }
        slot++;
    }

    /* Status bar */
    {
        int sy = win->y + (int)win->h - STATUSBAR_H;
        char sbuf[24];
        UINT32 n = 0;
        gui_fill_rect(win->x, sy, win->w, (UINT32)STATUSBAR_H, C_WIN_STATUSBAR);
        gui_fill_rect(win->x, sy, win->w, 1, C_BORDER);
        gui_uint_to_str((UINT32)snap_count, sbuf, 12);
        /* Append " items" */
        while (sbuf[n]) n++;
        sbuf[n++]=' '; sbuf[n++]='i'; sbuf[n++]='t'; sbuf[n++]='e'; sbuf[n++]='m'; sbuf[n++]='s'; sbuf[n]='\0';
        gui_draw_text(win->x + 8, sy + (STATUSBAR_H - FONT_H) / 2, sbuf, C_TEXT_MUTED);
    }
}

static void files_on_key(GUI_WIN *win, UINT8 scancode)
{
    (void)win;
    /* F5 = refresh */
    if (scancode == 0x3F) app_files_refresh();
}

void app_files_handle_click(int mx, int my)
{
    UINT32 i;
    const GUI_WIN *win = gui_wm_get(GUI_WIN_FILES);
    if (!win || win->minimized) return;

    /* Toolbar: Up button */
    if (gui_widget_handle_click(&s_btn_up, mx, my)) {
        /* Navigate up: find last '/' */
        UINT32 len = 0;
        while (s_path[len]) len++;
        if (len > 1) {
            while (len > 0 && s_path[--len] != '/') s_path[len] = '\0';
            if (len == 0) { s_path[0] = '/'; s_path[1] = '\0'; }
            else s_path[len] = '\0';
        }
        app_files_refresh();
        return;
    }
    /* Toolbar: Refresh button */
    if (gui_widget_handle_click(&s_btn_refresh, mx, my)) {
        app_files_refresh();
        return;
    }
    /* Sidebar bookmarks */
    for (i = 0; i < BOOKMARK_COUNT; i++) {
        if (gui_widget_handle_click(&s_bm_btns[i], mx, my)) {
            s_selected_bm = i;
            scopy(s_path, s_bookmarks[i].path, 64);
            app_files_refresh();
            return;
        }
    }
    /* Sidebar DRIVES entries */
    {
        int wy2 = win->y + CHROME_H + TOOLBAR_H;
        int drives_label_y = wy2 + 18 + BOOKMARK_COUNT * (CELL_H + 6) + 8;
        int drives_first_y = drives_label_y + CELL_H + 2;
        int dvol_count = vfs_get_volume_count();
        int di;

        for (di = 0; di < dvol_count; di++) {
            const VFS_VOLUME_INFO *vol = vfs_get_volume(di);
            int dvy = drives_first_y + di * (CELL_H + 6);

            if (!vol || !vol->valid) continue;

            if (mx >= win->x && mx < win->x + SIDEBAR_W
             && my >= dvy - 2 && my < dvy - 2 + CELL_H + 6) {
                s_selected_bm = (UINT32)(BOOKMARK_COUNT + di);
                scopy(s_path, vol->mount_point, 64);
                app_files_refresh();
                return;
            }
        }
    }
    /* File icon grid hit-testing */
    {
        int wy2 = win->y + CHROME_H + TOOLBAR_H + 4;
        int cx  = win->x + SIDEBAR_W + 4;
        int cw  = (int)win->w - SIDEBAR_W - 8;
        UINT32 cols = (UINT32)cw / ICON_CELL_W;
        if (cols == 0) cols = 1;
        VFS_DIRECTORY_ENTRY snap[FILES_MAX];
        UINT64 snap_count;
        files_lock();
        snap_count = s_file_count;
        for (i = 0; i < snap_count; i++) snap[i] = s_files[i];
        files_unlock();
        for (i = 0; i < snap_count; i++) {
            int fx = cx + (int)(i % cols) * ICON_CELL_W;
            int fy = wy2 + (int)(i / cols) * ICON_CELL_H;
            if (mx >= fx && mx < fx + ICON_CELL_W
             && my >= fy && my < fy + ICON_CELL_H) {
                UINT32 now = gui_compositor_loop_ticks();
                s_selected_file = i;
                if (snap[i].is_directory) {
                    /* Navigate into directory */
                    UINT32 len = 0;
                    while (s_path[len]) len++;
                    if (len > 0 && s_path[len-1] != '/') {
                        s_path[len] = '/'; s_path[len+1] = '\0'; len++;
                    }
                    UINT32 ni = 0;
                    while (snap[i].name[ni] && len + ni + 1 < 63) {
                        s_path[len + ni] = snap[i].name[ni]; ni++;
                    }
                    s_path[len + ni] = '\0';
                    app_files_refresh();
                    s_last_click_file = 0xFFFFFFFFu;
                } else {
                    /* Double-click on file → open in Text Editor */
                    if (s_last_click_file == i
                     && now - s_last_click_tick < FILES_DBLCLICK_TICKS) {
                        /* Build full path */
                        char full[64];
                        UINT32 plen = 0, ni = 0;
                        while (s_path[plen] && plen < 62) { full[plen] = s_path[plen]; plen++; }
                        if (plen > 0 && full[plen-1] != '/') { full[plen++] = '/'; }
                        while (snap[i].name[ni] && plen + ni < 63) {
                            full[plen + ni] = snap[i].name[ni]; ni++;
                        }
                        full[plen + ni] = '\0';
                        if (file_is_virtual_disk_image(snap[i].name)) {
                            (void)files_attach_virtual_disk(full);
                        } else {
                            app_editor_open_file(full);
                        }
                        s_last_click_file = 0xFFFFFFFFu;
                    } else {
                        s_last_click_file = i;
                        s_last_click_tick = now;
                    }
                }
                return;
            }
        }
    }
}

void app_files_update_hover(int mx, int my)
{
    UINT32 i;
    gui_widget_update_hover(&s_btn_up,      mx, my);
    gui_widget_update_hover(&s_btn_refresh, mx, my);
    for (i = 0; i < BOOKMARK_COUNT; i++)
        gui_widget_update_hover(&s_bm_btns[i], mx, my);
}

void app_files_initialize(void)
{
    UINT32 i;
    s_file_count  = 0;
    s_selected_bm = 0;
    s_selected_file = 0xFFFF;
    s_lock = 0;
    scopy(s_path, "/", 64);

    /* Toolbar buttons */
    gui_widget_button_flat(&s_btn_up,      0, 0, 20, 18, "^");
    gui_widget_button_flat(&s_btn_refresh, 0, 0, 20, 18, "R");
    s_btn_up.style.corner_r = s_btn_refresh.style.corner_r = 2;

    /* Sidebar bookmark buttons — transparent flat buttons */
    for (i = 0; i < BOOKMARK_COUNT; i++) {
        gui_widget_button_flat(&s_bm_btns[i], 0, 0,
                               (UINT32)(SIDEBAR_W - 4), (UINT32)(CELL_H + 4),
                               s_bookmarks[i].label);
        s_bm_btns[i].style.bg       = 0;      /* transparent */
        s_bm_btns[i].style.bg_hover = C_SIDEBAR_ACTIVE;
        s_bm_btns[i].style.border   = 0;
        s_bm_btns[i].style.fg       = C_TEXT_MUTED;
        /* Move text right to leave room for the colored dot */
    }

    gui_wm_set_callbacks(GUI_WIN_FILES, files_paint, files_on_key);
    app_files_refresh();
}
