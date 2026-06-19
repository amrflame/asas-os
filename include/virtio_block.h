#ifndef ASAS_VIRTIO_BLOCK_H
#define ASAS_VIRTIO_BLOCK_H

#include "memory.h"
#include "pci.h"
#include "hyperv_storage.h"

int virtio_block_initialize(const ASAS_PCI_DEVICE *device);
int virtio_block_register_device(void);
int virtio_block_legacy_backend_active(void);
int virtio_block_use_ide_ata(void);
int virtio_block_use_hyperv_storage(ASAS_FRAME_ALLOCATOR *allocator);
int virtio_block_use_ahci(const ASAS_PCI_DEVICE *device);
const char *virtio_block_backend_name(void);
int virtio_block_read_sector(UINT64 sector, void *buffer);
int virtio_block_write_sector(UINT64 sector, const void *buffer);

/* Multi-device selection -- routes target:lun to the active backend */
void                       virtio_block_select_device(UINT8 target, UINT8 lun);
UINT8                      virtio_block_get_current_target(void);
UINT8                      virtio_block_get_current_lun(void);
int                        virtio_block_get_storage_device_count(void);
const ASAS_STORAGE_DEVICE *virtio_block_get_storage_devices(void);

#endif
