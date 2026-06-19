#include "laptop.h"
#include "logger.h"

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
    UINT32 firmware_control;
    UINT32 dsdt;
} ACPI_FADT_MINIMAL;
#pragma pack(pop)

static UINT8 touchpad_present;
static UINT8 wifi_present;
static UINT8 initialized;

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

static ACPI_SDT_HEADER *find_table(const ACPI_XSDT *xsdt, const char signature[4])
{
    UINT32 entry_count;
    UINT32 index;

    if (xsdt == 0 || xsdt->header.length < sizeof(ACPI_SDT_HEADER)) {
        return 0;
    }
    entry_count = (xsdt->header.length - sizeof(ACPI_SDT_HEADER)) / sizeof(UINT64);
    for (index = 0; index < entry_count; index++) {
        ACPI_SDT_HEADER *header = (ACPI_SDT_HEADER *)(UINTN)xsdt->entries[index];

        if (header != 0 && signature_matches(header->signature, signature, 4)) {
            return header;
        }
    }
    return 0;
}

static int table_contains(const ACPI_SDT_HEADER *table, const char *text, UINT32 text_length)
{
    const UINT8 *bytes = (const UINT8 *)table;
    UINT32 index;
    UINT32 match_index;

    if (table == 0 || table->length < sizeof(ACPI_SDT_HEADER) + text_length) {
        return 0;
    }
    for (index = sizeof(ACPI_SDT_HEADER); index + text_length <= table->length; index++) {
        for (match_index = 0; match_index < text_length; match_index++) {
            if (bytes[index + match_index] != (UINT8)text[match_index]) {
                break;
            }
        }
        if (match_index == text_length) {
            return 1;
        }
    }
    return 0;
}

static int discover_touchpad(UINT64 rsdp_address)
{
    ACPI_RSDP *rsdp = (ACPI_RSDP *)(UINTN)rsdp_address;
    ACPI_XSDT *xsdt;
    ACPI_FADT_MINIMAL *fadt;
    ACPI_SDT_HEADER *dsdt;

    if (rsdp == 0 || !signature_matches(rsdp->signature, "RSD PTR ", 8) || rsdp->revision < 2) {
        return 0;
    }
    xsdt = (ACPI_XSDT *)(UINTN)rsdp->xsdt_address;
    if (xsdt == 0 || !signature_matches(xsdt->header.signature, "XSDT", 4)) {
        return 0;
    }
    fadt = (ACPI_FADT_MINIMAL *)find_table(xsdt, "FACP");
    if (fadt == 0 || fadt->header.length < sizeof(ACPI_FADT_MINIMAL)) {
        return 0;
    }
    dsdt = (ACPI_SDT_HEADER *)(UINTN)fadt->dsdt;
    if (dsdt == 0 || !signature_matches(dsdt->signature, "DSDT", 4)) {
        return 0;
    }
    return
        table_contains(dsdt, "PNP0F13", 7) ||
        table_contains(dsdt, "SYN", 3) ||
        table_contains(dsdt, "ELAN", 4) ||
        table_contains(dsdt, "ALPS", 4) ||
        table_contains(dsdt, "DLL", 3);
}

static int discover_wifi(const ASAS_PCI_REGISTRY *pci_registry)
{
    UINT32 index;

    if (pci_registry == 0) {
        return 0;
    }
    for (index = 0; index < pci_registry->count; index++) {
        const ASAS_PCI_DEVICE *device = &pci_registry->devices[index];

        if (device->class_code == 0x02 && device->subclass == 0x80) {
            return 1;
        }
    }
    return 0;
}

int laptop_initialize(UINT64 rsdp_address, const ASAS_PCI_REGISTRY *pci_registry)
{
    touchpad_present = (UINT8)discover_touchpad(rsdp_address);
    wifi_present = (UINT8)discover_wifi(pci_registry);
    initialized = 1;
    return 1;
}

int laptop_touchpad_present(void)
{
    return initialized && touchpad_present;
}

int laptop_wifi_present(void)
{
    return initialized && wifi_present;
}

int laptop_self_test(void)
{
    if (!initialized) {
        return 0;
    }
    if (laptop_touchpad_present()) {
        logger_write("INFO", "ACPI touchpad namespace detected");
    } else {
        logger_write("INFO", "ACPI touchpad namespace unavailable");
    }
    if (laptop_wifi_present()) {
        logger_write("INFO", "PCI Wi-Fi network controller detected");
    } else {
        logger_write("INFO", "PCI Wi-Fi network controller unavailable");
    }
    return 1;
}
