#ifndef ASAS_AHCI_H
#define ASAS_AHCI_H

#include "pci.h"

UINT32 ahci_initialize(const ASAS_PCI_DEVICE *device);
int ahci_read_sector(UINT64 lba, void *buffer);
int ahci_write_sector(UINT64 lba, const void *buffer);
int ahci_flush(void);
int ahci_register_block_device(void);

#endif
