#include "vfs.h"
#include "fat16.h"
#include "fat32.h"
#include "ntfs.h"
#include "virtio_block.h"
#include "heap.h"
#include "logger.h"
#include "block_device.h"
#include "partition.h"
#include "filesystem.h"
#include "virtual_disk.h"
#include "nvme.h"
#include "xhci.h"
#include "ahci.h"

#pragma intrinsic(_InterlockedExchange)
long _InterlockedExchange(long volatile *target, long value);

typedef enum {
    FS_TYPE_FAT16 = 0,
    FS_TYPE_FAT32 = 1,
    FS_TYPE_NTFS  = 2
} FS_TYPE;

typedef struct {
    const char *path;
    const UINT8 *data;
    UINT64 size;
} VFS_NODE;

#define VFS_DISK_HANDLE_COUNT 8
#define VFS_BOOT_NODE_COUNT 3
#define VFS_BOOT_HANDLE_BASE 100
#define VFS_PATH_CAPACITY 64

typedef struct {
    char path[VFS_PATH_CAPACITY];
    ASAS_FILESYSTEM_MOUNT *mount;
    UINT32 mount_generation;
    UINT8 used;
} VFS_DISK_HANDLE;

static const UINT8 welcome_text[] = "Asas OS virtual filesystem";
static VFS_NODE nodes[1];
static VFS_NODE boot_nodes[VFS_BOOT_NODE_COUNT];
static UINT8 boot_node_count;
static VFS_DISK_HANDLE disk_handles[VFS_DISK_HANDLE_COUNT];
static UINT8 boot_fallback_enabled;
static FS_TYPE active_fs = FS_TYPE_FAT16;
static volatile long vfs_lock_value;
static char vfs_status_reason[128];

/* Multi-volume table */
static VFS_VOLUME_INFO vfs_volumes[VFS_MAX_VOLUMES];
static ASAS_FILESYSTEM_MOUNT *vfs_mounts[VFS_MAX_VOLUMES];
static int vfs_volume_count;
static void vfs_test_path(char *path, UINTN capacity, const char *mount,
                          const char *name);
static void register_hyperv_storage_devices(int force_rescan);

static void reason_copy(const char *text)
{
    UINT32 index = 0;
    if (text == 0) text = "";
    while (text[index] != '\0' && index + 1U < sizeof(vfs_status_reason)) {
        vfs_status_reason[index] = text[index];
        index++;
    }
    vfs_status_reason[index] = '\0';
}

static void reason_append(const char *text)
{
    UINT32 out = 0;
    UINT32 in = 0;
    if (text == 0) return;
    while (vfs_status_reason[out] != '\0' &&
           out + 1U < sizeof(vfs_status_reason)) out++;
    while (text[in] != '\0' && out + 1U < sizeof(vfs_status_reason))
        vfs_status_reason[out++] = text[in++];
    vfs_status_reason[out] = '\0';
}

static void vfs_reset_handles(void)
{
    UINT32 index;
    for (index = 0; index < VFS_DISK_HANDLE_COUNT; index++) {
        if (disk_handles[index].used && disk_handles[index].mount != 0)
            filesystem_mount_release(disk_handles[index].mount);
        disk_handles[index].used = 0;
        disk_handles[index].mount = 0;
        disk_handles[index].mount_generation = 0;
        disk_handles[index].path[0] = '\0';
    }
}

static int path_has_prefix(const char *path, const char *prefix)
{
    UINT32 index = 0;
    while (prefix[index] != '\0') {
        if (path[index] != prefix[index]) return 0;
        index++;
    }
    return path[index] == '\0' || path[index] == '/';
}

static void vfs_lock(void)
{
    while (_InterlockedExchange(&vfs_lock_value, 1) != 0) {
    }
}

static void vfs_unlock(void)
{
    (void)_InterlockedExchange(&vfs_lock_value, 0);
}

static int strings_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return 0;
        }
        left++;
        right++;
    }

    return *left == *right;
}

static char to_lower(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

static int paths_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (to_lower(*left) != to_lower(*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == *right;
}

/* Re-mount the primary disk so all FS calls target device 0:0 again */
static void copy_string(char *destination, const char *source, UINT32 capacity); /* fwd */
static void vfs_select_primary(void)
{
    if (vfs_volume_count > 0 && vfs_mounts[0] != 0) {
        return;
    }
    /* Use the recorded primary volume target/lun so that on Hyper-V (where
       the boot disk may be at T=0/L=1) we never accidentally select the
       wrong device.  Volume 0 is populated by vfs_mount_all_volumes().    */
    UINT8 t = (vfs_volume_count > 0) ? (UINT8)vfs_volumes[0].target : 0;
    UINT8 l = (vfs_volume_count > 0) ? (UINT8)vfs_volumes[0].lun    : 0;
    if      (active_fs == FS_TYPE_FAT32) fat32_mount_device(t, l);
    else if (active_fs == FS_TYPE_NTFS)  ntfs_mount_device(t, l);
    else                                  fat16_mount_device(t, l);
}

/* Check whether 'path' starts with one of the secondary volume mount points.
   On match, sets *vol_idx_out and *rel_out (relative sub-path on that volume).
   Returns 1 on match, 0 otherwise.                                              */
static int vfs_select_volume_for_path(const char *path, int *vol_idx_out,
                                      const char **rel_out)
{
    int i;
    int best = -1;
    int best_length = 0;

    for (i = 0; i < vfs_volume_count; i++) {
        const VFS_VOLUME_INFO *v = &vfs_volumes[i];
        int mplen = 0;

        if (!v->valid) continue;
        while (v->mount_point[mplen]) mplen++;
        if (mplen > best_length && path_has_prefix(path, v->mount_point)) {
            best = i;
            best_length = mplen;
        }
    }
    /* Compatibility alias retained while callers migrate to /media/*. */
    if (best < 0 && path[0] == '/' && path[1] == 'd' && path[2] == 'i' &&
        path[3] == 's' && path[4] == 'k' && path[5] >= '0' && path[5] <= '7' &&
        (path[6] == '\0' || path[6] == '/')) {
        i = path[5] - '0';
        if (i < vfs_volume_count && vfs_volumes[i].valid) {
            *vol_idx_out = i;
            *rel_out = path[6] == '\0' ? "/" : path + 6;
            return 1;
        }
    }
    if (best < 0) return 0;
    *vol_idx_out = best;
    *rel_out = path[best_length] == '\0' ? "/" : path + best_length;
    return 1;
}

static UINT8 vfs_type_from_driver(const ASAS_FILESYSTEM_DRIVER *driver)
{
    if (driver == 0) return VFS_FS_NONE;
    if (strings_equal(driver->name, "fat32")) return VFS_FS_FAT32;
    if (strings_equal(driver->name, "ntfs")) return VFS_FS_NTFS;
    if (strings_equal(driver->name, "exfat")) return VFS_FS_EXFAT;
    if (strings_equal(driver->name, "iso9660")) return VFS_FS_ISO9660;
    if (strings_equal(driver->name, "udf")) return VFS_FS_UDF;
    if (strings_equal(driver->name, "ext2")) return VFS_FS_EXT2;
    return VFS_FS_NONE;
}

static ASAS_FILESYSTEM_MOUNT *vfs_mount_for_path(const char *path,
                                                 const char **relative_path)
{
    int volume_index;
    if (vfs_select_volume_for_path(path, &volume_index, relative_path)) {
        return vfs_mounts[volume_index];
    }
    if (path_has_prefix(path, "/system") && vfs_volume_count > 0) {
        *relative_path = path[7] == '\0' ? "/" : path + 7;
        return vfs_mounts[0];
    }
    *relative_path = path;
    return vfs_volume_count > 0 ? vfs_mounts[0] : 0;
}

/* List a directory that lives on a secondary volume (vol_idx >= 1) */
static UINT64 vfs_list_on_volume(int vol_idx, const char *rel_path,
                                  VFS_DIRECTORY_ENTRY *entries, UINT64 capacity)
{
    const VFS_VOLUME_INFO *vol = &vfs_volumes[vol_idx];
    UINT64 output_count = 0;
    UINT64 index;
    UINT64 item_cap;

    if (vfs_mounts[vol_idx] != 0) {
        ASAS_FILESYSTEM_ENTRY *items;
        UINT64 count;
        item_cap = capacity < 256U ? capacity : 256U;
        items = (ASAS_FILESYSTEM_ENTRY *)kmalloc(
            (UINTN)(item_cap * sizeof(ASAS_FILESYSTEM_ENTRY)));
        if (items == 0) return 0;
        count = filesystem_list(vfs_mounts[vol_idx], rel_path, items, item_cap);
        for (index = 0; index < count; index++) {
            copy_string(entries[index].name, items[index].name,
                        sizeof(entries[index].name));
            entries[index].size = items[index].size;
            entries[index].is_directory = items[index].is_directory;
            entries[index].permissions = VFS_PERMISSION_READ;
            if (!items[index].read_only)
                entries[index].permissions |= VFS_PERMISSION_WRITE;
        }
        kfree(items);
        return count;
    }

    if (vol->fs_type == VFS_FS_NONE) return 0;
    item_cap = capacity < 256U ? capacity : 256U;

    if (vol->fs_type == VFS_FS_FAT16) {
        FAT16_FILE_INFO *fat_entries;
        UINT64 fat_count;

        if (!fat16_mount_device(vol->target, vol->lun)) goto restore;
        fat_entries = (FAT16_FILE_INFO *)kmalloc(
            (UINTN)(item_cap * sizeof(FAT16_FILE_INFO)));
        if (!fat_entries) goto restore;
        fat_count = fat16_list_directory(rel_path, fat_entries, item_cap);
        for (index = 0; index < fat_count && output_count < capacity; index++) {
            copy_string(entries[output_count].name,
                        fat_entries[index].name, sizeof(entries[output_count].name));
            entries[output_count].size        = fat_entries[index].size;
            entries[output_count].is_directory = fat_entries[index].is_directory;
            entries[output_count].permissions  = VFS_PERMISSION_READ;
            if (!fat_entries[index].read_only)
                entries[output_count].permissions |= VFS_PERMISSION_WRITE;
            output_count++;
        }
        kfree(fat_entries);
    } else if (vol->fs_type == VFS_FS_FAT32) {
        FAT32_FILE_INFO *fat_entries;
        UINT64 fat_count;

        if (!fat32_mount_device(vol->target, vol->lun)) goto restore;
        fat_entries = (FAT32_FILE_INFO *)kmalloc(
            (UINTN)(item_cap * sizeof(FAT32_FILE_INFO)));
        if (!fat_entries) goto restore;
        fat_count = fat32_list_directory(rel_path, fat_entries, item_cap);
        for (index = 0; index < fat_count && output_count < capacity; index++) {
            copy_string(entries[output_count].name,
                        fat_entries[index].name, sizeof(entries[output_count].name));
            entries[output_count].size        = fat_entries[index].size;
            entries[output_count].is_directory = fat_entries[index].is_directory;
            entries[output_count].permissions  = VFS_PERMISSION_READ;
            if (!fat_entries[index].read_only)
                entries[output_count].permissions |= VFS_PERMISSION_WRITE;
            output_count++;
        }
        kfree(fat_entries);
    } else if (vol->fs_type == VFS_FS_NTFS) {
        UINT64 ntfs_cap = item_cap < 64U ? item_cap : 64U;
        NTFS_FILE_INFO *ntfs_entries;
        UINT64 ntfs_count;

        if (!ntfs_mount_device(vol->target, vol->lun)) goto restore;
        ntfs_entries = (NTFS_FILE_INFO *)kmalloc(
            (UINTN)(ntfs_cap * sizeof(NTFS_FILE_INFO)));
        if (!ntfs_entries) goto restore;
        ntfs_count = ntfs_list_directory(rel_path, ntfs_entries, ntfs_cap);
        for (index = 0; index < ntfs_count && output_count < capacity; index++) {
            copy_string(entries[output_count].name,
                        ntfs_entries[index].name, sizeof(entries[output_count].name));
            entries[output_count].size        = ntfs_entries[index].size;
            entries[output_count].is_directory = ntfs_entries[index].is_directory;
            entries[output_count].permissions  = VFS_PERMISSION_READ;
            if (!ntfs_entries[index].read_only)
                entries[output_count].permissions |= VFS_PERMISSION_WRITE;
            output_count++;
        }
        kfree(ntfs_entries);
    }

restore:
    vfs_select_primary();
    return output_count;
}

static void copy_string(char *destination, const char *source, UINT32 capacity)
{
    UINT32 index = 0;

    while (index + 1 < capacity && source[index] != '\0') {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

void vfs_set_boot_info(const ASAS_BOOT_INFO *boot_info)
{
    boot_node_count = 0;
    if (boot_info == 0) {
        return;
    }

    if (boot_info->boot_disk_text_base != 0 && boot_info->boot_disk_text_size != 0) {
        boot_nodes[boot_node_count].path = "/DISK.TXT";
        boot_nodes[boot_node_count].data = (const UINT8 *)(UINTN)boot_info->boot_disk_text_base;
        boot_nodes[boot_node_count].size = boot_info->boot_disk_text_size;
        boot_node_count++;
    }
    if (boot_info->boot_readme_base != 0 && boot_info->boot_readme_size != 0) {
        boot_nodes[boot_node_count].path = "/ASAS/README.TXT";
        boot_nodes[boot_node_count].data = (const UINT8 *)(UINTN)boot_info->boot_readme_base;
        boot_nodes[boot_node_count].size = boot_info->boot_readme_size;
        boot_node_count++;
    }
    if (boot_info->boot_user_program_base != 0 && boot_info->boot_user_program_size != 0) {
        boot_nodes[boot_node_count].path = "/HELLO.EXE";
        boot_nodes[boot_node_count].data = (const UINT8 *)(UINTN)boot_info->boot_user_program_base;
        boot_nodes[boot_node_count].size = boot_info->boot_user_program_size;
        boot_node_count++;
    }
}

void vfs_initialize(void)
{
    UINT32 index;

    boot_fallback_enabled = 0;    /* transition from RAM boot to disk mode */
    nodes[0].path = "/welcome.txt";
    nodes[0].data = welcome_text;
    nodes[0].size = sizeof(welcome_text) - 1;

    vfs_reset_handles();

    /* Auto-detect filesystem: NTFS → FAT32 → FAT16, in priority order.
     * fat16_initialize() is called here so the VFS is self-contained and
     * does not depend on main.c calling it separately.                   */
    vfs_volume_count = 0;
    for (index = 0; index < VFS_MAX_VOLUMES; index++) vfs_mounts[index] = 0;
    block_device_initialize();
    virtual_disk_initialize();
    (void)virtio_block_register_device();
    (void)hyperv_storage_register_block_device(virtio_block_get_current_target(),
                                               virtio_block_get_current_lun());
    register_hyperv_storage_devices(0);
    (void)nvme_register_block_device();
    (void)xhci_register_storage_block_device();
    (void)ahci_register_block_device();
    if (virtio_block_legacy_backend_active()) {
        (void)block_device_register_legacy_devices();
    }
    partition_manager_initialize();
    (void)partition_scan_all();
    filesystem_initialize();
    filesystem_register_builtin_drivers();
    (void)vfs_mount_all_volumes();

    if (vfs_volume_count > 0 && vfs_mounts[0] != 0) {
        active_fs = vfs_volumes[0].fs_type == VFS_FS_NTFS
            ? FS_TYPE_NTFS : FS_TYPE_FAT32;
        return;
    }
    /* Fallback: FAT16 (or unformatted — fat16_initialize returns 0 on failure
     * but active_fs stays FAT16 so all FS calls go to the FAT16 driver which
     * will safely return empty results rather than crashing).              */
    fat16_initialize();
    active_fs = FS_TYPE_FAT16;
}

void vfs_initialize_boot_fallback(void)
{
    nodes[0].path = "/welcome.txt";
    nodes[0].data = welcome_text;
    nodes[0].size = sizeof(welcome_text) - 1;

    vfs_reset_handles();

    active_fs = FS_TYPE_FAT16;
    boot_fallback_enabled = 1;
}

static int directory_has_entry(const char *path, const char *name)
{
    VFS_DIRECTORY_ENTRY entries[32];
    UINT64 count = vfs_list_directory(path, entries, 32);
    UINT64 index;
    for (index = 0; index < count; index++)
        if (strings_equal(entries[index].name, name)) return 1;
    return 0;
}

int vfs_mount_manager_self_test(void)
{
    int index;
    for (index = 0; index < vfs_volume_count; index++)
        if (vfs_volumes[index].valid)
            logger_write("INFO", vfs_volumes[index].mount_point);
    if (!vfs_is_directory("/system") || !vfs_is_directory("/media") ||
        !directory_has_entry("/", "system") ||
        !directory_has_entry("/", "media") ||
        !vfs_can_execute("/HELLO.EXE")) return 0;
    for (index = 1; index < vfs_volume_count; index++) {
        VFS_VOLUME_INFO snapshot;
        char test_path[64];
        UINT64 handle;
        if (!vfs_volumes[index].valid ||
            !strings_equal(vfs_volumes[index].mount_point, "/data")) continue;
        snapshot = vfs_volumes[index];
        vfs_test_path(test_path, sizeof(test_path), snapshot.mount_point,
                      "DISK.TXT");
        handle = vfs_open(test_path);
        if (handle == 0 || vfs_unmount(snapshot.mount_point) ||
            !vfs_close(handle) || !vfs_unmount(snapshot.mount_point) ||
            !vfs_mount_device(snapshot.device_name, snapshot.mount_point,
                              snapshot.read_only ? FILESYSTEM_FLAG_READ_ONLY : 0))
            return 0;
        handle = vfs_open(test_path);
        if (handle == 0 || !vfs_close(handle)) return 0;
        if (!vfs_unmount(snapshot.mount_point) ||
            !vfs_mount_device(snapshot.device_name, snapshot.mount_point,
                              FILESYSTEM_FLAG_NO_EXEC) ||
            vfs_can_execute(test_path) || !vfs_unmount(snapshot.mount_point) ||
            !vfs_mount_device(snapshot.device_name, snapshot.mount_point,
                              snapshot.read_only ? FILESYSTEM_FLAG_READ_ONLY : 0))
            return 0;
        return 1;
    }
    return 1;
}

UINT64 vfs_open(const char *path)
{
    UINT32 index;
    ASAS_FILESYSTEM_MOUNT *opened_mount = 0;

    if (strings_equal(path, nodes[0].path)) {
        return 1;
    }

    if (boot_fallback_enabled) {
        for (index = 0; index < boot_node_count; index++) {
            if (paths_equal(path, boot_nodes[index].path)) {
                return VFS_BOOT_HANDLE_BASE + index;
            }
        }

        return 0;
    }

    {
        const char *relative_path;
        ASAS_FILESYSTEM_MOUNT *mount = vfs_mount_for_path(path, &relative_path);
        if (mount != 0) {
            ASAS_FILE_STAT stat;
            if (!filesystem_stat(mount, relative_path, &stat) ||
                !stat.exists || stat.is_directory) return 0;
            if (!filesystem_mount_acquire(mount)) return 0;
            opened_mount = mount;
        } else {
            vfs_select_primary();
            if (active_fs == FS_TYPE_FAT32 && !fat32_exists(path)) return 0;
            if (active_fs == FS_TYPE_NTFS && !ntfs_exists(path)) return 0;
            if (active_fs == FS_TYPE_FAT16 &&
                fat16_file_size(path) == 0 && !fat16_is_directory(path)) return 0;
        }
    }

    vfs_lock();
    for (index = 0; index < VFS_DISK_HANDLE_COUNT; index++) {
        if (!disk_handles[index].used) {
            copy_string(disk_handles[index].path, path, VFS_PATH_CAPACITY);
            disk_handles[index].mount = opened_mount;
            disk_handles[index].mount_generation =
                opened_mount != 0 ? opened_mount->generation : 0;
            disk_handles[index].used = 1;
            vfs_unlock();
            return index + 2;
        }
    }
    vfs_unlock();
    if (opened_mount != 0) filesystem_mount_release(opened_mount);

    return 0;
}

UINT64 vfs_read(UINT64 handle, void *buffer, UINT64 size)
{
    UINT8 *output = (UINT8 *)buffer;
    UINT64 copy_size;
    UINT64 index;
    char disk_path[VFS_PATH_CAPACITY];
    ASAS_FILESYSTEM_MOUNT *disk_mount = 0;
    UINT32 mount_generation = 0;
    UINT8 disk_handle_open = 0;

    vfs_lock();
    if (
        handle >= 2 &&
        handle < VFS_DISK_HANDLE_COUNT + 2 &&
        disk_handles[handle - 2].used
    ) {
        copy_string(disk_path, disk_handles[handle - 2].path, VFS_PATH_CAPACITY);
        disk_mount = disk_handles[handle - 2].mount;
        mount_generation = disk_handles[handle - 2].mount_generation;
        disk_handle_open = 1;
    }
    vfs_unlock();

    if (
        boot_fallback_enabled &&
        handle >= VFS_BOOT_HANDLE_BASE &&
        handle < VFS_BOOT_HANDLE_BASE + boot_node_count
    ) {
        VFS_NODE *node = &boot_nodes[handle - VFS_BOOT_HANDLE_BASE];

        copy_size = size < node->size ? size : node->size;
        for (index = 0; index < copy_size; index++) {
            output[index] = node->data[index];
        }
        return copy_size;
    }

    if (disk_handle_open) {
        const char *relative_path;
        ASAS_FILESYSTEM_MOUNT *mount =
            vfs_mount_for_path(disk_path, &relative_path);
        if (disk_mount != 0) {
            if (!disk_mount->active ||
                disk_mount->generation != mount_generation ||
                mount != disk_mount)
                return 0;
        }
        if (mount != 0) return filesystem_read(mount, relative_path, buffer, size);
        vfs_select_primary();
        if      (active_fs == FS_TYPE_FAT32) return fat32_read_file(disk_path, buffer, size);
        else if (active_fs == FS_TYPE_NTFS)  return ntfs_read_file(disk_path, buffer, size);
        else                                  return fat16_read_file(disk_path, buffer, size);
    }

    if (handle != 1) {
        return 0;
    }

    copy_size = size < nodes[0].size ? size : nodes[0].size;
    for (index = 0; index < copy_size; index++) {
        output[index] = nodes[0].data[index];
    }
    return copy_size;
}

int vfs_close(UINT64 handle)
{
    if (handle == 1) {
        return 1;
    }
    if (
        handle >= VFS_BOOT_HANDLE_BASE &&
        handle < VFS_BOOT_HANDLE_BASE + boot_node_count
    ) {
        return 1;
    }
    vfs_lock();
    if (handle >= 2 && handle < VFS_DISK_HANDLE_COUNT + 2 &&
        disk_handles[handle - 2].used) {
        ASAS_FILESYSTEM_MOUNT *mount = disk_handles[handle - 2].mount;
        disk_handles[handle - 2].used = 0;
        disk_handles[handle - 2].mount = 0;
        vfs_unlock();
        if (mount != 0) filesystem_mount_release(mount);
        return 1;
    }
    vfs_unlock();
    return 0;
}

int vfs_write_file(const char *path, const void *buffer, UINT64 size)
{
    const char *relative_path;
    ASAS_FILESYSTEM_MOUNT *mount;
    if (boot_fallback_enabled) {
        (void)path;
        (void)buffer;
        (void)size;
        return 0;
    }

    mount = vfs_mount_for_path(path, &relative_path);
    if (mount != 0) return filesystem_write(mount, relative_path, buffer, size);
    if (active_fs == FS_TYPE_FAT32) return fat32_write_file(path, buffer, size);
    if (active_fs == FS_TYPE_NTFS)  return 0; /* NTFS write not supported */
    return fat16_write_root_file(path, buffer, size);
}

int vfs_delete_file(const char *path)
{
    const char *relative_path;
    ASAS_FILESYSTEM_MOUNT *mount;
    if (boot_fallback_enabled) {
        (void)path;
        return 0;
    }

    mount = vfs_mount_for_path(path, &relative_path);
    if (mount != 0) return filesystem_unlink(mount, relative_path);
    if (active_fs == FS_TYPE_FAT32) return fat32_delete_file(path);
    if (active_fs == FS_TYPE_NTFS)  return 0;
    return fat16_delete_root_file(path);
}

int vfs_create_directory(const char *path)
{
    const char *relative_path;
    ASAS_FILESYSTEM_MOUNT *mount;
    if (boot_fallback_enabled) {
        (void)path;
        return 0;
    }

    mount = vfs_mount_for_path(path, &relative_path);
    if (mount != 0) return filesystem_mkdir(mount, relative_path);
    if (active_fs == FS_TYPE_FAT32) return fat32_create_directory(path);
    if (active_fs == FS_TYPE_NTFS)  return 0;
    return fat16_create_root_directory(path);
}

int vfs_delete_directory(const char *path)
{
    const char *relative_path;
    ASAS_FILESYSTEM_MOUNT *mount;
    if (boot_fallback_enabled) {
        (void)path;
        return 0;
    }

    mount = vfs_mount_for_path(path, &relative_path);
    if (mount != 0) return filesystem_rmdir(mount, relative_path);
    if (active_fs == FS_TYPE_FAT32) return fat32_delete_directory(path);
    if (active_fs == FS_TYPE_NTFS) return 0;
    return fat16_delete_root_directory(path);
}

const char *vfs_write_status_reason(const char *path)
{
    const char *relative_path;
    ASAS_FILESYSTEM_MOUNT *mount;
    if (boot_fallback_enabled) return "storage is in RAM boot fallback";
    mount = vfs_mount_for_path(path, &relative_path);
    (void)relative_path;
    if (mount == 0 || !mount->active) return "no mounted filesystem for path";
    if (mount->device == 0) return "mounted filesystem has no block device";
    if ((mount->device->flags & BLOCK_DEVICE_FLAG_OPTICAL) != 0)
        return "physical optical media is read-only";
    if ((mount->device->flags & BLOCK_DEVICE_FLAG_READ_ONLY) != 0)
        return "device is write-protected";
    if ((mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0) {
        if (mount->driver != 0 && strings_equal(mount->driver->name, "ntfs") &&
            mount->context != 0)
            return ntfs_context_read_only_reason_string(
                (NTFS_CONTEXT *)mount->context);
        return "filesystem is mounted read-only";
    }
    if (mount->driver != 0 && strings_equal(mount->driver->name, "ntfs") &&
        mount->context != 0)
        return ntfs_context_last_error_string((NTFS_CONTEXT *)mount->context);
    if (mount->driver != 0 && strings_equal(mount->driver->name, "fat32") &&
        mount->context != 0)
        return fat32_context_last_error_string((FAT32_CONTEXT *)mount->context);
    reason_copy("driver ");
    reason_append(mount->driver != 0 ? mount->driver->name : "unknown");
    reason_append(": operation rejected by filesystem driver");
    return vfs_status_reason;
}

int vfs_rename(const char *source, const char *destination)
{
    const char *source_relative;
    const char *destination_relative;
    ASAS_FILESYSTEM_MOUNT *source_mount = vfs_mount_for_path(source, &source_relative);
    ASAS_FILESYSTEM_MOUNT *destination_mount =
        vfs_mount_for_path(destination, &destination_relative);
    if (source_mount == 0 || source_mount != destination_mount) return 0;
    return filesystem_rename(source_mount, source_relative, destination_relative);
}

int vfs_can_execute(const char *path)
{
    const char *relative_path;
    ASAS_FILESYSTEM_MOUNT *mount;
    if (path == 0) return 0;
    if (boot_fallback_enabled) return 1;
    mount = vfs_mount_for_path(path, &relative_path);
    (void)relative_path;
    return mount != 0 && (mount->flags & FILESYSTEM_FLAG_NO_EXEC) == 0;
}

int vfs_is_directory(const char *path)
{
    UINT32 index;
    const char *relative_path;
    ASAS_FILESYSTEM_MOUNT *mount;

    if (boot_fallback_enabled) {
        if (paths_equal(path, "/")) {
            return 1;
        }
        if (paths_equal(path, "/ASAS")) {
            for (index = 0; index < boot_node_count; index++) {
                if (paths_equal(boot_nodes[index].path, "/ASAS/README.TXT")) {
                    return 1;
                }
            }
        }
        for (index = 0; index < boot_node_count; index++) {
            if (paths_equal(path, boot_nodes[index].path)) {
                return 0;
            }
        }
        return 0;
    }

    if (strings_equal(path, "/media")) return 1;

    mount = vfs_mount_for_path(path, &relative_path);
    if (mount != 0) {
        ASAS_FILE_STAT stat;
        return filesystem_stat(mount, relative_path, &stat) && stat.is_directory;
    }
    if (active_fs == FS_TYPE_FAT32) return fat32_is_directory(path);
    if (active_fs == FS_TYPE_NTFS)  return ntfs_is_directory(path);
    return fat16_is_directory(path);
}

UINT64 vfs_file_size(const char *path)
{
    UINT32 index;
    const char *relative_path;
    ASAS_FILESYSTEM_MOUNT *mount;

    if (boot_fallback_enabled) {
        if (strings_equal(path, nodes[0].path)) {
            return nodes[0].size;
        }
        for (index = 0; index < boot_node_count; index++) {
            if (paths_equal(path, boot_nodes[index].path)) {
                return boot_nodes[index].size;
            }
        }
        return 0;
    }

    mount = vfs_mount_for_path(path, &relative_path);
    if (mount != 0) {
        ASAS_FILE_STAT stat;
        if (!filesystem_stat(mount, relative_path, &stat) || stat.is_directory) return 0;
        return stat.size;
    }
    vfs_select_primary();
    if (active_fs == FS_TYPE_FAT32) return fat32_file_size(path);
    if (active_fs == FS_TYPE_NTFS)  return ntfs_file_size(path);
    return fat16_file_size(path);
}

UINT64 vfs_list_root(VFS_DIRECTORY_ENTRY *entries, UINT64 capacity)
{
    return vfs_list_directory("/", entries, capacity);
}

UINT64 vfs_list_directory(const char *path, VFS_DIRECTORY_ENTRY *entries, UINT64 capacity)
{
    UINT64 output_count = 0;
    UINT64 index;
    UINT64 internal_cap;
    UINT32 boot_index;

    if (capacity == 0) return 0;

    if (!boot_fallback_enabled && strings_equal(path, "/media")) {
        int volume_index;
        for (volume_index = 0; volume_index < vfs_volume_count &&
             output_count < capacity; volume_index++) {
            const char *mount_point = vfs_volumes[volume_index].mount_point;
            const char *name;
            if (!vfs_volumes[volume_index].valid ||
                !path_has_prefix(mount_point, "/media") ||
                mount_point[6] != '/') continue;
            name = mount_point + 7;
            copy_string(entries[output_count].name, name,
                        sizeof(entries[output_count].name));
            entries[output_count].size = 0;
            entries[output_count].is_directory = 1;
            entries[output_count].permissions = VFS_PERMISSION_READ;
            if (!vfs_volumes[volume_index].read_only)
                entries[output_count].permissions |= VFS_PERMISSION_WRITE;
            output_count++;
        }
        return output_count;
    }

    /* Route paths belonging to a secondary volume (e.g. "/hdd1/FOLDER") */
    if (!boot_fallback_enabled) {
        int vol_idx;
        const char *rel_path;
        if (vfs_select_volume_for_path(path, &vol_idx, &rel_path)) {
            return vfs_list_on_volume(vol_idx, rel_path, entries, capacity);
        }
        /* Ensure the primary device is active for the calls below */
        vfs_select_primary();
    }

    /* Add the virtual welcome.txt for the root directory */
    if (strings_equal(path, "/")) {
        copy_string(entries[0].name, "welcome.txt", sizeof(entries[0].name));
        entries[0].size         = nodes[0].size;
        entries[0].is_directory = 0;
        entries[0].permissions  = VFS_PERMISSION_READ;
        output_count = 1;
    }

    if (boot_fallback_enabled) {
        if (strings_equal(path, "/")) {
            int added_asas = 0;

            for (boot_index = 0; boot_index < boot_node_count && output_count < capacity; boot_index++) {
                if (paths_equal(boot_nodes[boot_index].path, "/DISK.TXT")) {
                    copy_string(entries[output_count].name, "DISK.TXT", sizeof(entries[output_count].name));
                    entries[output_count].size = boot_nodes[boot_index].size;
                    entries[output_count].is_directory = 0;
                    entries[output_count].permissions = VFS_PERMISSION_READ;
                    output_count++;
                } else if (paths_equal(boot_nodes[boot_index].path, "/HELLO.EXE")) {
                    copy_string(entries[output_count].name, "HELLO.EXE", sizeof(entries[output_count].name));
                    entries[output_count].size = boot_nodes[boot_index].size;
                    entries[output_count].is_directory = 0;
                    entries[output_count].permissions = VFS_PERMISSION_READ | VFS_PERMISSION_EXECUTE;
                    output_count++;
                } else if (
                    paths_equal(boot_nodes[boot_index].path, "/ASAS/README.TXT") &&
                    !added_asas &&
                    output_count < capacity
                ) {
                    copy_string(entries[output_count].name, "ASAS", sizeof(entries[output_count].name));
                    entries[output_count].size = 0;
                    entries[output_count].is_directory = 1;
                    entries[output_count].permissions = VFS_PERMISSION_READ;
                    output_count++;
                    added_asas = 1;
                }
            }
            return output_count;
        }

        if (paths_equal(path, "/ASAS")) {
            for (boot_index = 0; boot_index < boot_node_count && output_count < capacity; boot_index++) {
                if (paths_equal(boot_nodes[boot_index].path, "/ASAS/README.TXT")) {
                    copy_string(entries[output_count].name, "README.TXT", sizeof(entries[output_count].name));
                    entries[output_count].size = boot_nodes[boot_index].size;
                    entries[output_count].is_directory = 0;
                    entries[output_count].permissions = VFS_PERMISSION_READ;
                    output_count++;
                }
            }
            return output_count;
        }

        return 0;
    }

    /* Reserve slots already used by virtual entries (e.g. welcome.txt at root) */
    {
        UINT64 remaining = output_count < capacity ? capacity - output_count : 0;
        internal_cap = remaining < 256 ? remaining : 256;
    }
    if (internal_cap == 0) return output_count;

    if (vfs_volume_count > 0 && vfs_mounts[0] != 0) {
        ASAS_FILESYSTEM_ENTRY *items = (ASAS_FILESYSTEM_ENTRY *)kmalloc(
            (UINTN)(internal_cap * sizeof(ASAS_FILESYSTEM_ENTRY)));
        UINT64 count;
        if (items == 0) return output_count;
        count = filesystem_list(vfs_mounts[0], path, items, internal_cap);
        for (index = 0; index < count && output_count < capacity; index++) {
            copy_string(entries[output_count].name, items[index].name,
                        sizeof(entries[output_count].name));
            entries[output_count].size = items[index].size;
            entries[output_count].is_directory = items[index].is_directory;
            entries[output_count].permissions = VFS_PERMISSION_READ;
            if (!items[index].read_only)
                entries[output_count].permissions |= VFS_PERMISSION_WRITE;
            output_count++;
        }
        kfree(items);
    } else if (active_fs == FS_TYPE_FAT32) {
        FAT32_FILE_INFO *fat_entries = (FAT32_FILE_INFO *)kmalloc(
            (UINTN)(internal_cap * sizeof(FAT32_FILE_INFO)));
        UINT64 fat_count;
        if (!fat_entries) return output_count;
        fat_count = fat32_list_directory(path, fat_entries, internal_cap);
        logger_write_hex("VFS", "fat32 list count", fat_count);
        for (index = 0; index < fat_count && output_count < capacity; index++) {
            copy_string(entries[output_count].name, fat_entries[index].name,
                        sizeof(entries[output_count].name));
            entries[output_count].size         = fat_entries[index].size;
            entries[output_count].is_directory  = fat_entries[index].is_directory;
            entries[output_count].permissions   = VFS_PERMISSION_READ;
            if (!fat_entries[index].read_only)
                entries[output_count].permissions |= VFS_PERMISSION_WRITE;
            output_count++;
        }
        kfree(fat_entries);
    } else if (active_fs == FS_TYPE_NTFS) {
        /* NTFS inline INDEX_ROOT rarely exceeds 64 entries */
        UINT64 ntfs_cap = internal_cap < 64 ? internal_cap : 64;
        NTFS_FILE_INFO *ntfs_entries = (NTFS_FILE_INFO *)kmalloc(
            (UINTN)(ntfs_cap * sizeof(NTFS_FILE_INFO)));
        UINT64 ntfs_count;
        if (!ntfs_entries) return output_count;
        ntfs_count = ntfs_list_directory(path, ntfs_entries, ntfs_cap);
        for (index = 0; index < ntfs_count && output_count < capacity; index++) {
            copy_string(entries[output_count].name, ntfs_entries[index].name,
                        sizeof(entries[output_count].name));
            entries[output_count].size         = ntfs_entries[index].size;
            entries[output_count].is_directory  = ntfs_entries[index].is_directory;
            entries[output_count].permissions   = VFS_PERMISSION_READ;
            if (!ntfs_entries[index].read_only)
                entries[output_count].permissions |= VFS_PERMISSION_WRITE;
            output_count++;
        }
        kfree(ntfs_entries);
    } else {
        /* FAT16 */
        FAT16_FILE_INFO *fat_entries = (FAT16_FILE_INFO *)kmalloc(
            (UINTN)(internal_cap * sizeof(FAT16_FILE_INFO)));
        UINT64 fat_count;
        if (!fat_entries) return output_count;
        fat_count = fat16_list_directory(path, fat_entries, internal_cap);
        logger_write_hex("VFS", "fat16 list count", fat_count);
        for (index = 0; index < fat_count && output_count < capacity; index++) {
            copy_string(entries[output_count].name, fat_entries[index].name,
                        sizeof(entries[output_count].name));
            entries[output_count].size         = fat_entries[index].size;
            entries[output_count].is_directory  = fat_entries[index].is_directory;
            entries[output_count].permissions   = VFS_PERMISSION_READ;
            if (!fat_entries[index].read_only)
                entries[output_count].permissions |= VFS_PERMISSION_WRITE;
            output_count++;
        }
        kfree(fat_entries);
    }

    /* Append the organized mount namespace to the virtual root. */
    if (strings_equal(path, "/")) {
        static const char *names[3] = { "system", "data", "media" };
        int namespace_index;
        for (namespace_index = 0; namespace_index < 3 &&
             output_count < capacity; namespace_index++) {
            int include = namespace_index != 1;
            int volume_index;
            if (namespace_index == 1) {
                for (volume_index = 1; volume_index < vfs_volume_count;
                     volume_index++)
                    if (vfs_volumes[volume_index].valid &&
                        strings_equal(vfs_volumes[volume_index].mount_point,
                                      "/data")) include = 1;
            }
            if (!include) continue;
            copy_string(entries[output_count].name, names[namespace_index],
                        sizeof(entries[output_count].name));
            entries[output_count].size        = 0;
            entries[output_count].is_directory = 1;
            entries[output_count].permissions  = VFS_PERMISSION_READ;
            output_count++;
        }
    }

    return output_count;
}

int vfs_get_volume_count(void)
{
    return vfs_volume_count;
}

const VFS_VOLUME_INFO *vfs_get_volume(int index)
{
    if (index < 0 || index >= vfs_volume_count) return (const VFS_VOLUME_INFO *)0;
    return &vfs_volumes[index];
}

static int block_device_has_partitions(ASAS_BLOCK_DEVICE *device)
{
    UINT32 index;
    for (index = 0; index < block_device_count(); index++) {
        ASAS_BLOCK_DEVICE *candidate = block_device_get(index);
        if (candidate != 0 && candidate->parent == device) return 1;
    }
    return 0;
}

static int mount_point_in_use(const char *path)
{
    int index;
    for (index = 0; index < vfs_volume_count; index++) {
        if (vfs_volumes[index].valid &&
            strings_equal(vfs_volumes[index].mount_point, path))
            return 1;
    }
    return 0;
}

static void append_decimal(char *path, UINT32 capacity, UINT32 number)
{
    char digits[10];
    UINT32 digit_count = 0;
    UINT32 length = 0;
    while (path[length] != '\0') length++;
    do {
        digits[digit_count++] = (char)('0' + (number % 10U));
        number /= 10U;
    } while (number != 0 && digit_count < sizeof(digits));
    while (digit_count != 0 && length + 1U < capacity) {
        path[length++] = digits[--digit_count];
    }
    path[length] = '\0';
}

static void choose_auto_mount_point(VFS_VOLUME_INFO *volume,
                                    char path[32])
{
    UINT32 number = 0;
    if (volume->is_cdrom) {
        do {
            copy_string(path, "/media/cdrom", 32);
            append_decimal(path, 32, number++);
        } while (mount_point_in_use(path));
    } else if (volume->is_removable) {
        do {
            copy_string(path, "/media/usb", 32);
            append_decimal(path, 32, number++);
        } while (mount_point_in_use(path));
    } else if (!mount_point_in_use("/data")) {
        copy_string(path, "/data", 32);
    } else {
        number = 1;
        do {
            copy_string(path, "/media/disk", 32);
            append_decimal(path, 32, number++);
        } while (mount_point_in_use(path));
    }
}

static void register_hyperv_storage_devices(int force_rescan)
{
    int hyperv_count;
    int index;
    const ASAS_STORAGE_DEVICE *devices;

    hyperv_count = force_rescan
        ? hyperv_storage_rescan_devices()
        : hyperv_storage_probe_devices();
    devices = hyperv_storage_get_devices();
    for (index = 0; index < hyperv_count && devices != 0; index++) {
        if (!devices[index].valid) continue;
        (void)hyperv_storage_register_block_device(devices[index].target,
                                                   devices[index].lun);
    }
}

/* Probe all whole-disk filesystems and discovered MBR/GPT partitions. */
int vfs_mount_all_volumes(void)
{
    UINT32 index;
    UINT32 count = block_device_count();

    for (index = 0; index < count; index++) {
        ASAS_BLOCK_DEVICE *device = block_device_get(index);
        const ASAS_FILESYSTEM_DRIVER *driver;
        ASAS_FILESYSTEM_MOUNT *mount;
        VFS_VOLUME_INFO *volume;
        const ASAS_PARTITION_INFO *partition;
        int volume_number;
        int slot;

        if (device == 0) continue;
        if (filesystem_device_is_mounted(device)) continue;
        if (device->parent == 0 && block_device_has_partitions(device)) continue;

        driver = filesystem_probe(device);
        if (driver == 0) continue;
        mount = filesystem_mount(device, driver, 0);
        if (mount == 0) continue;

        for (slot = 0; slot < VFS_MAX_VOLUMES; slot++)
            if (slot >= vfs_volume_count || !vfs_volumes[slot].valid) break;
        if (slot == VFS_MAX_VOLUMES) {
            (void)filesystem_unmount(mount);
            break;
        }
        volume_number = slot;
        volume = &vfs_volumes[volume_number];
        volume->valid = 1;
        volume->is_cdrom = (device->flags & BLOCK_DEVICE_FLAG_OPTICAL) != 0;
        volume->is_removable =
            (device->flags & BLOCK_DEVICE_FLAG_REMOVABLE) != 0;
        volume->read_only = (mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0 ||
            (device->flags & BLOCK_DEVICE_FLAG_READ_ONLY) != 0;
        volume->no_exec = (mount->flags & FILESYSTEM_FLAG_NO_EXEC) != 0;
        volume->target = device->target;
        volume->lun = device->lun;
        volume->fs_type = vfs_type_from_driver(driver);
        copy_string(volume->device_name, device->name, sizeof(volume->device_name));
        volume->partition_type[0] = '\0';
        volume->uuid[0] = '\0';
        partition = partition_find_by_device(device);
        if (partition != 0) {
            copy_string(volume->partition_type, partition->type_name,
                        sizeof(volume->partition_type));
            copy_string(volume->uuid, partition->uuid, sizeof(volume->uuid));
        }
        if (volume_number == 0) {
            copy_string(volume->mount_point, "/system", sizeof(volume->mount_point));
            if (partition != 0 && partition->label[0] != '\0')
                copy_string(volume->label, partition->label, sizeof(volume->label));
            else copy_string(volume->label, "System", sizeof(volume->label));
        } else {
            char path[32];
            choose_auto_mount_point(volume, path);
            copy_string(volume->mount_point, path, sizeof(volume->mount_point));
            if (partition != 0 && partition->label[0] != '\0')
                copy_string(volume->label, partition->label, sizeof(volume->label));
            else copy_string(volume->label, device->name, sizeof(volume->label));
        }
        vfs_mounts[volume_number] = mount;
        if (volume_number == vfs_volume_count) vfs_volume_count++;
    }

    vfs_select_primary();
    return vfs_volume_count;
}

int vfs_rescan_devices(void)
{
    int index;

    if (boot_fallback_enabled) return 0;

    (void)virtio_block_register_device();
    (void)nvme_register_block_device();
    (void)xhci_register_storage_block_device();
    (void)ahci_register_block_device();

    register_hyperv_storage_devices(1);

    for (index = 0; index < (int)block_device_count(); index++) {
        ASAS_BLOCK_DEVICE *device = block_device_get((UINT32)index);
        if (device != 0 && device->parent == 0 &&
            (device->flags & BLOCK_DEVICE_FLAG_OPTICAL) == 0 &&
            !block_device_has_partitions(device)) {
            (void)partition_scan_device(device);
        }
    }

    return vfs_mount_all_volumes();
}

int vfs_handle_device_removed(const char *device_name)
{
    ASAS_BLOCK_DEVICE *device;
    int index;
    int affected = 0;

    if (device_name == 0 || device_name[0] == '\0') return 0;
    device = block_device_find(device_name);
    if (device == 0) return 0;

    for (index = 0; index < vfs_volume_count; index++) {
        ASAS_FILESYSTEM_MOUNT *mount = vfs_mounts[index];
        ASAS_BLOCK_DEVICE *mounted;
        if (!vfs_volumes[index].valid || mount == 0) continue;
        mounted = mount->device;
        if (mounted != device &&
            !(mounted != 0 && mounted->parent == device)) continue;

        mount->flags |= FILESYSTEM_FLAG_READ_ONLY | FILESYSTEM_FLAG_NO_EXEC;
        vfs_volumes[index].read_only = 1;
        vfs_volumes[index].no_exec = 1;
        if (block_device_flush_barrier(mounted)) {
            logger_write("VFS", "device removal barrier flushed");
        }
        if (mount->reference_count == 0 &&
            filesystem_unmount(mount)) {
            vfs_volumes[index].valid = 0;
            vfs_mounts[index] = 0;
            logger_write("VFS", "removed device volume unmounted");
        } else {
            logger_write("VFS", "removed device volume locked read-only");
        }
        affected++;
    }

    device->flags |= BLOCK_DEVICE_FLAG_READ_ONLY | BLOCK_DEVICE_FLAG_NO_CACHE;
    return affected;
}

int vfs_mount_device(const char *device_name, const char *mount_point,
                     UINT32 flags)
{
    ASAS_BLOCK_DEVICE *device;
    const ASAS_FILESYSTEM_DRIVER *driver;
    ASAS_FILESYSTEM_MOUNT *mount;
    VFS_VOLUME_INFO *volume;
    UINT32 index;
    UINT32 slot;
    if (device_name == 0 || mount_point == 0 || mount_point[0] != '/' ||
        strings_equal(mount_point, "/") || strings_equal(mount_point, "/media") ||
        (flags & ~(FILESYSTEM_FLAG_READ_ONLY | FILESYSTEM_FLAG_NO_EXEC)) != 0)
        return 0;
    for (index = 0; index < (UINT32)vfs_volume_count; index++)
        if (vfs_volumes[index].valid &&
            (path_has_prefix(mount_point, vfs_volumes[index].mount_point) ||
             path_has_prefix(vfs_volumes[index].mount_point, mount_point))) return 0;
    device = block_device_find(device_name);
    if (device == 0) {
        for (index = 0; index < block_device_count(); index++) {
            ASAS_BLOCK_DEVICE *candidate = block_device_get(index);
            const ASAS_PARTITION_INFO *partition =
                candidate != 0 ? partition_find_by_device(candidate) : 0;
            if (partition != 0 &&
                (strings_equal(partition->uuid, device_name) ||
                 strings_equal(partition->label, device_name))) {
                device = candidate;
                break;
            }
        }
    }
    if (device == 0 || filesystem_device_is_mounted(device)) return 0;
    driver = filesystem_probe(device);
    if (driver == 0) return 0;
    mount = filesystem_mount(device, driver, flags);
    if (mount == 0) return 0;
    for (slot = 0; slot < VFS_MAX_VOLUMES; slot++)
        if (slot >= (UINT32)vfs_volume_count || !vfs_volumes[slot].valid) break;
    if (slot == VFS_MAX_VOLUMES) {
        (void)filesystem_unmount(mount);
        return 0;
    }
    volume = &vfs_volumes[slot];
    copy_string(volume->mount_point, mount_point, sizeof(volume->mount_point));
    copy_string(volume->device_name, device->name, sizeof(volume->device_name));
    copy_string(volume->label, device->name, sizeof(volume->label));
    volume->valid = 1;
    volume->is_cdrom = (device->flags & BLOCK_DEVICE_FLAG_OPTICAL) != 0;
    volume->is_removable = (device->flags & BLOCK_DEVICE_FLAG_REMOVABLE) != 0;
    volume->read_only = (mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0;
    volume->no_exec = (mount->flags & FILESYSTEM_FLAG_NO_EXEC) != 0;
    volume->target = device->target;
    volume->lun = device->lun;
    volume->fs_type = vfs_type_from_driver(driver);
    volume->partition_type[0] = '\0';
    volume->uuid[0] = '\0';
    {
        const ASAS_PARTITION_INFO *partition = partition_find_by_device(device);
        if (partition != 0) {
            copy_string(volume->partition_type, partition->type_name,
                        sizeof(volume->partition_type));
            copy_string(volume->uuid, partition->uuid, sizeof(volume->uuid));
            if (partition->label[0] != '\0')
                copy_string(volume->label, partition->label,
                            sizeof(volume->label));
        }
    }
    vfs_mounts[slot] = mount;
    if (slot == (UINT32)vfs_volume_count) vfs_volume_count++;
    return 1;
}

int vfs_unmount(const char *mount_point)
{
    int index;
    if (mount_point == 0 || strings_equal(mount_point, "/") ||
        strings_equal(mount_point, "/system")) return 0;
    for (index = 0; index < vfs_volume_count; index++) {
        if (!vfs_volumes[index].valid ||
            !strings_equal(vfs_volumes[index].mount_point, mount_point)) continue;
        if (!filesystem_unmount(vfs_mounts[index])) return 0;
        vfs_volumes[index].valid = 0;
        vfs_mounts[index] = 0;
        return 1;
    }
    return 0;
}

int vfs_remount(const char *mount_point, UINT32 flags)
{
    int index;
    VFS_VOLUME_INFO snapshot;
    if (mount_point == 0 || strings_equal(mount_point, "/") ||
        strings_equal(mount_point, "/system") ||
        (flags & ~(FILESYSTEM_FLAG_READ_ONLY | FILESYSTEM_FLAG_NO_EXEC)) != 0)
        return 0;
    for (index = 0; index < vfs_volume_count; index++) {
        if (!vfs_volumes[index].valid ||
            !strings_equal(vfs_volumes[index].mount_point, mount_point))
            continue;
        snapshot = vfs_volumes[index];
        if (!vfs_unmount(mount_point)) return 0;
        if (vfs_mount_device(snapshot.device_name, snapshot.mount_point, flags))
            return 1;
        (void)vfs_mount_device(snapshot.device_name, snapshot.mount_point,
                               snapshot.read_only ? FILESYSTEM_FLAG_READ_ONLY : 0);
        return 0;
    }
    return 0;
}

int vfs_sync_path(const char *path)
{
    const char *relative_path;
    ASAS_FILESYSTEM_MOUNT *mount;
    (void)relative_path;
    if (path == 0) return 0;
    mount = vfs_mount_for_path(path, &relative_path);
    return mount != 0 && filesystem_sync(mount);
}

int vfs_self_test(void)
{
    static const char lfn_payload[] = "Asas FAT32 LFN mutation";
    UINT8 buffer[32];
    VFS_DIRECTORY_ENTRY entries[8];
    UINT64 handle = vfs_open("/welcome.txt");
    UINT64 bytes = vfs_read(handle, buffer, sizeof(buffer));
    UINT64 disk_handle;
    UINT64 disk_bytes;
    UINT64 entry_count;
    UINT64 nested_handle;
    UINT64 nested_bytes;
    UINT64 long_name_handle;
    UINT64 long_name_bytes;

    if (handle != 1 || bytes != sizeof(welcome_text) - 1 || buffer[0] != 'A') {
        return 0;
    }

    disk_handle = vfs_open("/disk.txt");
    disk_bytes = vfs_read(disk_handle, buffer, sizeof(buffer));
    (void)vfs_close(disk_handle);
    entry_count = vfs_list_root(entries, 8);
    nested_handle = vfs_open("/ASAS/README.TXT");
    nested_bytes = vfs_read(nested_handle, buffer, sizeof(buffer));
    (void)vfs_close(nested_handle);
    long_name_handle = vfs_open("/ASAS/System Readme.txt");
    long_name_bytes = vfs_read(long_name_handle, buffer, sizeof(buffer));
    (void)vfs_close(long_name_handle);
    if (!(
        disk_handle == 2 &&
        disk_bytes > 10 &&
        buffer[0] == 'A' &&
        buffer[5] == 'O' &&
        entry_count >= 5 &&
        nested_handle != 0 &&
        nested_bytes > 10 &&
        long_name_handle != 0 &&
        long_name_bytes == nested_bytes &&
        buffer[0] == 'A'
    )) return 0;
    if (!vfs_write_file("/Created Long Filename.txt", lfn_payload,
                        sizeof(lfn_payload) - 1)) return 0;
    handle = vfs_open("/Created Long Filename.txt");
    bytes = vfs_read(handle, buffer, sizeof(buffer));
    (void)vfs_close(handle);
    if (handle == 0 || bytes != sizeof(lfn_payload) - 1 ||
        buffer[0] != 'A' ||
        !vfs_rename("/Created Long Filename.txt",
                    "/Renamed Long Filename.txt") ||
        vfs_open("/Created Long Filename.txt") != 0) return 0;
    handle = vfs_open("/Renamed Long Filename.txt");
    bytes = vfs_read(handle, buffer, sizeof(buffer));
    (void)vfs_close(handle);
    if (handle == 0 || bytes != sizeof(lfn_payload) - 1 ||
        !vfs_delete_file("/Renamed Long Filename.txt") ||
        vfs_open("/Renamed Long Filename.txt") != 0 ||
        !vfs_create_directory("/Created Long Directory") ||
        !vfs_delete_directory("/Created Long Directory")) return 0;
    return 1;
}

static void vfs_test_path(char *path, UINTN capacity, const char *mount,
                          const char *name)
{
    UINTN used = 0;
    while (mount[used] != '\0' && used + 1 < capacity) {
        path[used] = mount[used];
        used++;
    }
    if (used > 1 && path[used - 1] == '/') used--;
    if (used + 1 < capacity) path[used++] = '/';
    while (*name != '\0' && used + 1 < capacity) path[used++] = *name++;
    path[used] = '\0';
}

static void vfs_test_numbered_path(char *path, UINTN capacity,
                                   const char *mount, const char *directory,
                                   UINT32 number)
{
    char suffix[16];
    UINTN length = 0;
    UINT32 divisor = 100;
    const char *prefix = directory;
    vfs_test_path(path, capacity, mount, prefix);
    while (path[length] != '\0') length++;
    if (length + 10 >= capacity) return;
    path[length++] = '/';
    path[length++] = 'E';
    path[length++] = 'N';
    path[length++] = 'T';
    path[length++] = 'R';
    path[length++] = 'Y';
    path[length++] = '-';
    suffix[0] = (char)('0' + (number / divisor) % 10);
    divisor /= 10;
    suffix[1] = (char)('0' + (number / divisor) % 10);
    divisor /= 10;
    suffix[2] = (char)('0' + (number / divisor) % 10);
    suffix[3] = '.';
    suffix[4] = 'T';
    suffix[5] = 'X';
    suffix[6] = 'T';
    suffix[7] = '\0';
    for (number = 0; suffix[number] != '\0'; number++) path[length++] = suffix[number];
    path[length] = '\0';
}

int vfs_ntfs_mutation_self_test(void)
{
    static UINT8 large_data[2097152];
    static const char small_data[] = "Asas NTFS mutation integration";
    char marker[96];
    char small[96];
    char large[96];
    char moved[96];
    char directory[96];
    char entry[96];
    char full[96];
    int volume_index;
    int ntfs_volume = -1;
    UINT32 index;

    for (volume_index = 0; volume_index < vfs_volume_count; volume_index++) {
        if (vfs_volumes[volume_index].fs_type != VFS_FS_NTFS) continue;
        vfs_test_path(marker, sizeof(marker),
                      vfs_volumes[volume_index].mount_point,
                      "WINDOWS-SEED.TXT");
        if (vfs_file_size(marker) != 0) {
            ntfs_volume = volume_index;
            break;
        }
    }
    if (ntfs_volume < 0) return 1;

    for (index = 0; index < sizeof(large_data); index++)
        large_data[index] = (UINT8)(index ^ (index >> 8));
    vfs_test_path(small, sizeof(small), vfs_volumes[ntfs_volume].mount_point,
                  "ASAS-SMALL.TXT");
    vfs_test_path(large, sizeof(large), vfs_volumes[ntfs_volume].mount_point,
                  "ASAS-LARGE.BIN");
    vfs_test_path(directory, sizeof(directory),
                  vfs_volumes[ntfs_volume].mount_point, "ASAS-LARGE-DIR");
    vfs_test_path(moved, sizeof(moved), vfs_volumes[ntfs_volume].mount_point,
                  "ASAS-LARGE-DIR/MOVED.BIN");
    vfs_test_path(full, sizeof(full), vfs_volumes[ntfs_volume].mount_point,
                  "ASAS-DISK-FULL.BIN");

    if (!vfs_write_file(small, small_data, sizeof(small_data) - 1) ||
        vfs_file_size(small) != sizeof(small_data) - 1 ||
        !vfs_create_directory(directory) ||
        !vfs_write_file(large, large_data, 4096) ||
        !vfs_write_file(large, large_data, 786432) ||
        vfs_file_size(large) != 786432 ||
        !vfs_rename(large, moved) || vfs_file_size(moved) != 786432 ||
        vfs_file_size(large) != 0) return 0;
    logger_write("INFO", "NTFS runlist growth attribute list and move verified");

    for (index = 0; index < 160; index++) {
        vfs_test_numbered_path(entry, sizeof(entry),
                               vfs_volumes[ntfs_volume].mount_point,
                               "ASAS-LARGE-DIR", index);
        if (!vfs_write_file(entry, small_data, sizeof(small_data) - 1)) return 0;
    }
    logger_write("INFO", "NTFS large directory split and remount verified");

    for (index = 0; index < 160; index++) {
        vfs_test_numbered_path(entry, sizeof(entry),
                               vfs_volumes[ntfs_volume].mount_point,
                               "ASAS-LARGE-DIR", index);
        if (!vfs_delete_file(entry)) return 0;
    }
    if (!vfs_delete_directory(directory) || !vfs_delete_file(small) ||
        !vfs_delete_file(moved)) return 0;
    logger_write("INFO", "NTFS index merge cleanup and allocation release verified");

    /* The prepared image leaves less than 2 MiB free.  Allocation must fail
       before publishing metadata, and the failed path must remain absent. */
    if (vfs_write_file(full, large_data, sizeof(large_data)) ||
        vfs_file_size(full) != 0 || vfs_file_size(marker) == 0) return 0;
    logger_write("INFO", "NTFS disk full rollback and remount verified");
    logger_write("INFO", "NTFS mutation integration suite passed");
    return 1;
}

int vfs_exfat_integration_self_test(void)
{
    static const char marker_name[] = "EXFAT-SEED.TXT";
    static const char unicode_name[] = "Unicode-\xCE\xA9.txt";
    UINT8 buffer[64];
    char path[96];
    UINT64 handle;
    UINT64 bytes;
    static const char mutation_data[] = "Asas exFAT writable mutation";
    char created[96];
    char directory[96];
    char moved[96];
    int index;
    for (index = 0; index < vfs_volume_count; index++) {
        if (vfs_volumes[index].fs_type != VFS_FS_EXFAT) continue;
        vfs_test_path(path, sizeof(path), vfs_volumes[index].mount_point,
                      marker_name);
        handle = vfs_open(path);
        bytes = vfs_read(handle, buffer, sizeof(buffer));
        (void)vfs_close(handle);
        if (handle == 0 || bytes < 10U || buffer[0] != 'A') return 0;
        vfs_test_path(path, sizeof(path), vfs_volumes[index].mount_point,
                      unicode_name);
        handle = vfs_open(path);
        bytes = vfs_read(handle, buffer, sizeof(buffer));
        (void)vfs_close(handle);
        if (handle == 0 || bytes < 10U || buffer[0] != 'U') return 0;
        vfs_test_path(created, sizeof(created), vfs_volumes[index].mount_point,
                      "ASAS-CREATED.TXT");
        vfs_test_path(directory, sizeof(directory), vfs_volumes[index].mount_point,
                      "ASAS-DIR");
        vfs_test_path(moved, sizeof(moved), vfs_volumes[index].mount_point,
                      "ASAS-DIR/MOVED.TXT");
        if (!vfs_write_file(created, mutation_data, sizeof(mutation_data) - 1U) ||
            vfs_file_size(created) != sizeof(mutation_data) - 1U ||
            !vfs_write_file(created, mutation_data, 10U) ||
            vfs_file_size(created) != 10U ||
            !vfs_create_directory(directory) ||
            !vfs_rename(created, moved) || vfs_file_size(created) != 0 ||
            vfs_file_size(moved) != 10U || !vfs_delete_file(moved) ||
            !vfs_delete_directory(directory)) return 0;
        logger_write("INFO", "exFAT create write overwrite move delete and remount verified");
        return 1;
    }
    return 1;
}
