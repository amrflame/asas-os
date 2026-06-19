#ifndef ASAS_BLOCK_DEVICE_H
#define ASAS_BLOCK_DEVICE_H

#include "uefi.h"

#define BLOCK_DEVICE_MAX_COUNT 32

#define BLOCK_DEVICE_FLAG_READ_ONLY  0x01U
#define BLOCK_DEVICE_FLAG_REMOVABLE  0x02U
#define BLOCK_DEVICE_FLAG_OPTICAL    0x04U
#define BLOCK_DEVICE_FLAG_PARTITION  0x08U
#define BLOCK_DEVICE_FLAG_HOT_PLUG   0x10U
#define BLOCK_DEVICE_FLAG_NO_CACHE   0x20U

#define BLOCK_DEVICE_CAPABILITY_MASK (BLOCK_DEVICE_FLAG_READ_ONLY | \
                                      BLOCK_DEVICE_FLAG_REMOVABLE | \
                                      BLOCK_DEVICE_FLAG_OPTICAL | \
                                      BLOCK_DEVICE_FLAG_HOT_PLUG)

typedef struct ASAS_BLOCK_DEVICE ASAS_BLOCK_DEVICE;

typedef struct {
    UINT64 read_ops;
    UINT64 write_ops;
    UINT64 flush_ops;
    UINT64 blocks_read;
    UINT64 blocks_written;
    UINT64 cache_hits;
    UINT64 cache_misses;
    UINT64 read_ahead_ops;
    UINT64 retries;
    UINT64 errors;
    UINT64 out_of_bounds;
} ASAS_BLOCK_DEVICE_TELEMETRY;

typedef struct {
    int (*read_blocks)(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                       UINT32 count, void *buffer);
    int (*write_blocks)(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                        UINT32 count, const void *buffer);
    int (*flush)(ASAS_BLOCK_DEVICE *device);
} ASAS_BLOCK_DEVICE_OPS;

struct ASAS_BLOCK_DEVICE {
    UINT32 id;
    char name[16];
    UINT32 logical_block_size;
    UINT32 physical_block_size;
    UINT64 block_count;
    UINT32 flags;
    UINT8 target;
    UINT8 lun;
    UINT64 start_lba;
    ASAS_BLOCK_DEVICE *parent;
    const ASAS_BLOCK_DEVICE_OPS *ops;
    void *driver_context;
};

void block_device_initialize(void);
ASAS_BLOCK_DEVICE *block_device_register(const ASAS_BLOCK_DEVICE *description);
ASAS_BLOCK_DEVICE *block_device_register_partition(ASAS_BLOCK_DEVICE *parent,
                                                   UINT64 start_lba,
                                                   UINT64 block_count,
                                                   const char *name,
                                                   UINT32 flags);
UINT32 block_device_count(void);
ASAS_BLOCK_DEVICE *block_device_get(UINT32 index);
ASAS_BLOCK_DEVICE *block_device_find(const char *name);
int block_device_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                      UINT32 count, void *buffer);
int block_device_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                       UINT32 count, const void *buffer);
int block_device_flush(ASAS_BLOCK_DEVICE *device);
int block_device_flush_barrier(ASAS_BLOCK_DEVICE *device);
int block_device_get_telemetry(const ASAS_BLOCK_DEVICE *device,
                               ASAS_BLOCK_DEVICE_TELEMETRY *telemetry);
int block_device_has_capability(const ASAS_BLOCK_DEVICE *device,
                                UINT32 capability);
int block_device_self_test(void);

/* Compatibility registration for the storage drivers currently routed by
 * virtio_block.c. New drivers should register their own operations directly. */
int block_device_register_legacy_devices(void);

#endif
