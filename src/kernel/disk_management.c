#include "disk_management.h"
#include "block_device.h"
#include "filesystem.h"
#include "logger.h"
#include "ntfs.h"
#include "partition.h"
#include "vfs.h"
#include "virtual_disk.h"

static int dm_strings_equal(const char *left, const char *right)
{
    if (left == 0 || right == 0) return 0;
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) return 0;
        left++;
        right++;
    }
    return *left == *right;
}

static ASAS_BLOCK_DEVICE *dm_find_device(const char *name)
{
    UINT32 index;
    ASAS_BLOCK_DEVICE *device = block_device_find(name);
    if (device != 0) return device;
    for (index = 0; index < block_device_count(); index++) {
        const ASAS_PARTITION_INFO *partition;
        device = block_device_get(index);
        partition = partition_find_by_device(device);
        if (partition != 0 &&
            (dm_strings_equal(partition->label, name) ||
             dm_strings_equal(partition->uuid, name)))
            return device;
    }
    return 0;
}

static int dm_is_mounted(const ASAS_BLOCK_DEVICE *device)
{
    return device != 0 && filesystem_device_is_mounted(device);
}

static int dm_supported_format_name(const char *fs_name)
{
    return dm_strings_equal(fs_name, "fat32") ||
           dm_strings_equal(fs_name, "exfat") ||
           dm_strings_equal(fs_name, "udf") ||
           dm_strings_equal(fs_name, "ext2") ||
           dm_strings_equal(fs_name, "ntfs");
}

int disk_management_list_disks(void)
{
    UINT32 index;
    logger_write_hex("DISKMGMT", "block devices",
                     (UINT64)block_device_count());
    for (index = 0; index < block_device_count(); index++) {
        ASAS_BLOCK_DEVICE *device = block_device_get(index);
        if (device == 0 || device->parent != 0) continue;
        logger_write("DISKMGMT", device->name);
        logger_write_hex("DISKMGMT", "blocks", device->block_count);
        logger_write_hex("DISKMGMT", "logical block size",
                         (UINT64)device->logical_block_size);
        logger_write_hex("DISKMGMT", "flags", (UINT64)device->flags);
        logger_write_hex("DISKMGMT", "virtual",
                         (UINT64)(virtual_disk_find(device->name) != 0));
    }
    return 1;
}

int disk_management_list_partitions(void)
{
    UINT32 index;
    logger_write_hex("DISKMGMT", "partitions", (UINT64)partition_count());
    for (index = 0; index < partition_count(); index++) {
        const ASAS_PARTITION_INFO *partition = partition_get(index);
        if (partition == 0 || partition->device == 0) continue;
        logger_write("DISKMGMT", partition->device->name);
        if (partition->parent != 0) logger_write("DISKMGMT", partition->parent->name);
        logger_write("DISKMGMT", partition->type_name);
        if (partition->label[0] != '\0') logger_write("DISKMGMT", partition->label);
        if (partition->uuid[0] != '\0') logger_write("DISKMGMT", partition->uuid);
        logger_write_hex("DISKMGMT", "start LBA", partition->start_lba);
        logger_write_hex("DISKMGMT", "blocks", partition->block_count);
        logger_write_hex("DISKMGMT", "scheme", (UINT64)partition->scheme);
    }
    return 1;
}

int disk_management_list_volumes(void)
{
    int index;
    logger_write_hex("DISKMGMT", "volumes", (UINT64)(UINT32)vfs_get_volume_count());
    for (index = 0; index < vfs_get_volume_count(); index++) {
        const VFS_VOLUME_INFO *volume = vfs_get_volume(index);
        if (volume == 0 || !volume->valid) continue;
        logger_write("DISKMGMT", volume->mount_point);
        logger_write("DISKMGMT", volume->device_name);
        logger_write("DISKMGMT", volume->label);
        if (volume->partition_type[0] != '\0')
            logger_write("DISKMGMT", volume->partition_type);
        if (volume->uuid[0] != '\0') logger_write("DISKMGMT", volume->uuid);
        logger_write_hex("DISKMGMT", "filesystem", (UINT64)volume->fs_type);
        logger_write_hex("DISKMGMT", "read only", (UINT64)volume->read_only);
        logger_write_hex("DISKMGMT", "no exec", (UINT64)volume->no_exec);
    }
    return 1;
}

int disk_management_list_capabilities(void)
{
    UINT32 index;
    for (index = 0; index < block_device_count(); index++) {
        ASAS_BLOCK_DEVICE *device = block_device_get(index);
        if (device == 0) continue;
        logger_write("DISKMGMT", device->name);
        logger_write_hex("DISKMGMT", "read only",
            (UINT64)block_device_has_capability(device, BLOCK_DEVICE_FLAG_READ_ONLY));
        logger_write_hex("DISKMGMT", "removable",
            (UINT64)block_device_has_capability(device, BLOCK_DEVICE_FLAG_REMOVABLE));
        logger_write_hex("DISKMGMT", "optical",
            (UINT64)block_device_has_capability(device, BLOCK_DEVICE_FLAG_OPTICAL));
        logger_write_hex("DISKMGMT", "hot plug",
            (UINT64)block_device_has_capability(device, BLOCK_DEVICE_FLAG_HOT_PLUG));
        logger_write_hex("DISKMGMT", "write protected",
            (UINT64)((device->flags & BLOCK_DEVICE_FLAG_READ_ONLY) != 0));
        logger_write_hex("DISKMGMT", "mounted", (UINT64)dm_is_mounted(device));
    }
    return 1;
}

const char *disk_management_read_only_reason(const char *device_name)
{
    ASAS_BLOCK_DEVICE *device = dm_find_device(device_name);
    UINT32 index;
    if (device == 0) return "unknown device";
    if ((device->flags & BLOCK_DEVICE_FLAG_OPTICAL) != 0)
        return "physical optical media is read-only";
    if ((device->flags & BLOCK_DEVICE_FLAG_READ_ONLY) != 0)
        return "device is write-protected";
    for (index = 0; index < filesystem_mount_count(); index++) {
        ASAS_FILESYSTEM_MOUNT *mount = filesystem_get_mount(index);
        if (mount == 0 || !mount->active || mount->device != device ||
            mount->driver == 0) continue;
        if (dm_strings_equal(mount->driver->name, "ntfs") &&
            mount->context != 0) {
            return ntfs_context_read_only_reason_string(
                (NTFS_CONTEXT *)mount->context);
        }
        if ((mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0)
            return "temporary read-only: filesystem safety policy";
    }
    return "full read-write";
}

int disk_management_hotplug_rescan(void)
{
    return vfs_rescan_devices();
}

int disk_management_device_removed(const char *device_name)
{
    int affected = vfs_handle_device_removed(device_name);
    logger_write_hex("DISKMGMT", "device removal affected volumes",
                     (UINT64)(UINT32)affected);
    return affected;
}

int disk_management_validate(ASAS_DISK_MANAGEMENT_OP operation,
                             const char *target,
                             UINT32 flags)
{
    ASAS_BLOCK_DEVICE *device;
    (void)flags;
    if (target == 0 || target[0] == '\0') return 0;
    if (operation == DISK_MANAGEMENT_OP_UNMOUNT ||
        operation == DISK_MANAGEMENT_OP_REMOUNT ||
        operation == DISK_MANAGEMENT_OP_FS_CHECK ||
        operation == DISK_MANAGEMENT_OP_FS_REPAIR) {
        return target[0] == '/';
    }
    device = dm_find_device(target);
    if (device == 0) return 0;
    if (operation == DISK_MANAGEMENT_OP_FORMAT ||
        operation == DISK_MANAGEMENT_OP_PARTITION_CREATE ||
        operation == DISK_MANAGEMENT_OP_PARTITION_DELETE ||
        operation == DISK_MANAGEMENT_OP_PARTITION_RESIZE) {
        if ((device->flags & BLOCK_DEVICE_FLAG_READ_ONLY) != 0) return 0;
        if (dm_is_mounted(device)) return 0;
    }
    return 1;
}

int disk_management_dry_run(ASAS_DISK_MANAGEMENT_OP operation,
                            const char *target,
                            UINT32 flags)
{
    int ok = disk_management_validate(operation, target, flags);
    logger_write("DISKMGMT", ok ? "dry-run ok" : "dry-run rejected");
    logger_write_hex("DISKMGMT", "operation", (UINT64)operation);
    logger_write_hex("DISKMGMT", "flags", (UINT64)flags);
    if (target != 0) logger_write("DISKMGMT", target);
    return ok;
}

int disk_management_mount(const char *device_name, const char *mount_point,
                          UINT32 flags)
{
    if (!disk_management_validate(DISK_MANAGEMENT_OP_MOUNT,
                                  device_name, flags)) return 0;
    return vfs_mount_device(device_name, mount_point, flags);
}

int disk_management_unmount(const char *mount_point)
{
    if (!disk_management_validate(DISK_MANAGEMENT_OP_UNMOUNT,
                                  mount_point, 0)) return 0;
    return vfs_unmount(mount_point);
}

int disk_management_remount(const char *mount_point, UINT32 flags)
{
    if (!disk_management_validate(DISK_MANAGEMENT_OP_REMOUNT,
                                  mount_point, flags)) return 0;
    return vfs_remount(mount_point, flags);
}

int disk_management_format(const char *device_name, const char *fs_name,
                           int dry_run)
{
    if (!disk_management_validate(DISK_MANAGEMENT_OP_FORMAT,
                                  device_name, 0) ||
        fs_name == 0 ||
        !dm_supported_format_name(fs_name)) return 0;
    if (dry_run) {
        logger_write("DISKMGMT", "format dry-run ok");
        logger_write("DISKMGMT", device_name);
        logger_write("DISKMGMT", fs_name);
        return 1;
    }
    logger_write("DISKMGMT", "format engine not registered");
    return 0;
}

int disk_management_partition_mbr(const char *operation,
                                  const char *device_name,
                                  UINT32 slot, UINT8 type,
                                  UINT64 start_lba,
                                  UINT64 block_count,
                                  int dry_run)
{
    ASAS_BLOCK_DEVICE *device;
    if (!disk_management_validate(DISK_MANAGEMENT_OP_PARTITION_CREATE,
                                  device_name, 0) ||
        operation == 0 || slot >= 4) return 0;
    device = dm_find_device(device_name);
    if (dry_run) {
        logger_write("DISKMGMT", "partition mbr dry-run ok");
        logger_write("DISKMGMT", operation);
        return 1;
    }
    if (dm_strings_equal(operation, "create"))
        return partition_mbr_create(device, slot, type, start_lba, block_count);
    if (dm_strings_equal(operation, "delete"))
        return partition_mbr_delete(device, slot);
    if (dm_strings_equal(operation, "resize"))
        return partition_mbr_resize(device, slot, start_lba, block_count);
    return 0;
}

int disk_management_partition_gpt(const char *operation,
                                  const char *device_name,
                                  UINT32 slot, UINT64 start_lba,
                                  UINT64 block_count,
                                  int dry_run)
{
    static const UINT8 linux_type[16] = {
        0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
        0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
    };
    static const UINT8 unique_guid[16] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
    };
    static const UINT16 name[36] = {
        'A','S','A','S','-','D','A','T','A',0
    };
    ASAS_BLOCK_DEVICE *device;
    if (!disk_management_validate(DISK_MANAGEMENT_OP_PARTITION_CREATE,
                                  device_name, 0) ||
        operation == 0 || slot >= PARTITION_MAX_PER_DISK) return 0;
    device = dm_find_device(device_name);
    if (dry_run) {
        logger_write("DISKMGMT", "partition gpt dry-run ok");
        logger_write("DISKMGMT", operation);
        return 1;
    }
    if (dm_strings_equal(operation, "create"))
        return partition_gpt_create(device, slot, linux_type, unique_guid,
                                    name, start_lba, block_count);
    if (dm_strings_equal(operation, "delete"))
        return partition_gpt_delete(device, slot);
    if (dm_strings_equal(operation, "resize"))
        return partition_gpt_resize(device, slot, start_lba, block_count);
    return 0;
}

int disk_management_fs_check(const char *path)
{
    UINT64 handle;
    int ok;
    if (!disk_management_validate(DISK_MANAGEMENT_OP_FS_CHECK, path, 0))
        return 0;
    handle = vfs_open(path);
    ok = vfs_is_directory(path) || vfs_file_size(path) != 0 || handle != 0;
    if (handle != 0) (void)vfs_close(handle);
    logger_write("DISKMGMT", ok ? "fs check ok" : "fs check failed");
    return ok;
}

int disk_management_fs_repair(const char *path, int dry_run)
{
    if (!disk_management_validate(DISK_MANAGEMENT_OP_FS_REPAIR, path, 0))
        return 0;
    if (dry_run) {
        logger_write("DISKMGMT", "fs repair dry-run ok");
        return 1;
    }
    logger_write("DISKMGMT", "fs repair engine not registered");
    return 0;
}

int disk_management_self_test(void)
{
    return disk_management_dry_run(DISK_MANAGEMENT_OP_FS_CHECK, "/system", 0) &&
           !disk_management_validate(DISK_MANAGEMENT_OP_FORMAT, "", 0);
}
