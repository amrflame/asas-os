#ifndef ASAS_NVME_H
#define ASAS_NVME_H

#include "pci.h"
#include "memory.h"

int nvme_initialize(const ASAS_PCI_DEVICE *device, ASAS_FRAME_ALLOCATOR *allocator);
int nvme_read_sector(UINT64 lba, void *buffer);
int nvme_write_sector(UINT64 lba, const void *buffer);
int nvme_flush(void);
int nvme_register_block_device(void);

#endif
