#ifndef ASAS_VFS_H
#define ASAS_VFS_H

#include "uefi.h"
#include "boot_info.h"

#define VFS_PERMISSION_READ    1
#define VFS_PERMISSION_WRITE   2
#define VFS_PERMISSION_EXECUTE 4

/* Volume filesystem types */
#define VFS_FS_FAT16  0
#define VFS_FS_FAT32  1
#define VFS_FS_NTFS   2
#define VFS_FS_EXFAT  3
#define VFS_FS_ISO9660 4
#define VFS_FS_UDF    5
#define VFS_FS_EXT2   6
#define VFS_FS_NONE   7
#define VFS_MAX_VOLUMES 8

typedef struct {
    char name[32];
    UINT64 size;
    UINT8 is_directory;
    UINT8 permissions;
} VFS_DIRECTORY_ENTRY;

typedef struct {
    UINT8  valid;
    UINT8  is_cdrom;
    UINT8  is_removable;
    UINT8  read_only;
    UINT8  no_exec;
    UINT8  target;
    UINT8  lun;
    UINT8  fs_type;          /* VFS_FS_* */
    char   mount_point[32];  /* /system, /data, /media/usb0, ... */
    char   label[64];
    char   device_name[16];
    char   partition_type[24];
    char   uuid[37];
} VFS_VOLUME_INFO;

void vfs_initialize(void);
void vfs_initialize_boot_fallback(void);
void vfs_set_boot_info(const ASAS_BOOT_INFO *boot_info);
UINT64 vfs_open(const char *path);
UINT64 vfs_read(UINT64 handle, void *buffer, UINT64 size);
int vfs_close(UINT64 handle);
int vfs_write_file(const char *path, const void *buffer, UINT64 size);
int vfs_delete_file(const char *path);
int vfs_create_directory(const char *path);
int vfs_delete_directory(const char *path);
int vfs_rename(const char *source, const char *destination);
const char *vfs_write_status_reason(const char *path);
int vfs_can_execute(const char *path);
int vfs_is_directory(const char *path);
UINT64 vfs_list_root(VFS_DIRECTORY_ENTRY *entries, UINT64 capacity);
UINT64 vfs_list_directory(const char *path, VFS_DIRECTORY_ENTRY *entries, UINT64 capacity);
UINT64 vfs_file_size(const char *path);
int vfs_self_test(void);
int vfs_ntfs_mutation_self_test(void);
int vfs_exfat_integration_self_test(void);
int vfs_mount_manager_self_test(void);

/* Multi-volume API */
int                    vfs_mount_all_volumes(void);
int                    vfs_rescan_devices(void);
int                    vfs_handle_device_removed(const char *device_name);
int                    vfs_remount(const char *mount_point, UINT32 flags);
int                    vfs_sync_path(const char *path);
int                    vfs_unmount(const char *mount_point);
int                    vfs_mount_device(const char *device_name,
                                        const char *mount_point,
                                        UINT32 flags);
int                    vfs_get_volume_count(void);
const VFS_VOLUME_INFO *vfs_get_volume(int index);

#endif
