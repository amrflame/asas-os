#ifndef ASAS_IOAPIC_H
#define ASAS_IOAPIC_H

#include "uefi.h"

void ioapic_route_irq(UINT64 ioapic_address, UINT32 gsi, UINT8 vector, UINT8 destination_apic_id);

#endif

