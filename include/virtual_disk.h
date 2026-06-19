#ifndef ASAS_VIRTUAL_DISK_H
#define ASAS_VIRTUAL_DISK_H

#include "uefi.h"
#include "block_device.h"

#define VDISK_MAX_COUNT 8
#define VDISK_FLAG_READ_ONLY 0x01U

typedef struct {
    UINT8 active;
    UINT8 read_only;
    char name[16];
    char format[12];
    char path[256];
    UINT64 size;
    UINT64 file_size;
    ASAS_BLOCK_DEVICE *device;
} ASAS_VDISK_INFO;

void virtual_disk_initialize(void);
ASAS_BLOCK_DEVICE *virtual_disk_attach_raw(const char *path, UINT32 flags);
ASAS_BLOCK_DEVICE *virtual_disk_attach_fixed_vhd(const char *path, UINT32 flags);
ASAS_BLOCK_DEVICE *virtual_disk_attach_auto(const char *path, UINT32 flags);
int virtual_disk_detach(const char *name);
int virtual_disk_check(const char *name);
int virtual_disk_validate_image(const char *path, const char *format);
UINT32 virtual_disk_count(void);
const ASAS_VDISK_INFO *virtual_disk_get(UINT32 index);
const ASAS_VDISK_INFO *virtual_disk_find(const char *name);

#endif
