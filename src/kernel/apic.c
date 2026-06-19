#include "apic.h"
#include "logger.h"
#include "panic.h"
#include "scheduler.h"

#pragma intrinsic(__readmsr)
UINT64 __readmsr(UINT32 register_number);
#pragma intrinsic(__inbyte)
unsigned char __inbyte(unsigned short Port);
#pragma intrinsic(__outbyte)
void __outbyte(unsigned short Port, unsigned char Data);

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_ENABLE (1ULL << 11)
#define APIC_REGISTER_EOI 0x0B0
#define APIC_REGISTER_ID 0x020
#define APIC_REGISTER_ICR_LOW 0x300
#define APIC_REGISTER_ICR_HIGH 0x310
#define APIC_REGISTER_SPURIOUS 0x0F0
#define APIC_REGISTER_LVT_TIMER 0x320
#define APIC_REGISTER_TIMER_CURRENT 0x390
#define APIC_REGISTER_TIMER_INITIAL 0x380
#define APIC_REGISTER_TIMER_DIVIDE 0x3E0
#define APIC_TIMER_PERIODIC (1U << 17)
#define APIC_TIMER_ONE_SHOT 0U
#define APIC_TIMER_MASKED (1U << 16)
#define APIC_TIMER_VECTOR 32U
#define APIC_SPURIOUS_VECTOR 255U

/* PIT I/O ports */
#define PIT_CHANNEL2_DATA 0x42
#define PIT_COMMAND       0x43
#define PIT_CONTROL       0x61

/* PIT frequency = 1,193,182 Hz. Count for 10 ms = 11931. */
#define PIT_10MS_COUNT 11931U

static volatile UINT32 *apic_base;
static volatile UINT64 timer_ticks;

static void apic_write(UINT32 register_offset, UINT32 value)
{
    apic_base[register_offset / sizeof(UINT32)] = value;
    (void)apic_base[APIC_REGISTER_SPURIOUS / sizeof(UINT32)];
}

static UINT32 apic_read(UINT32 register_offset)
{
    return apic_base[register_offset / sizeof(UINT32)];
}

static void apic_wait_for_delivery(void)
{
    while ((apic_read(APIC_REGISTER_ICR_LOW) & (1U << 12)) != 0) {
    }
}

static void apic_delay(void)
{
    volatile UINT32 index;

    for (index = 0; index < 1000000U; index++) {
    }
}

/* Use PIT channel 2 to measure how many APIC timer ticks occur in 10 ms.
   Returns the calibrated count for a 10 ms period (divide=16). */
static UINT32 apic_calibrate_timer(void)
{
    unsigned char control;
    UINT32 apic_after;

    /* Mask the APIC timer and set divide-by-16 before counting. */
    apic_write(APIC_REGISTER_TIMER_DIVIDE, 0x3U);
    apic_write(APIC_REGISTER_LVT_TIMER, APIC_TIMER_ONE_SHOT | APIC_TIMER_MASKED | APIC_TIMER_VECTOR);
    apic_write(APIC_REGISTER_TIMER_INITIAL, 0xFFFFFFFFU);

    /* Configure PIT channel 2, mode 0 (one-shot), binary. */
    control = __inbyte(PIT_CONTROL);
    __outbyte(PIT_CONTROL, (unsigned char)(control & ~0x03U));   /* gate off, speaker off */
    __outbyte(PIT_COMMAND, 0xB0U);                               /* channel 2, lobyte/hibyte, mode 0 */
    __outbyte(PIT_CHANNEL2_DATA, (unsigned char)(PIT_10MS_COUNT & 0xFFU));
    __outbyte(PIT_CHANNEL2_DATA, (unsigned char)(PIT_10MS_COUNT >> 8));

    /* Start the countdown by asserting the gate (bit 0). */
    __outbyte(PIT_CONTROL, (unsigned char)((__inbyte(PIT_CONTROL) & ~0x02U) | 0x01U));

    /* Spin until PIT output goes high (bit 5 of port 0x61). */
    while ((__inbyte(PIT_CONTROL) & 0x20U) == 0) {
    }

    apic_after = apic_read(APIC_REGISTER_TIMER_CURRENT);

    /* Restore gate off so PIT channel 2 does not interfere later. */
    __outbyte(PIT_CONTROL, (unsigned char)(__inbyte(PIT_CONTROL) & ~0x01U));

    return 0xFFFFFFFFU - apic_after;
}

void apic_initialize(void)
{
    UINT64 base_msr = __readmsr(IA32_APIC_BASE_MSR);
    UINT32 ticks_per_10ms;

    if ((base_msr & APIC_ENABLE) == 0) {
        panic("local APIC is disabled");
    }

    apic_base = (volatile UINT32 *)(UINTN)(base_msr & 0xFFFFF000ULL);
    timer_ticks = 0;

    apic_write(APIC_REGISTER_SPURIOUS, 0x100U | APIC_SPURIOUS_VECTOR);

    ticks_per_10ms = apic_calibrate_timer();
    logger_write_hex("INFO", "APIC ticks per 10ms", ticks_per_10ms);

    apic_write(APIC_REGISTER_TIMER_DIVIDE, 0x3U);
    apic_write(APIC_REGISTER_LVT_TIMER, APIC_TIMER_PERIODIC | APIC_TIMER_VECTOR);
    apic_write(APIC_REGISTER_TIMER_INITIAL, ticks_per_10ms);
    logger_write("INFO", "local APIC timer calibrated and started");
}

void apic_timer_interrupt_handler(void)
{
    timer_ticks++;
    apic_eoi();
    scheduler_on_timer_tick();
}

void apic_eoi(void)
{
    apic_write(APIC_REGISTER_EOI, 0);
}

UINT64 apic_timer_ticks(void)
{
    return timer_ticks;
}

UINT32 apic_current_id(void)
{
    return apic_read(APIC_REGISTER_ID) >> 24;
}

void apic_start_processor(UINT32 apic_id, UINT8 startup_vector)
{
    apic_write(APIC_REGISTER_ICR_HIGH, apic_id << 24);
    apic_write(APIC_REGISTER_ICR_LOW, 0x0000C500U);
    apic_wait_for_delivery();
    apic_delay();

    apic_write(APIC_REGISTER_ICR_HIGH, apic_id << 24);
    apic_write(APIC_REGISTER_ICR_LOW, 0x00008500U);
    apic_wait_for_delivery();
    apic_delay();

    apic_write(APIC_REGISTER_ICR_HIGH, apic_id << 24);
    apic_write(APIC_REGISTER_ICR_LOW, 0x00004600U | startup_vector);
    apic_wait_for_delivery();
    apic_delay();

    apic_write(APIC_REGISTER_ICR_HIGH, apic_id << 24);
    apic_write(APIC_REGISTER_ICR_LOW, 0x00004600U | startup_vector);
    apic_wait_for_delivery();
}
