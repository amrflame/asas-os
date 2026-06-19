#ifndef ASAS_ACPI_H
#define ASAS_ACPI_H

#include "uefi.h"

#define ASAS_MAX_PROCESSORS 32

typedef struct {
    UINT32 count;
    UINT32 apic_ids[ASAS_MAX_PROCESSORS];
} ASAS_PROCESSOR_LIST;

UINT32 acpi_discover_processors(UINT64 rsdp_address, ASAS_PROCESSOR_LIST *processors);
int acpi_discover_ioapic(UINT64 rsdp_address, UINT64 *ioapic_address, UINT32 *keyboard_gsi);

#endif
