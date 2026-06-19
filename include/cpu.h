#ifndef ASAS_CPU_H
#define ASAS_CPU_H

#include "uefi.h"

typedef struct {
    char vendor[13];
    UINT32 max_basic_leaf;
    UINT32 has_apic;
    UINT32 has_x2apic;
    UINT32 has_tsc;
    UINT32 has_msr;
    UINT32 has_nx;
} ASAS_CPU_INFO;

void cpu_detect(ASAS_CPU_INFO *info);

#endif
