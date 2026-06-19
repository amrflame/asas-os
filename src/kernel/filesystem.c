#include "filesystem.h"
#include "fat32.h"
#include "ntfs.h"
#include "exfat.h"
#include "iso9660.h"
#include "udf.h"
#include "ext2.h"
#include "heap.h"
#include "logger.h"

static void copy_entry_name(char *destination, const char *source)
{
    UINT32 index = 0;
    while (index + 1 < 256 && source[index]) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

static const ASAS_FILESYSTEM_DRIVER *drivers[FILESYSTEM_MAX_DRIVERS];
static ASAS_FILESYSTEM_MOUNT mounts[FILESYSTEM_MAX_MOUNTS];
static UINT32 driver_count;
static UINT32 mount_count;
static UINT32 next_generation = 1;

#pragma intrinsic(_InterlockedExchange)
long _InterlockedExchange(long volatile *target, long value);

static void lock_value(volatile long *value)
{
    while (_InterlockedExchange(value, 1) != 0) {
    }
}

static void unlock_value(volatile long *value)
{
    (void)_InterlockedExchange(value, 0);
}

static void mount_lock(ASAS_FILESYSTEM_MOUNT *mount) { lock_value(&mount->lock_value); }
static void mount_unlock(ASAS_FILESYSTEM_MOUNT *mount) { unlock_value(&mount->lock_value); }

static int fat32_probe_adapter(ASAS_BLOCK_DEVICE *device)
{
    UINT8 sector[4096];
    if (device == 0 || device->logical_block_size < 512 ||
        device->logical_block_size > sizeof(sector) ||
        !block_device_read(device, 0, 1, sector)) return 0;
    return sector[22] == 0 && sector[23] == 0 &&
           sector[82] == 'F' && sector[83] == 'A' && sector[84] == 'T' &&
           sector[85] == '3' && sector[86] == '2';
}

static int fat32_mount_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    mount->context = fat32_context_create(mount->device);
    return mount->context != 0;
}

static int fat32_stat_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                              ASAS_FILE_STAT *stat)
{
    FAT32_CONTEXT *context = (FAT32_CONTEXT *)mount->context;
    if (!fat32_context_exists(context, path)) return 0;
    stat->exists = 1;
    stat->is_directory = (UINT8)fat32_context_is_directory(context, path);
    stat->size = stat->is_directory ? 0 : fat32_context_file_size(context, path);
    stat->read_only = 0;
    return 1;
}

static UINT64 fat32_read_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                                 void *buffer, UINT64 capacity)
{
    return fat32_context_read_file((FAT32_CONTEXT *)mount->context,
                                   path, buffer, capacity);
}

static UINT64 fat32_list_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                                 ASAS_FILESYSTEM_ENTRY *entries, UINT64 capacity)
{
    FAT32_FILE_INFO *items;
    UINT64 count;
    UINT64 index;
    if (capacity == 0) return 0;
    items = (FAT32_FILE_INFO *)kmalloc((UINTN)(capacity * sizeof(FAT32_FILE_INFO)));
    if (items == 0) return 0;
    count = fat32_context_list_directory((FAT32_CONTEXT *)mount->context,
                                         path, items, capacity);
    for (index = 0; index < count; index++) {
        copy_entry_name(entries[index].name, items[index].name);
        entries[index].size = items[index].size;
        entries[index].is_directory = items[index].is_directory;
        entries[index].read_only = items[index].read_only;
    }
    kfree(items);
    return count;
}

static int fat32_write_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                               const void *buffer, UINT64 size)
{
    FAT32_CONTEXT *context = (FAT32_CONTEXT *)mount->context;
    return fat32_context_write_file(context, path, buffer, size) &&
           fat32_context_sync(context);
}

static int fat32_unlink_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    FAT32_CONTEXT *context = (FAT32_CONTEXT *)mount->context;
    return fat32_context_delete_file(context, path) && fat32_context_sync(context);
}

static int fat32_mkdir_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    FAT32_CONTEXT *context = (FAT32_CONTEXT *)mount->context;
    return fat32_context_create_directory(context, path) && fat32_context_sync(context);
}

static int fat32_rmdir_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    FAT32_CONTEXT *context = (FAT32_CONTEXT *)mount->context;
    return fat32_context_delete_directory(context, path) && fat32_context_sync(context);
}

static int fat32_rename_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *source,
                                const char *destination)
{
    FAT32_CONTEXT *context = (FAT32_CONTEXT *)mount->context;
    return fat32_context_rename(context, source, destination) &&
           fat32_context_sync(context);
}

static int fat32_sync_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    return fat32_context_sync((FAT32_CONTEXT *)mount->context);
}

static int generic_sync(ASAS_FILESYSTEM_MOUNT *mount)
{
    return block_device_flush(mount->device);
}

static const ASAS_FILESYSTEM_DRIVER fat32_driver = {
    "fat32", 0, fat32_probe_adapter, fat32_mount_adapter, fat32_stat_adapter,
    fat32_read_adapter, fat32_list_adapter, fat32_write_adapter,
    fat32_unlink_adapter, fat32_mkdir_adapter, fat32_rmdir_adapter,
    fat32_rename_adapter, fat32_sync_adapter
};

static int ntfs_probe_adapter(ASAS_BLOCK_DEVICE *device)
{
    UINT8 sector[4096];
    if (device == 0 || device->logical_block_size < 512 ||
        device->logical_block_size > sizeof(sector) ||
        !block_device_read(device, 0, 1, sector)) return 0;
    return sector[3] == 'N' && sector[4] == 'T' && sector[5] == 'F' &&
           sector[6] == 'S' && sector[7] == ' ' && sector[8] == ' ' &&
           sector[9] == ' ' && sector[10] == ' ';
}

static int ntfs_mount_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    mount->context = ntfs_context_create(mount->device);
    if (mount->context != 0 &&
        !ntfs_context_is_writable((NTFS_CONTEXT *)mount->context)) {
        mount->flags |= FILESYSTEM_FLAG_READ_ONLY;
        logger_write("NTFS",
            ntfs_context_read_only_reason_string((NTFS_CONTEXT *)mount->context));
    }
    return mount->context != 0;
}

static int ntfs_stat_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                             ASAS_FILE_STAT *stat)
{
    NTFS_CONTEXT *context = (NTFS_CONTEXT *)mount->context;
    if (!ntfs_context_exists(context, path)) return 0;
    stat->exists = 1;
    stat->is_directory = (UINT8)ntfs_context_is_directory(context, path);
    stat->size = stat->is_directory ? 0 : ntfs_context_file_size(context, path);
    stat->read_only = (mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0;
    return 1;
}

static UINT64 ntfs_read_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                                void *buffer, UINT64 capacity)
{
    return ntfs_context_read_file((NTFS_CONTEXT *)mount->context,
                                  path, buffer, capacity);
}

static int ntfs_write_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                              const void *buffer, UINT64 size)
{
    return ntfs_context_write_file((NTFS_CONTEXT *)mount->context,
                                   path, buffer, size);
}

static int ntfs_unlink_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    return ntfs_context_delete_file((NTFS_CONTEXT *)mount->context, path);
}

static int ntfs_mkdir_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    return ntfs_context_create_directory((NTFS_CONTEXT *)mount->context, path);
}

static int ntfs_rmdir_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    return ntfs_context_delete_directory((NTFS_CONTEXT *)mount->context, path);
}

static int ntfs_rename_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                               const char *source, const char *destination)
{
    return ntfs_context_rename((NTFS_CONTEXT *)mount->context,
                               source, destination);
}

static UINT64 ntfs_list_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                                ASAS_FILESYSTEM_ENTRY *entries, UINT64 capacity)
{
    NTFS_FILE_INFO *items;
    UINT64 count;
    UINT64 index;
    if (capacity == 0) return 0;
    items = (NTFS_FILE_INFO *)kmalloc((UINTN)(capacity * sizeof(NTFS_FILE_INFO)));
    if (items == 0) return 0;
    count = ntfs_context_list_directory((NTFS_CONTEXT *)mount->context,
                                        path, items, capacity);
    for (index = 0; index < count; index++) {
        copy_entry_name(entries[index].name, items[index].name);
        entries[index].size = items[index].size;
        entries[index].is_directory = items[index].is_directory;
        entries[index].read_only = items[index].read_only;
    }
    kfree(items);
    return count;
}

static const ASAS_FILESYSTEM_DRIVER ntfs_driver = {
    "ntfs", 0,
    ntfs_probe_adapter, ntfs_mount_adapter, ntfs_stat_adapter,
    ntfs_read_adapter, ntfs_list_adapter, ntfs_write_adapter,
    ntfs_unlink_adapter, ntfs_mkdir_adapter, ntfs_rmdir_adapter,
    ntfs_rename_adapter, generic_sync
};

static int exfat_probe_adapter(ASAS_BLOCK_DEVICE *device)
{
    return exfat_probe(device);
}

static int exfat_mount_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    mount->context = exfat_context_create(mount->device);
    return mount->context != 0;
}

static int exfat_stat_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                              ASAS_FILE_STAT *stat)
{
    EXFAT_CONTEXT *context = (EXFAT_CONTEXT *)mount->context;
    if (!exfat_context_exists(context, path)) return 0;
    stat->exists = 1;
    stat->is_directory = (UINT8)exfat_context_is_directory(context, path);
    stat->size = stat->is_directory ? 0 : exfat_context_file_size(context, path);
    stat->read_only = 0;
    return 1;
}

static UINT64 exfat_read_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                                 void *buffer, UINT64 capacity)
{
    return exfat_context_read_file((EXFAT_CONTEXT *)mount->context,
                                   path, buffer, capacity);
}

static UINT64 exfat_list_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                                 ASAS_FILESYSTEM_ENTRY *entries, UINT64 capacity)
{
    EXFAT_FILE_INFO *items;
    UINT64 count;
    UINT64 index;
    if (capacity == 0) return 0;
    items = (EXFAT_FILE_INFO *)kmalloc((UINTN)(capacity * sizeof(*items)));
    if (items == 0) return 0;
    count = exfat_context_list_directory((EXFAT_CONTEXT *)mount->context,
                                         path, items, capacity);
    for (index = 0; index < count; index++) {
        copy_entry_name(entries[index].name, items[index].name);
        entries[index].size = items[index].size;
        entries[index].is_directory = items[index].is_directory;
        entries[index].read_only = 0;
    }
    kfree(items);
    return count;
}

static int exfat_write_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                               const void *buffer, UINT64 size)
{
    return exfat_context_write_file((EXFAT_CONTEXT *)mount->context,
                                    path, buffer, size);
}

static int exfat_unlink_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    return exfat_context_delete_file((EXFAT_CONTEXT *)mount->context, path);
}

static int exfat_mkdir_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    return exfat_context_create_directory((EXFAT_CONTEXT *)mount->context, path);
}

static int exfat_rmdir_adapter(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    return exfat_context_delete_directory((EXFAT_CONTEXT *)mount->context, path);
}

static int exfat_rename_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                const char *source, const char *destination)
{
    return exfat_context_rename((EXFAT_CONTEXT *)mount->context,
                                source, destination);
}

static int exfat_sync_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    return exfat_context_sync((EXFAT_CONTEXT *)mount->context);
}

static const ASAS_FILESYSTEM_DRIVER exfat_driver = {
    "exfat", 0,
    exfat_probe_adapter, exfat_mount_adapter, exfat_stat_adapter,
    exfat_read_adapter, exfat_list_adapter, exfat_write_adapter,
    exfat_unlink_adapter, exfat_mkdir_adapter, exfat_rmdir_adapter,
    exfat_rename_adapter, exfat_sync_adapter
};

static int iso9660_probe_adapter(ASAS_BLOCK_DEVICE *device)
{
    return iso9660_probe(device);
}

static int iso9660_mount_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    mount->context = iso9660_context_create(mount->device);
    return mount->context != 0;
}

static int iso9660_stat_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                const char *path, ASAS_FILE_STAT *stat)
{
    ISO9660_CONTEXT *context = (ISO9660_CONTEXT *)mount->context;
    if (!iso9660_context_exists(context, path)) return 0;
    stat->exists = 1;
    stat->is_directory = (UINT8)iso9660_context_is_directory(context, path);
    stat->size = stat->is_directory ? 0 : iso9660_context_file_size(context, path);
    stat->read_only = (UINT8)iso9660_context_is_read_only(context, path);
    return 1;
}

static UINT64 iso9660_read_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                   const char *path, void *buffer,
                                   UINT64 capacity)
{
    return iso9660_context_read_file((ISO9660_CONTEXT *)mount->context,
                                     path, buffer, capacity);
}

static UINT64 iso9660_list_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                   const char *path,
                                   ASAS_FILESYSTEM_ENTRY *entries,
                                   UINT64 capacity)
{
    ISO9660_FILE_INFO *items;
    UINT64 count;
    UINT64 index;
    if (capacity == 0) return 0;
    items = (ISO9660_FILE_INFO *)kmalloc((UINTN)(capacity * sizeof(*items)));
    if (items == 0) return 0;
    count = iso9660_context_list_directory((ISO9660_CONTEXT *)mount->context,
                                           path, items, capacity);
    for (index = 0; index < count; index++) {
        copy_entry_name(entries[index].name, items[index].name);
        entries[index].size = items[index].size;
        entries[index].is_directory = items[index].is_directory;
        entries[index].read_only = items[index].read_only;
    }
    kfree(items);
    return count;
}

static int iso9660_write_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                 const char *path, const void *buffer,
                                 UINT64 size)
{
    return iso9660_context_write_file((ISO9660_CONTEXT *)mount->context,
                                      path, buffer, size);
}

static int iso9660_unlink_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                  const char *path)
{
    return iso9660_context_delete_file((ISO9660_CONTEXT *)mount->context, path);
}

static int iso9660_mkdir_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                 const char *path)
{
    return iso9660_context_create_directory((ISO9660_CONTEXT *)mount->context,
                                            path);
}

static int iso9660_rmdir_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                 const char *path)
{
    return iso9660_context_delete_directory((ISO9660_CONTEXT *)mount->context,
                                            path);
}

static int iso9660_rename_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                  const char *source,
                                  const char *destination)
{
    return iso9660_context_rename((ISO9660_CONTEXT *)mount->context,
                                  source, destination);
}

static int iso9660_sync_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    return iso9660_context_sync((ISO9660_CONTEXT *)mount->context);
}

static const ASAS_FILESYSTEM_DRIVER iso9660_driver = {
    "iso9660", 0,
    iso9660_probe_adapter, iso9660_mount_adapter, iso9660_stat_adapter,
    iso9660_read_adapter, iso9660_list_adapter, iso9660_write_adapter,
    iso9660_unlink_adapter, iso9660_mkdir_adapter, iso9660_rmdir_adapter,
    iso9660_rename_adapter, iso9660_sync_adapter
};

static int udf_probe_adapter(ASAS_BLOCK_DEVICE *device)
{
    return udf_probe(device);
}

static int udf_mount_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    mount->context = udf_context_create(mount->device);
    return mount->context != 0;
}

static int udf_stat_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                            const char *path, ASAS_FILE_STAT *stat)
{
    UDF_CONTEXT *context = (UDF_CONTEXT *)mount->context;
    if (!udf_context_exists(context, path)) return 0;
    stat->exists = 1;
    stat->is_directory = (UINT8)udf_context_is_directory(context, path);
    stat->size = stat->is_directory ? 0 : udf_context_file_size(context, path);
    stat->read_only = (mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0;
    return 1;
}

static UINT64 udf_read_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                               const char *path, void *buffer,
                               UINT64 capacity)
{
    return udf_context_read_file((UDF_CONTEXT *)mount->context,
                                 path, buffer, capacity);
}

static UINT64 udf_list_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                               const char *path,
                               ASAS_FILESYSTEM_ENTRY *entries,
                               UINT64 capacity)
{
    UDF_FILE_INFO *items;
    UINT64 count;
    UINT64 index;
    if (capacity == 0) return 0;
    items = (UDF_FILE_INFO *)kmalloc((UINTN)(capacity * sizeof(*items)));
    if (items == 0) return 0;
    count = udf_context_list_directory((UDF_CONTEXT *)mount->context,
                                       path, items, capacity);
    for (index = 0; index < count; index++) {
        copy_entry_name(entries[index].name, items[index].name);
        entries[index].size = items[index].size;
        entries[index].is_directory = items[index].is_directory;
        entries[index].read_only = items[index].read_only;
    }
    kfree(items);
    return count;
}

static int udf_write_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                             const char *path, const void *buffer,
                             UINT64 size)
{
    return udf_context_write_file((UDF_CONTEXT *)mount->context,
                                  path, buffer, size);
}

static int udf_unlink_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                              const char *path)
{
    return udf_context_delete_file((UDF_CONTEXT *)mount->context, path);
}

static int udf_mkdir_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                             const char *path)
{
    return udf_context_create_directory((UDF_CONTEXT *)mount->context, path);
}

static int udf_rmdir_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                             const char *path)
{
    return udf_context_delete_directory((UDF_CONTEXT *)mount->context, path);
}

static int udf_rename_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                              const char *source,
                              const char *destination)
{
    return udf_context_rename((UDF_CONTEXT *)mount->context,
                              source, destination);
}

static int udf_sync_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    return udf_context_sync((UDF_CONTEXT *)mount->context);
}

static const ASAS_FILESYSTEM_DRIVER udf_driver = {
    "udf", 0,
    udf_probe_adapter, udf_mount_adapter, udf_stat_adapter,
    udf_read_adapter, udf_list_adapter, udf_write_adapter,
    udf_unlink_adapter, udf_mkdir_adapter, udf_rmdir_adapter,
    udf_rename_adapter, udf_sync_adapter
};

static int ext2_probe_adapter(ASAS_BLOCK_DEVICE *device)
{
    return ext2_probe(device);
}

static int ext2_mount_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    mount->context = ext2_context_create(mount->device);
    if (mount->context != 0 &&
        ext2_context_is_read_only((EXT2_CONTEXT *)mount->context))
        mount->flags |= FILESYSTEM_FLAG_READ_ONLY;
    return mount->context != 0;
}

static int ext2_stat_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                             const char *path, ASAS_FILE_STAT *stat)
{
    EXT2_CONTEXT *context = (EXT2_CONTEXT *)mount->context;
    if (!ext2_context_exists(context, path)) return 0;
    stat->exists = 1;
    stat->is_directory = (UINT8)ext2_context_is_directory(context, path);
    stat->size = stat->is_directory ? 0 : ext2_context_file_size(context, path);
    stat->read_only = (mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0;
    return 1;
}

static UINT64 ext2_read_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                const char *path, void *buffer,
                                UINT64 capacity)
{
    return ext2_context_read_file((EXT2_CONTEXT *)mount->context,
                                  path, buffer, capacity);
}

static UINT64 ext2_list_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                                const char *path,
                                ASAS_FILESYSTEM_ENTRY *entries,
                                UINT64 capacity)
{
    EXT2_FILE_INFO *items;
    UINT64 count;
    UINT64 index;
    if (capacity == 0) return 0;
    items = (EXT2_FILE_INFO *)kmalloc((UINTN)(capacity * sizeof(*items)));
    if (items == 0) return 0;
    count = ext2_context_list_directory((EXT2_CONTEXT *)mount->context,
                                        path, items, capacity);
    for (index = 0; index < count; index++) {
        copy_entry_name(entries[index].name, items[index].name);
        entries[index].size = items[index].size;
        entries[index].is_directory = items[index].is_directory;
        entries[index].read_only = items[index].read_only;
    }
    kfree(items);
    return count;
}

static int ext2_write_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                              const char *path, const void *buffer,
                              UINT64 size)
{
    return ext2_context_write_file((EXT2_CONTEXT *)mount->context,
                                   path, buffer, size);
}

static int ext2_unlink_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                               const char *path)
{
    return ext2_context_delete_file((EXT2_CONTEXT *)mount->context, path);
}

static int ext2_mkdir_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                              const char *path)
{
    return ext2_context_create_directory((EXT2_CONTEXT *)mount->context, path);
}

static int ext2_rmdir_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                              const char *path)
{
    return ext2_context_delete_directory((EXT2_CONTEXT *)mount->context, path);
}

static int ext2_rename_adapter(ASAS_FILESYSTEM_MOUNT *mount,
                               const char *source,
                               const char *destination)
{
    return ext2_context_rename((EXT2_CONTEXT *)mount->context,
                               source, destination);
}

static int ext2_sync_adapter(ASAS_FILESYSTEM_MOUNT *mount)
{
    return ext2_context_sync((EXT2_CONTEXT *)mount->context);
}

static const ASAS_FILESYSTEM_DRIVER ext2_driver = {
    "ext2", 0,
    ext2_probe_adapter, ext2_mount_adapter, ext2_stat_adapter,
    ext2_read_adapter, ext2_list_adapter, ext2_write_adapter,
    ext2_unlink_adapter, ext2_mkdir_adapter, ext2_rmdir_adapter,
    ext2_rename_adapter, ext2_sync_adapter
};

void filesystem_initialize(void)
{
    UINT32 index;
    for (index = 0; index < FILESYSTEM_MAX_MOUNTS; index++) {
        if (mounts[index].active) (void)filesystem_unmount(&mounts[index]);
    }
    driver_count = 0;
}

int filesystem_register(const ASAS_FILESYSTEM_DRIVER *driver)
{
    if (driver == 0 || driver->name == 0 || driver->probe == 0 ||
        driver->mount == 0 || driver_count >= FILESYSTEM_MAX_DRIVERS) return 0;
    drivers[driver_count++] = driver;
    return 1;
}

void filesystem_register_builtin_drivers(void)
{
    (void)filesystem_register(&fat32_driver);
    (void)filesystem_register(&ntfs_driver);
    (void)filesystem_register(&exfat_driver);
    (void)filesystem_register(&ext2_driver);
    (void)filesystem_register(&udf_driver);
    (void)filesystem_register(&iso9660_driver);
}

const ASAS_FILESYSTEM_DRIVER *filesystem_probe(ASAS_BLOCK_DEVICE *device)
{
    UINT32 index;
    const ASAS_FILESYSTEM_DRIVER *result = 0;
    for (index = 0; index < driver_count; index++) {
        if (drivers[index]->probe(device)) {
            result = drivers[index];
            break;
        }
    }
    return result;
}

ASAS_FILESYSTEM_MOUNT *filesystem_mount(ASAS_BLOCK_DEVICE *device,
                                       const ASAS_FILESYSTEM_DRIVER *driver,
                                       UINT32 flags)
{
    ASAS_FILESYSTEM_MOUNT *mount;
    UINT32 slot;
    if (device == 0 || driver == 0 || mount_count >= FILESYSTEM_MAX_MOUNTS) return 0;
    for (slot = 0; slot < FILESYSTEM_MAX_MOUNTS; slot++)
        if (!mounts[slot].active) break;
    if (slot == FILESYSTEM_MAX_MOUNTS) return 0;
    mount = &mounts[slot];
    mount->id = slot;
    mount->generation = next_generation++;
    if (next_generation == 0) next_generation = 1;
    mount->reference_count = 0;
    mount->device = device;
    mount->driver = driver;
    mount->flags = flags | driver->flags;
    mount->context = 0;
    mount->lock_value = 0;
    mount->active = 1;
    if (!driver->mount(mount)) {
        mount->active = 0;
        return 0;
    }
    mount_count++;
    return mount;
}

UINT32 filesystem_mount_count(void) { return mount_count; }

ASAS_FILESYSTEM_MOUNT *filesystem_get_mount(UINT32 index)
{
    UINT32 slot;
    UINT32 found = 0;
    for (slot = 0; slot < FILESYSTEM_MAX_MOUNTS; slot++) {
        if (!mounts[slot].active) continue;
        if (found++ == index) return &mounts[slot];
    }
    return 0;
}

int filesystem_device_is_mounted(const ASAS_BLOCK_DEVICE *device)
{
    UINT32 index;
    if (device == 0) return 0;
    for (index = 0; index < FILESYSTEM_MAX_MOUNTS; index++) {
        if (!mounts[index].active) continue;
        ASAS_BLOCK_DEVICE *mounted = mounts[index].device;
        if (mounted == device || (mounted != 0 && mounted->parent == device)) return 1;
    }
    return 0;
}

int filesystem_unmount(ASAS_FILESYSTEM_MOUNT *mount)
{
    if (mount == 0 || mount->id >= FILESYSTEM_MAX_MOUNTS ||
        &mounts[mount->id] != mount || !mount->active ||
        mount->reference_count != 0) return 0;

    mount_lock(mount);
    if (mount->driver == &fat32_driver) {
        if (!fat32_context_sync((FAT32_CONTEXT *)mount->context)) {
            mount_unlock(mount);
            return 0;
        }
        fat32_context_destroy((FAT32_CONTEXT *)mount->context);
    } else if (mount->driver == &ntfs_driver) {
        ntfs_context_destroy((NTFS_CONTEXT *)mount->context);
    } else if (mount->driver == &exfat_driver) {
        exfat_context_destroy((EXFAT_CONTEXT *)mount->context);
    } else if (mount->driver == &iso9660_driver) {
        iso9660_context_destroy((ISO9660_CONTEXT *)mount->context);
    } else if (mount->driver == &udf_driver) {
        udf_context_destroy((UDF_CONTEXT *)mount->context);
    } else if (mount->driver == &ext2_driver) {
        ext2_context_destroy((EXT2_CONTEXT *)mount->context);
    }
    mount_count--;
    mount->device = 0;
    mount->driver = 0;
    mount->context = 0;
    mount->active = 0;
    mount_unlock(mount);
    return 1;
}

int filesystem_mount_acquire(ASAS_FILESYSTEM_MOUNT *mount)
{
    if (mount == 0 || mount->id >= FILESYSTEM_MAX_MOUNTS ||
        &mounts[mount->id] != mount) return 0;
    mount_lock(mount);
    if (!mount->active) { mount_unlock(mount); return 0; }
    mount->reference_count++;
    mount_unlock(mount);
    return 1;
}

void filesystem_mount_release(ASAS_FILESYSTEM_MOUNT *mount)
{
    if (mount == 0 || mount->id >= FILESYSTEM_MAX_MOUNTS ||
        &mounts[mount->id] != mount) return;
    mount_lock(mount);
    if (mount->active && mount->reference_count != 0) mount->reference_count--;
    mount_unlock(mount);
}

int filesystem_stat(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                    ASAS_FILE_STAT *stat)
{
    int result;
    if (mount == 0 || !mount->active || stat == 0 ||
        mount->driver->stat == 0) return 0;
    stat->exists = 0;
    mount_lock(mount);
    result = mount->driver->stat(mount, path, stat);
    mount_unlock(mount);
    return result;
}

UINT64 filesystem_read(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                       void *buffer, UINT64 capacity)
{
    UINT64 result;
    if (mount == 0 || !mount->active || mount->driver->read == 0) return 0;
    mount_lock(mount);
    result = mount->driver->read(mount, path, buffer, capacity);
    mount_unlock(mount);
    return result;
}

UINT64 filesystem_list(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                       ASAS_FILESYSTEM_ENTRY *entries, UINT64 capacity)
{
    UINT64 result;
    if (mount == 0 || !mount->active || mount->driver->list == 0) return 0;
    mount_lock(mount);
    result = mount->driver->list(mount, path, entries, capacity);
    mount_unlock(mount);
    return result;
}

int filesystem_write(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                     const void *buffer, UINT64 size)
{
    int result;
    if (mount == 0 || !mount->active ||
        (mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0 ||
        mount->driver->write == 0) return 0;
    mount_lock(mount);
    result = mount->driver->write(mount, path, buffer, size);
    mount_unlock(mount);
    return result;
}

int filesystem_unlink(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    int result;
    if (mount == 0 || !mount->active ||
        (mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0 ||
        mount->driver->unlink == 0) return 0;
    mount_lock(mount);
    result = mount->driver->unlink(mount, path);
    mount_unlock(mount);
    return result;
}

int filesystem_mkdir(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    int result;
    if (mount == 0 || !mount->active ||
        (mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0 ||
        mount->driver->mkdir == 0) return 0;
    mount_lock(mount);
    result = mount->driver->mkdir(mount, path);
    mount_unlock(mount);
    return result;
}

int filesystem_rmdir(ASAS_FILESYSTEM_MOUNT *mount, const char *path)
{
    int result;
    if (mount == 0 || !mount->active ||
        (mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0 ||
        mount->driver->rmdir == 0) return 0;
    mount_lock(mount);
    result = mount->driver->rmdir(mount, path);
    mount_unlock(mount);
    return result;
}

int filesystem_rename(ASAS_FILESYSTEM_MOUNT *mount, const char *source,
                      const char *destination)
{
    int result;
    if (mount == 0 || !mount->active ||
        (mount->flags & FILESYSTEM_FLAG_READ_ONLY) != 0 ||
        mount->driver->rename == 0) return 0;
    mount_lock(mount);
    result = mount->driver->rename(mount, source, destination);
    mount_unlock(mount);
    return result;
}

int filesystem_sync(ASAS_FILESYSTEM_MOUNT *mount)
{
    int result;
    if (mount == 0 || !mount->active || mount->driver->sync == 0) return 0;
    mount_lock(mount);
    result = mount->driver->sync(mount);
    mount_unlock(mount);
    return result;
}
