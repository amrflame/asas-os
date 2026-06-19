#include "acpi.h"

#pragma pack(push, 1)
typedef struct {
    char signature[8];
    UINT8 checksum;
    char oem_id[6];
    UINT8 revision;
    UINT32 rsdt_address;
    UINT32 length;
    UINT64 xsdt_address;
    UINT8 extended_checksum;
    UINT8 reserved[3];
} ACPI_RSDP;

typedef struct {
    char signature[4];
    UINT32 length;
    UINT8 revision;
    UINT8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    UINT32 oem_revision;
    UINT32 creator_id;
    UINT32 creator_revision;
} ACPI_SDT_HEADER;

typedef struct {
    ACPI_SDT_HEADER header;
    UINT64 entries[1];
} ACPI_XSDT;

typedef struct {
    ACPI_SDT_HEADER header;
    UINT32 local_apic_address;
    UINT32 flags;
    UINT8 entries[1];
} ACPI_MADT;

typedef struct {
    UINT8 type;
    UINT8 length;
} ACPI_MADT_ENTRY;

typedef struct {
    UINT8 type;
    UINT8 length;
    UINT8 acpi_processor_id;
    UINT8 apic_id;
    UINT32 flags;
} ACPI_MADT_LOCAL_APIC;

typedef struct {
    UINT8 type;
    UINT8 length;
    UINT16 reserved;
    UINT32 x2apic_id;
    UINT32 flags;
    UINT32 acpi_processor_uid;
} ACPI_MADT_LOCAL_X2APIC;

typedef struct {
    UINT8 type;
    UINT8 length;
    UINT8 ioapic_id;
    UINT8 reserved;
    UINT32 ioapic_address;
    UINT32 global_system_interrupt_base;
} ACPI_MADT_IOAPIC;

typedef struct {
    UINT8 type;
    UINT8 length;
    UINT8 bus;
    UINT8 source;
    UINT32 global_system_interrupt;
    UINT16 flags;
} ACPI_MADT_INTERRUPT_OVERRIDE;
#pragma pack(pop)

static int signature_matches(const char *left, const char *right, UINT32 length)
{
    UINT32 index;

    for (index = 0; index < length; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }

    return 1;
}

static ACPI_MADT *find_madt(const ACPI_XSDT *xsdt)
{
    UINT32 entry_count = (xsdt->header.length - sizeof(ACPI_SDT_HEADER)) / sizeof(UINT64);
    UINT32 index;

    for (index = 0; index < entry_count; index++) {
        ACPI_SDT_HEADER *header = (ACPI_SDT_HEADER *)(UINTN)xsdt->entries[index];

        if (signature_matches(header->signature, "APIC", 4)) {
            return (ACPI_MADT *)header;
        }
    }

    return 0;
}

UINT32 acpi_discover_processors(UINT64 rsdp_address, ASAS_PROCESSOR_LIST *processors)
{
    ACPI_RSDP *rsdp = (ACPI_RSDP *)(UINTN)rsdp_address;
    ACPI_XSDT *xsdt;
    ACPI_MADT *madt;
    UINT8 *entry_pointer;
    UINT8 *table_end;
    processors->count = 0;

    if (rsdp == 0 || !signature_matches(rsdp->signature, "RSD PTR ", 8) || rsdp->revision < 2) {
        return 0;
    }

    xsdt = (ACPI_XSDT *)(UINTN)rsdp->xsdt_address;
    if (xsdt == 0 || !signature_matches(xsdt->header.signature, "XSDT", 4)) {
        return 0;
    }

    madt = find_madt(xsdt);
    if (madt == 0) {
        return 0;
    }

    entry_pointer = &madt->entries[0];
    table_end = (UINT8 *)madt + madt->header.length;

    while (entry_pointer + sizeof(ACPI_MADT_ENTRY) <= table_end) {
        ACPI_MADT_ENTRY *entry = (ACPI_MADT_ENTRY *)entry_pointer;

        if (entry->length < sizeof(ACPI_MADT_ENTRY) || entry_pointer + entry->length > table_end) {
            break;
        }

        if (entry->type == 0 && entry->length >= sizeof(ACPI_MADT_LOCAL_APIC)) {
            ACPI_MADT_LOCAL_APIC *local_apic = (ACPI_MADT_LOCAL_APIC *)entry;
            if ((local_apic->flags & 3U) != 0) {
                if (processors->count < ASAS_MAX_PROCESSORS) {
                    processors->apic_ids[processors->count++] = local_apic->apic_id;
                }
            }
        } else if (entry->type == 9 && entry->length >= sizeof(ACPI_MADT_LOCAL_X2APIC)) {
            ACPI_MADT_LOCAL_X2APIC *local_x2apic = (ACPI_MADT_LOCAL_X2APIC *)entry;
            if ((local_x2apic->flags & 3U) != 0) {
                if (processors->count < ASAS_MAX_PROCESSORS) {
                    processors->apic_ids[processors->count++] = local_x2apic->x2apic_id;
                }
            }
        }

        entry_pointer += entry->length;
    }

    return processors->count;
}

int acpi_discover_ioapic(UINT64 rsdp_address, UINT64 *ioapic_address, UINT32 *keyboard_gsi)
{
    ACPI_RSDP *rsdp = (ACPI_RSDP *)(UINTN)rsdp_address;
    ACPI_XSDT *xsdt;
    ACPI_MADT *madt;
    UINT8 *entry_pointer;
    UINT8 *table_end;

    *ioapic_address = 0;
    *keyboard_gsi = 1;

    if (rsdp == 0 || rsdp->revision < 2) {
        return 0;
    }

    xsdt = (ACPI_XSDT *)(UINTN)rsdp->xsdt_address;
    madt = find_madt(xsdt);
    if (madt == 0) {
        return 0;
    }

    entry_pointer = &madt->entries[0];
    table_end = (UINT8 *)madt + madt->header.length;

    while (entry_pointer + sizeof(ACPI_MADT_ENTRY) <= table_end) {
        ACPI_MADT_ENTRY *entry = (ACPI_MADT_ENTRY *)entry_pointer;

        if (entry->length < sizeof(ACPI_MADT_ENTRY) || entry_pointer + entry->length > table_end) {
            break;
        }

        if (entry->type == 1 && entry->length >= sizeof(ACPI_MADT_IOAPIC)) {
            ACPI_MADT_IOAPIC *ioapic = (ACPI_MADT_IOAPIC *)entry;
            *ioapic_address = ioapic->ioapic_address;
        } else if (entry->type == 2 && entry->length >= sizeof(ACPI_MADT_INTERRUPT_OVERRIDE)) {
            ACPI_MADT_INTERRUPT_OVERRIDE *override = (ACPI_MADT_INTERRUPT_OVERRIDE *)entry;
            if (override->source == 1) {
                *keyboard_gsi = override->global_system_interrupt;
            }
        }

        entry_pointer += entry->length;
    }

    return *ioapic_address != 0;
}
