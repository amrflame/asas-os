#ifndef ASAS_IDE_ATA_H
#define ASAS_IDE_ATA_H

#include "uefi.h"

int ide_ata_initialize(void);
int ide_ata_read_sector(UINT64 lba, void *buffer);
int ide_ata_write_sector(UINT64 lba, const void *buffer);

#endif
