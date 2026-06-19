#ifndef ASAS_SMP_H
#define ASAS_SMP_H

#include "acpi.h"
#include "boot_info.h"

int smp_start_secondary_processors(
    const ASAS_BOOT_INFO *boot_info,
    const ASAS_PROCESSOR_LIST *processors
);

UINT32 smp_online_processor_count(void);

#endif
