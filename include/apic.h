#ifndef ASAS_APIC_H
#define ASAS_APIC_H

#include "uefi.h"

void apic_initialize(void);
UINT64 apic_timer_ticks(void);
void apic_timer_interrupt_handler(void);
UINT32 apic_current_id(void);
void apic_start_processor(UINT32 apic_id, UINT8 startup_vector);
void apic_eoi(void);

#endif
