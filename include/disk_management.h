#ifndef ASAS_DISK_MANAGEMENT_H
#define ASAS_DISK_MANAGEMENT_H

#include "uefi.h"

typedef enum {
    DISK_MANAGEMENT_OP_MOUNT = 0,
    DISK_MANAGEMENT_OP_UNMOUNT,
    DISK_MANAGEMENT_OP_REMOUNT,
    DISK_MANAGEMENT_OP_FORMAT,
    DISK_MANAGEMENT_OP_PARTITION_CREATE,
    DISK_MANAGEMENT_OP_PARTITION_DELETE,
    DISK_MANAGEMENT_OP_PARTITION_RESIZE,
    DISK_MANAGEMENT_OP_FS_CHECK,
    DISK_MANAGEMENT_OP_FS_REPAIR
} ASAS_DISK_MANAGEMENT_OP;

int disk_management_list_disks(void);
int disk_management_list_partitions(void);
int disk_management_list_volumes(void);
int disk_management_list_capabilities(void);
const char *disk_management_read_only_reason(const char *device_name);
int disk_management_hotplug_rescan(void);
int disk_management_device_removed(const char *device_name);
int disk_management_validate(ASAS_DISK_MANAGEMENT_OP operation,
                             const char *target,
                             UINT32 flags);
int disk_management_dry_run(ASAS_DISK_MANAGEMENT_OP operation,
                            const char *target,
                            UINT32 flags);
int disk_management_mount(const char *device_name, const char *mount_point,
                          UINT32 flags);
int disk_management_unmount(const char *mount_point);
int disk_management_remount(const char *mount_point, UINT32 flags);
int disk_management_format(const char *device_name, const char *fs_name,
                           int dry_run);
int disk_management_partition_mbr(const char *operation,
                                  const char *device_name,
                                  UINT32 slot, UINT8 type,
                                  UINT64 start_lba,
                                  UINT64 block_count,
                                  int dry_run);
int disk_management_partition_gpt(const char *operation,
                                  const char *device_name,
                                  UINT32 slot, UINT64 start_lba,
                                  UINT64 block_count,
                                  int dry_run);
int disk_management_fs_check(const char *path);
int disk_management_fs_repair(const char *path, int dry_run);
int disk_management_self_test(void);

#endif
