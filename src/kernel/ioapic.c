#include "ioapic.h"

static UINT32 ioapic_read(volatile UINT32 *base, UINT32 register_index)
{
    base[0] = register_index;
    return base[4];
}

static void ioapic_write(volatile UINT32 *base, UINT32 register_index, UINT32 value)
{
    base[0] = register_index;
    base[4] = value;
}

void ioapic_route_irq(UINT64 ioapic_address, UINT32 gsi, UINT8 vector, UINT8 destination_apic_id)
{
    volatile UINT32 *base = (volatile UINT32 *)(UINTN)ioapic_address;
    UINT32 redirection_index = 0x10 + gsi * 2;
    UINT32 maximum_entry = (ioapic_read(base, 1) >> 16) & 0xFF;

    if (gsi > maximum_entry) {
        return;
    }

    ioapic_write(base, redirection_index + 1, (UINT32)destination_apic_id << 24);
    ioapic_write(base, redirection_index, vector);
}

