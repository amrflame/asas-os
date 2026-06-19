/*
 * gui_app_diskmgmt.c - Disk Management application.
 */

#include "gui_wm.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_widget.h"
#include "block_device.h"
#include "partition.h"
#include "filesystem.h"
#include "vfs.h"
#include "virtual_disk.h"
#include "disk_management.h"
#include "logger.h"

#define DISK_BTN_COUNT 13U

static UINT32 s_selected_device;
static UINT32 s_selected_partition;
static UINT32 s_status_color;
static char s_status[80];
static GUI_WIDGET s_buttons[DISK_BTN_COUNT];

static void dm_copy(char *dst, const char *src, UINT32 capacity)
{
    UINT32 i = 0;
    if (dst == 0 || src == 0 || capacity == 0) return;
    while (src[i] != '\0' && i + 1U < capacity) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int dm_equal(const char *left, const char *right)
{
    if (left == 0 || right == 0) return 0;
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) return 0;
        left++;
        right++;
    }
    return *left == *right;
}

static void dm_set_status(const char *text, UINT32 color)
{
    dm_copy(s_status, text, sizeof(s_status));
    s_status_color = color;
}

static void dm_u64_to_str(UINT64 value, char *buffer, UINT32 capacity)
{
    char temp[24];
    UINT32 count = 0;
    UINT32 out = 0;
    if (capacity == 0) return;
    if (value == 0) {
        if (capacity > 1) {
            buffer[0] = '0';
            buffer[1] = '\0';
        } else buffer[0] = '\0';
        return;
    }
    while (value != 0 && count < sizeof(temp)) {
        temp[count++] = (char)('0' + (char)(value % 10U));
        value /= 10U;
    }
    while (count != 0 && out + 1U < capacity) {
        buffer[out++] = temp[--count];
    }
    buffer[out] = '\0';
}

static void dm_size_text(UINT64 blocks, UINT32 block_size,
                         char *buffer, UINT32 capacity)
{
    UINT64 bytes = blocks * (UINT64)block_size;
    UINT64 value = bytes;
    const char *unit = " B";
    UINT32 len = 0;
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        value = bytes / (1024ULL * 1024ULL * 1024ULL);
        unit = " GB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        value = bytes / (1024ULL * 1024ULL);
        unit = " MB";
    } else if (bytes >= 1024ULL) {
        value = bytes / 1024ULL;
        unit = " KB";
    }
    dm_u64_to_str(value, buffer, capacity);
    while (buffer[len] != '\0' && len + 1U < capacity) len++;
    while (*unit != '\0' && len + 1U < capacity) buffer[len++] = *unit++;
    buffer[len] = '\0';
}

static ASAS_BLOCK_DEVICE *dm_selected_device(void)
{
    if (s_selected_device >= block_device_count()) {
        s_selected_device = 0;
    }
    return block_device_get(s_selected_device);
}

static const ASAS_PARTITION_INFO *dm_partition_for_selected(UINT32 local_index)
{
    ASAS_BLOCK_DEVICE *disk = dm_selected_device();
    UINT32 index;
    UINT32 seen = 0;
    if (disk == 0) return 0;
    for (index = 0; index < partition_count(); index++) {
        const ASAS_PARTITION_INFO *partition = partition_get(index);
        if (partition == 0 || partition->parent != disk) continue;
        if (seen == local_index) return partition;
        seen++;
    }
    return 0;
}

static const ASAS_PARTITION_INFO *dm_active_partition(void)
{
    const ASAS_PARTITION_INFO *partition =
        dm_partition_for_selected(s_selected_partition);
    if (partition != 0) return partition;
    s_selected_partition = 0;
    return dm_partition_for_selected(0);
}

static const VFS_VOLUME_INFO *dm_volume_for_device(const char *name)
{
    int index;
    for (index = 0; index < vfs_get_volume_count(); index++) {
        const VFS_VOLUME_INFO *volume = vfs_get_volume(index);
        if (volume != 0 && volume->valid &&
            dm_equal(volume->device_name, name)) return volume;
    }
    return 0;
}

static int dm_is_system_volume(const char *device_name)
{
    const VFS_VOLUME_INFO *volume = dm_volume_for_device(device_name);
    return volume != 0 && dm_equal(volume->mount_point, "/system");
}

static ASAS_BLOCK_DEVICE *dm_operation_device(void)
{
    const ASAS_PARTITION_INFO *partition = dm_active_partition();
    if (partition != 0) return partition->device;
    return dm_selected_device();
}

static void dm_draw_row(int x, int y, UINT32 w, const char *left,
                        const char *right, int active)
{
    gui_fill_rect(x, y, w, (UINT32)(CELL_H + 7),
                  active ? C_SIDEBAR_ACTIVE : C_WIN_BODY);
    gui_draw_text(x + 6, y + 4, left, C_TEXT_PRIMARY);
    if (right != 0) gui_draw_text_right(x, y + 4, w - 6, right, C_TEXT_MUTED);
}

static void dm_draw_partition_bar(int x, int y, UINT32 w, UINT32 h)
{
    ASAS_BLOCK_DEVICE *disk = dm_selected_device();
    UINT32 index;
    UINT32 seen = 0;
    if (disk == 0 || disk->block_count == 0) {
        gui_fill_rect(x, y, w, h, C_DIVIDER);
        return;
    }
    gui_fill_rect(x, y, w, h, C_DIVIDER);
    for (index = 0; index < partition_count(); index++) {
        const ASAS_PARTITION_INFO *partition = partition_get(index);
        UINT32 px;
        UINT32 pw;
        UINT32 color;
        if (partition == 0 || partition->parent != disk) continue;
        px = x + (UINT32)(partition->start_lba * (UINT64)w / disk->block_count);
        pw = (UINT32)(partition->block_count * (UINT64)w / disk->block_count);
        if (pw < 3U) pw = 3U;
        color = seen == s_selected_partition ? C_ACCENT : 0xFF2A6F5F;
        gui_fill_rect((int)px, y, pw, h, color);
        gui_draw_border((int)px, y, pw, h, C_BORDER);
        seen++;
    }
}

static void dm_button(UINT32 index, int x, int y, UINT32 w, const char *label,
                      int danger)
{
    if (danger) gui_widget_button_danger(&s_buttons[index], x, y, w, 22, label);
    else gui_widget_button_flat(&s_buttons[index], x, y, w, 22, label);
    gui_widget_render(&s_buttons[index]);
}

static void diskmgmt_paint(GUI_WIN *win)
{
    int wx = win->x;
    int wy = win->y + CHROME_H;
    int ww = (int)win->w;
    int wh = (int)win->h - CHROME_H;
    int left_w = 150;
    int right_w = 170;
    int cx = wx + left_w;
    int cw = ww - left_w - right_w;
    int rx = wx + ww - right_w;
    int y;
    UINT32 index;
    ASAS_BLOCK_DEVICE *disk = dm_selected_device();
    const ASAS_PARTITION_INFO *active_partition = dm_active_partition();
    ASAS_BLOCK_DEVICE *target = dm_operation_device();
    const VFS_VOLUME_INFO *volume = target != 0 ? dm_volume_for_device(target->name) : 0;

    gui_fill_rect(wx, wy, (UINT32)ww, (UINT32)wh, C_WIN_BODY);
    if (ww < 430 || wh < 260) {
        gui_draw_text(wx + 12, wy + 14, "Resize window to use Disk Manager",
                      C_TEXT_YELLOW);
        return;
    }
    gui_fill_rect(wx, wy, (UINT32)left_w, (UINT32)wh, C_SIDEBAR_BG);
    gui_fill_rect(rx, wy, (UINT32)right_w, (UINT32)wh, C_SIDEBAR_BG);
    gui_fill_rect(wx + left_w, wy, 1, (UINT32)wh, C_BORDER);
    gui_fill_rect(rx, wy, 1, (UINT32)wh, C_BORDER);

    y = wy + 10;
    gui_draw_text(wx + 10, y, "DISKS", C_TEXT_LABEL);
    y += CELL_H + 6;
    for (index = 0; index < block_device_count() && y + 22 < wy + wh; index++) {
        ASAS_BLOCK_DEVICE *device = block_device_get(index);
        char size[24];
        if (device == 0 || device->parent != 0) continue;
        dm_size_text(device->block_count, device->logical_block_size,
                     size, sizeof(size));
        dm_draw_row(wx + 6, y, (UINT32)(left_w - 12), device->name, size,
                    index == s_selected_device);
        y += CELL_H + 9;
    }
    y += 6;
    gui_draw_text(wx + 10, y, "VIRTUAL", C_TEXT_LABEL);
    y += CELL_H + 5;
    {
        UINT32 count = virtual_disk_count();
        char count_text[16];
        gui_uint_to_str(count, count_text, sizeof(count_text));
        gui_draw_text(wx + 10, y, "Attached", C_TEXT_PRIMARY);
        gui_draw_text_right(wx, y, (UINT32)left_w - 10, count_text, C_TEXT_MUTED);
    }

    y = wy + 10;
    gui_draw_text(cx + 12, y, "PARTITION MAP", C_TEXT_LABEL);
    y += CELL_H + 8;
    dm_draw_partition_bar(cx + 12, y, (UINT32)(cw - 24), 24);
    y += 36;

    gui_draw_text(cx + 12, y, "PARTITIONS", C_TEXT_LABEL);
    y += CELL_H + 6;
    {
        UINT32 seen = 0;
        for (index = 0; index < partition_count() && y + 24 < wy + wh - 34; index++) {
            const ASAS_PARTITION_INFO *partition = partition_get(index);
            char size[24];
            if (partition == 0 || disk == 0 || partition->parent != disk) continue;
            dm_size_text(partition->block_count,
                         partition->device->logical_block_size,
                         size, sizeof(size));
            dm_draw_row(cx + 12, y, (UINT32)(cw - 24), partition->device->name,
                        size, seen == s_selected_partition);
            y += CELL_H + 9;
            gui_draw_text(cx + 20, y, partition->type_name, C_TEXT_MUTED);
            if (partition->uuid[0] != '\0')
                gui_draw_text_right(cx, y, (UINT32)cw - 20,
                                    partition->uuid, C_TEXT_MUTED);
            y += CELL_H + 5;
            seen++;
        }
        if (seen == 0) {
            gui_draw_text(cx + 12, y, "No partition table detected", C_TEXT_MUTED);
        }
    }

    y = wy + wh - 24;
    gui_fill_rect(cx, y - 4, (UINT32)cw, 1, C_BORDER);
    gui_draw_text(cx + 12, y, s_status, s_status_color);

    y = wy + 10;
    gui_draw_text(rx + 10, y, "DETAILS", C_TEXT_LABEL);
    y += CELL_H + 8;
    if (target != 0) {
        char size[24];
        dm_size_text(target->block_count, target->logical_block_size,
                     size, sizeof(size));
        gui_draw_text(rx + 10, y, target->name, C_TEXT_PRIMARY);
        gui_draw_text_right(rx, y, (UINT32)right_w - 10, size, C_TEXT_MUTED);
        y += CELL_H + 5;
        gui_draw_text(rx + 10, y, "Filesystem", C_TEXT_MUTED);
        if (volume != 0) {
            char fs[16];
            gui_uint_to_str(volume->fs_type, fs, sizeof(fs));
            gui_draw_text_right(rx, y, (UINT32)right_w - 10, fs, C_TEXT_PRIMARY);
        } else {
            gui_draw_text_right(rx, y, (UINT32)right_w - 10, "unmounted", C_TEXT_PRIMARY);
        }
        y += CELL_H + 5;
        gui_draw_text(rx + 10, y, "Free space", C_TEXT_MUTED);
        gui_draw_text_right(rx, y, (UINT32)right_w - 10, "n/a", C_TEXT_PRIMARY);
        y += CELL_H + 5;
        gui_draw_text(rx + 10, y, "Read only", C_TEXT_MUTED);
        gui_draw_text_right(rx, y, (UINT32)right_w - 10,
            (target->flags & BLOCK_DEVICE_FLAG_READ_ONLY) != 0 ? "yes" : "no",
            (target->flags & BLOCK_DEVICE_FLAG_READ_ONLY) != 0 ? C_TEXT_RED : C_TEXT_GREEN);
        y += CELL_H + 5;
        gui_draw_text_n(rx + 10, y, disk_management_read_only_reason(target->name),
                        25,
                        (target->flags & BLOCK_DEVICE_FLAG_READ_ONLY) != 0 ?
                        C_TEXT_YELLOW : C_TEXT_MUTED);
        y += CELL_H + 5;
        gui_draw_text(rx + 10, y, "Removable", C_TEXT_MUTED);
        gui_draw_text_right(rx, y, (UINT32)right_w - 10,
            (target->flags & BLOCK_DEVICE_FLAG_REMOVABLE) != 0 ? "yes" : "no",
            C_TEXT_PRIMARY);
        y += CELL_H + 5;
        gui_draw_text(rx + 10, y, "Health", C_TEXT_MUTED);
        gui_draw_text_right(rx, y, (UINT32)right_w - 10, "ok", C_TEXT_GREEN);
        y += CELL_H + 8;
        if (active_partition != 0 && active_partition->label[0] != '\0') {
            gui_draw_text(rx + 10, y, active_partition->label, C_TEXT_PRIMARY);
            y += CELL_H + 5;
        }
    }

    gui_fill_rect(rx + 8, y, (UINT32)(right_w - 16), 1, C_BORDER);
    y += 8;
    dm_button(0, rx + 10, y, 70, "Mount", 0);
    dm_button(1, rx + 88, y, 72, "Unmount", 0);
    y += 28;
    dm_button(2, rx + 10, y, 46, "RW", 0);
    dm_button(3, rx + 62, y, 46, "RO", 0);
    dm_button(4, rx + 114, y, 46, "NoExec", 0);
    y += 30;
    dm_button(5, rx + 10, y, 70, "Check", 0);
    dm_button(6, rx + 88, y, 72, "Repair", 0);
    y += 30;
    dm_button(7, rx + 10, y, 70, "Format", 1);
    dm_button(8, rx + 88, y, 72, "Delete", 1);
    y += 30;
    dm_button(9, rx + 10, y, 70, "Create", 0);
    dm_button(10, rx + 88, y, 72, "Resize", 0);
    y += 30;
    dm_button(11, rx + 10, y, 70, "VD Check", 0);
    dm_button(12, rx + 88, y, 72, "Detach", 0);
}

void app_diskmgmt_handle_click(int mx, int my)
{
    const GUI_WIN *win = gui_wm_get(GUI_WIN_DISKMGMT);
    int wx, wy, wh, left_w, right_w, cx, cw, rx;
    int y;
    UINT32 index;
    ASAS_BLOCK_DEVICE *target;
    const VFS_VOLUME_INFO *volume;
    if (win == 0 || win->minimized) return;
    wx = win->x;
    wy = win->y + CHROME_H;
    wh = (int)win->h - CHROME_H;
    left_w = 150;
    right_w = 170;
    cx = wx + left_w;
    cw = (int)win->w - left_w - right_w;
    rx = wx + (int)win->w - right_w;
    if ((int)win->w < 430 || wh < 260) return;

    y = wy + 10 + CELL_H + 6;
    for (index = 0; index < block_device_count(); index++) {
        ASAS_BLOCK_DEVICE *device = block_device_get(index);
        if (device == 0 || device->parent != 0) continue;
        if (mx >= wx + 6 && mx < wx + left_w - 6 &&
            my >= y && my < y + CELL_H + 7) {
            s_selected_device = index;
            s_selected_partition = 0;
            dm_set_status("Disk selected", C_TEXT_GREEN);
            return;
        }
        y += CELL_H + 9;
    }

    y = wy + 10 + CELL_H + 8 + 36 + CELL_H + 6;
    {
        UINT32 seen = 0;
        ASAS_BLOCK_DEVICE *disk = dm_selected_device();
        for (index = 0; index < partition_count(); index++) {
            const ASAS_PARTITION_INFO *partition = partition_get(index);
            if (partition == 0 || disk == 0 || partition->parent != disk) continue;
            if (mx >= cx + 12 && mx < cx + cw - 12 &&
                my >= y && my < y + CELL_H + 7) {
                s_selected_partition = seen;
                dm_set_status("Partition selected", C_TEXT_GREEN);
                return;
            }
            y += CELL_H + 9 + CELL_H + 5;
            seen++;
        }
    }

    if (mx < rx || mx >= rx + right_w) return;
    target = dm_operation_device();
    volume = target != 0 ? dm_volume_for_device(target->name) : 0;
    if (target == 0) return;

    if (gui_widget_handle_click(&s_buttons[0], mx, my)) {
        if (volume != 0) dm_set_status("Already mounted", C_TEXT_YELLOW);
        else if (disk_management_mount(target->name, "/data", 0))
            dm_set_status("Mounted at /data", C_TEXT_GREEN);
        else dm_set_status("Mount failed", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[1], mx, my)) {
        if (volume == 0) dm_set_status("Not mounted", C_TEXT_YELLOW);
        else if (dm_equal(volume->mount_point, "/system"))
            dm_set_status("System volume is locked", C_TEXT_RED);
        else if (disk_management_unmount(volume->mount_point))
            dm_set_status("Unmounted", C_TEXT_GREEN);
        else dm_set_status("Unmount failed", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[2], mx, my)) {
        if (volume != 0 && disk_management_remount(volume->mount_point, 0))
            dm_set_status("Remounted read-write", C_TEXT_GREEN);
        else dm_set_status("Remount failed", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[3], mx, my)) {
        if (volume != 0 &&
            disk_management_remount(volume->mount_point, FILESYSTEM_FLAG_READ_ONLY))
            dm_set_status("Safe read-only mode", C_TEXT_YELLOW);
        else dm_set_status("Remount failed", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[4], mx, my)) {
        if (volume != 0 &&
            disk_management_remount(volume->mount_point, FILESYSTEM_FLAG_NO_EXEC))
            dm_set_status("No-exec policy applied", C_TEXT_GREEN);
        else dm_set_status("No-exec failed", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[5], mx, my)) {
        if (volume != 0 && disk_management_fs_check(volume->mount_point))
            dm_set_status("Filesystem check passed", C_TEXT_GREEN);
        else dm_set_status("Check failed", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[6], mx, my)) {
        if (volume != 0 && disk_management_fs_repair(volume->mount_point, 1))
            dm_set_status("Repair dry-run ok", C_TEXT_YELLOW);
        else dm_set_status("Repair unavailable", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[7], mx, my)) {
        if (dm_is_system_volume(target->name))
            dm_set_status("Format blocked on system volume", C_TEXT_RED);
        else if (disk_management_format(target->name, "fat32", 1))
            dm_set_status("Format dry-run ok", C_TEXT_YELLOW);
        else dm_set_status("Format blocked", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[8], mx, my)) {
        if (dm_is_system_volume(target->name))
            dm_set_status("Delete blocked on system volume", C_TEXT_RED);
        else dm_set_status("Delete requires shell confirm", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[9], mx, my)) {
        ASAS_BLOCK_DEVICE *disk = dm_selected_device();
        if (disk != 0 &&
            disk_management_partition_mbr("create", disk->name, 0, 0x0c,
                                          2048, 4096, 1))
            dm_set_status("Create partition dry-run ok", C_TEXT_YELLOW);
        else dm_set_status("Create blocked", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[10], mx, my)) {
        ASAS_BLOCK_DEVICE *disk = dm_selected_device();
        if (disk != 0 &&
            disk_management_partition_mbr("resize", disk->name, 0, 0x0c,
                                          2048, 8192, 1))
            dm_set_status("Resize dry-run ok", C_TEXT_YELLOW);
        else dm_set_status("Resize blocked", C_TEXT_RED);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[11], mx, my)) {
        const ASAS_VDISK_INFO *info = virtual_disk_get(0);
        if (info != 0 && virtual_disk_check(info->name))
            dm_set_status("Virtual disk check ok", C_TEXT_GREEN);
        else dm_set_status("No virtual disk to check", C_TEXT_YELLOW);
        return;
    }
    if (gui_widget_handle_click(&s_buttons[12], mx, my)) {
        const ASAS_VDISK_INFO *info = virtual_disk_get(0);
        if (info != 0 && virtual_disk_detach(info->name))
            dm_set_status("Virtual disk detached", C_TEXT_GREEN);
        else dm_set_status("Detach blocked; unmount first", C_TEXT_RED);
        return;
    }
    (void)wh;
}

void app_diskmgmt_update_hover(int mx, int my)
{
    UINT32 index;
    for (index = 0; index < DISK_BTN_COUNT; index++)
        gui_widget_update_hover(&s_buttons[index], mx, my);
}

void app_diskmgmt_initialize(void)
{
    UINT32 index;
    s_selected_device = 0;
    s_selected_partition = 0;
    dm_set_status("Ready", C_TEXT_GREEN);
    for (index = 0; index < DISK_BTN_COUNT; index++)
        gui_widget_button_flat(&s_buttons[index], 0, 0, 1, 1, "");
    gui_wm_set_callbacks(GUI_WIN_DISKMGMT, diskmgmt_paint, 0);
}
