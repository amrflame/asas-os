#include "cpu.h"

#pragma intrinsic(__cpuid)
void __cpuid(int registers[4], int leaf);

static void copy_register(char *destination, int value)
{
    destination[0] = (char)(value & 0xFF);
    destination[1] = (char)((value >> 8) & 0xFF);
    destination[2] = (char)((value >> 16) & 0xFF);
    destination[3] = (char)((value >> 24) & 0xFF);
}

void cpu_detect(ASAS_CPU_INFO *info)
{
    int registers[4];

    __cpuid(registers, 0);
    info->max_basic_leaf = (UINT32)registers[0];
    copy_register(&info->vendor[0], registers[1]);
    copy_register(&info->vendor[4], registers[3]);
    copy_register(&info->vendor[8], registers[2]);
    info->vendor[12] = '\0';

    info->has_apic = 0;
    info->has_x2apic = 0;
    info->has_tsc = 0;
    info->has_msr = 0;
    info->has_nx = 0;

    if (info->max_basic_leaf >= 1) {
        __cpuid(registers, 1);
        info->has_tsc = ((UINT32)registers[3] >> 4) & 1U;
        info->has_msr = ((UINT32)registers[3] >> 5) & 1U;
        info->has_apic = ((UINT32)registers[3] >> 9) & 1U;
        info->has_x2apic = ((UINT32)registers[2] >> 21) & 1U;
    }

    __cpuid(registers, (int)0x80000000U);
    if ((UINT32)registers[0] >= 0x80000001U) {
        __cpuid(registers, (int)0x80000001U);
        info->has_nx = ((UINT32)registers[3] >> 20) & 1U;
    }
}
