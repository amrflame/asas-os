#ifndef ASAS_PARTITION_H
#define ASAS_PARTITION_H

#include "block_device.h"

#define PARTITION_MAX_PER_DISK 16

typedef enum {
    PARTITION_SCHEME_NONE = 0,
    PARTITION_SCHEME_MBR,
    PARTITION_SCHEME_GPT
} ASAS_PARTITION_SCHEME;

typedef enum {
    PARTITION_TYPE_UNKNOWN = 0,
    PARTITION_TYPE_EFI_SYSTEM,
    PARTITION_TYPE_MICROSOFT_RESERVED,
    PARTITION_TYPE_MICROSOFT_BASIC_DATA,
    PARTITION_TYPE_WINDOWS_RECOVERY,
    PARTITION_TYPE_LINUX_FILESYSTEM,
    PARTITION_TYPE_LINUX_SWAP
} ASAS_PARTITION_TYPE;

typedef struct {
    ASAS_BLOCK_DEVICE *device;
    ASAS_BLOCK_DEVICE *parent;
    ASAS_PARTITION_SCHEME scheme;
    UINT32 number;
    UINT8 type;
    UINT8 type_guid[16];
    UINT8 unique_guid[16];
    ASAS_PARTITION_TYPE known_type;
    char type_name[24];
    char label[64];
    char uuid[37];
    UINT64 start_lba;
    UINT64 block_count;
} ASAS_PARTITION_INFO;

void partition_manager_initialize(void);
int partition_scan_device(ASAS_BLOCK_DEVICE *device);
int partition_scan_all(void);
UINT32 partition_count(void);
const ASAS_PARTITION_INFO *partition_get(UINT32 index);
const ASAS_PARTITION_INFO *partition_find_by_device(const ASAS_BLOCK_DEVICE *device);
int partition_mbr_create(ASAS_BLOCK_DEVICE *device, UINT32 slot, UINT8 type,
                         UINT64 start_lba, UINT64 block_count);
int partition_mbr_delete(ASAS_BLOCK_DEVICE *device, UINT32 slot);
int partition_mbr_resize(ASAS_BLOCK_DEVICE *device, UINT32 slot,
                         UINT64 start_lba, UINT64 block_count);
int partition_gpt_create(ASAS_BLOCK_DEVICE *device, UINT32 slot,
                         const UINT8 type_guid[16],
                         const UINT8 unique_guid[16],
                         const UINT16 name[36], UINT64 start_lba,
                         UINT64 block_count);
int partition_gpt_delete(ASAS_BLOCK_DEVICE *device, UINT32 slot);
int partition_gpt_resize(ASAS_BLOCK_DEVICE *device, UINT32 slot,
                         UINT64 start_lba, UINT64 block_count);
int partition_self_test(void);

#endif
