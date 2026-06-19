#include "smp.h"
#include "apic.h"
#include "logger.h"
#include "panic.h"

#pragma intrinsic(__readcr3)
UINT64 __readcr3(void);
#pragma intrinsic(__halt)
void __halt(void);
#pragma intrinsic(_InterlockedIncrement)
long _InterlockedIncrement(long volatile *Addend);

#define AP_STACK_SIZE 16384
#define AP_TRAMPOLINE_CR3_OFFSET 0x100
#define AP_TRAMPOLINE_STACK_OFFSET 0x108
#define AP_TRAMPOLINE_ENTRY_OFFSET 0x110

static UINT8 ap_stacks[ASAS_MAX_PROCESSORS][AP_STACK_SIZE];
static volatile UINT32 online_processors = 1;

static void ap_main(void)
{
    _InterlockedIncrement((long volatile *)&online_processors);

    for (;;) {
        __halt();
    }
}

static void write_trampoline_value(UINT64 base, UINT32 offset, UINT64 value)
{
    *(volatile UINT64 *)(UINTN)(base + offset) = value;
}

int smp_start_secondary_processors(
    const ASAS_BOOT_INFO *boot_info,
    const ASAS_PROCESSOR_LIST *processors
)
{
    UINT32 bsp_id = apic_current_id();
    UINT32 expected_count = 1;
    UINT32 index;
    volatile UINT32 wait;

    write_trampoline_value(boot_info->ap_trampoline, AP_TRAMPOLINE_CR3_OFFSET, __readcr3());
    write_trampoline_value(
        boot_info->ap_trampoline,
        AP_TRAMPOLINE_ENTRY_OFFSET,
        (UINT64)(UINTN)ap_main
    );

    for (index = 0; index < processors->count; index++) {
        UINT32 apic_id = processors->apic_ids[index];

        if (apic_id == bsp_id || apic_id > 255) {
            continue;
        }

        write_trampoline_value(
            boot_info->ap_trampoline,
            AP_TRAMPOLINE_STACK_OFFSET,
            (UINT64)(UINTN)&ap_stacks[index][AP_STACK_SIZE]
        );
        expected_count++;
        apic_start_processor(apic_id, (UINT8)(boot_info->ap_trampoline >> 12));

        for (wait = 0; wait < 50000000U && online_processors < expected_count; wait++) {
        }

        if (online_processors < expected_count) {
            logger_write("INFO", "secondary processor startup incomplete");
            return 0;
        }
    }

    logger_write("INFO", "secondary processors are online");
    return 1;
}

UINT32 smp_online_processor_count(void)
{
    return online_processors;
}
