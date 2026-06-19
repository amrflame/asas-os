#include "power.h"
#include "logger.h"

#pragma intrinsic(__outbyte)
void __outbyte(unsigned short port, unsigned char value);
#pragma intrinsic(__outword)
void __outword(unsigned short port, unsigned short value);

#define ACPI_PM1_SLEEP_ENABLE (1U << 13)
#define ACPI_PM1_SLEEP_TYPE_SHIFT 10
#define RESET_CONTROL_PORT 0x0CF9

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
    UINT8 reserved;
    UINT8 preferred_pm_profile;
    UINT16 sci_interrupt;
    UINT32 smi_command_port;
    UINT8 acpi_enable;
    UINT8 acpi_disable;
    UINT8 s4bios_request;
    UINT8 pstate_control;
    UINT32 pm1a_event_block;
    UINT32 pm1b_event_block;
    UINT32 pm1a_control_block;
    UINT32 pm1b_control_block;
} ACPI_FADT_BASE;
#pragma pack(pop)

static UINT16 pm1a_control_block;
static UINT16 pm1b_control_block;
static UINT8 shutdown_sleep_type;
static UINT8 sleep_sleep_type;
static UINT8 has_shutdown_sleep_type;
static UINT8 has_sleep_sleep_type;
static UINT8 has_battery_namespace;
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

static UINT32 aml_package_length_size(const UINT8 *bytes, UINT32 remaining)
{
    UINT8 lead;

    if (remaining == 0) {
        return 0;
    }
    lead = bytes[0];
    return (UINT32)(lead >> 6) + 1;
}

static int aml_read_integer(const UINT8 *bytes, UINT32 remaining, UINT32 *offset, UINT8 *value)
{
    UINT32 cursor = *offset;

    if (cursor >= remaining) {
        return 0;
    }
    if (bytes[cursor] == 0x00) {
        *value = 0;
        *offset = cursor + 1;
        return 1;
    }
    if (bytes[cursor] == 0x01) {
        *value = 1;
        *offset = cursor + 1;
        return 1;
    }
    if (bytes[cursor] == 0x0A && cursor + 1 < remaining) {
        *value = bytes[cursor + 1];
        *offset = cursor + 2;
        return 1;
    }
    return 0;
}

static int aml_find_sleep_type(const ACPI_SDT_HEADER *dsdt, const char name[4], UINT8 *sleep_type)
{
    const UINT8 *bytes = (const UINT8 *)dsdt;
    UINT32 index;

    if (dsdt == 0 || dsdt->length < sizeof(ACPI_SDT_HEADER) + 12) {
        return 0;
    }
    for (index = sizeof(ACPI_SDT_HEADER); index + 10 < dsdt->length; index++) {
        if (
            bytes[index] == '_' &&
            bytes[index + 1] == name[1] &&
            bytes[index + 2] == name[2] &&
            bytes[index + 3] == '_'
        ) {
            UINT32 cursor = index + 4;
            UINT32 package_length_size;
            UINT8 first_value;

            if (cursor >= dsdt->length || bytes[cursor] != 0x12) {
                continue;
            }
            cursor++;
            package_length_size = aml_package_length_size(&bytes[cursor], dsdt->length - cursor);
            if (package_length_size == 0 || cursor + package_length_size >= dsdt->length) {
                continue;
            }
            cursor += package_length_size;
            if (cursor >= dsdt->length) {
                continue;
            }
            cursor++;
            if (aml_read_integer(bytes, dsdt->length, &cursor, &first_value)) {
                *sleep_type = first_value;
                return 1;
            }
        }
    }
    return 0;
}

static int aml_contains_name(const ACPI_SDT_HEADER *dsdt, const char *name, UINT32 name_length)
{
    const UINT8 *bytes = (const UINT8 *)dsdt;
    UINT32 index;
    UINT32 match_index;

    if (dsdt == 0 || dsdt->length < sizeof(ACPI_SDT_HEADER) + name_length) {
        return 0;
    }
    for (index = sizeof(ACPI_SDT_HEADER); index + name_length <= dsdt->length; index++) {
        for (match_index = 0; match_index < name_length; match_index++) {
            if (bytes[index + match_index] != (UINT8)name[match_index]) {
                break;
            }
        }
        if (match_index == name_length) {
            return 1;
        }
    }
    return 0;
}

int power_initialize(UINT64 rsdp_address)
{
    ACPI_RSDP *rsdp = (ACPI_RSDP *)(UINTN)rsdp_address;
    ACPI_XSDT *xsdt;
    ACPI_FADT_BASE *fadt;
    ACPI_SDT_HEADER *dsdt;

    initialized = 0;
    pm1a_control_block = 0;
    pm1b_control_block = 0;
    has_shutdown_sleep_type = 0;
    has_sleep_sleep_type = 0;
    has_battery_namespace = 0;
    if (rsdp == 0 || !signature_matches(rsdp->signature, "RSD PTR ", 8) || rsdp->revision < 2) {
        return 0;
    }
    xsdt = (ACPI_XSDT *)(UINTN)rsdp->xsdt_address;
    if (xsdt == 0 || !signature_matches(xsdt->header.signature, "XSDT", 4)) {
        return 0;
    }
    fadt = (ACPI_FADT_BASE *)find_table(xsdt, "FACP");
    if (fadt == 0 || fadt->header.length < sizeof(ACPI_FADT_BASE)) {
        return 0;
    }
    pm1a_control_block = (UINT16)fadt->pm1a_control_block;
    pm1b_control_block = (UINT16)fadt->pm1b_control_block;
    dsdt = (ACPI_SDT_HEADER *)(UINTN)fadt->dsdt;
    if (dsdt != 0 && signature_matches(dsdt->signature, "DSDT", 4)) {
        has_shutdown_sleep_type = (UINT8)aml_find_sleep_type(dsdt, "_S5_", &shutdown_sleep_type);
        has_sleep_sleep_type = (UINT8)aml_find_sleep_type(dsdt, "_S3_", &sleep_sleep_type);
        has_battery_namespace = (UINT8)(
            aml_contains_name(dsdt, "PNP0C0A", 7) ||
            (
                aml_contains_name(dsdt, "_BIF", 4) &&
                aml_contains_name(dsdt, "_BST", 4)
            )
        );
    }
    initialized = pm1a_control_block != 0;
    return initialized;
}

int power_can_shutdown(void)
{
    return initialized && has_shutdown_sleep_type;
}

int power_can_sleep(void)
{
    return initialized && has_sleep_sleep_type;
}

int power_can_reboot(void)
{
    return 1;
}

int power_shutdown(void)
{
    UINT16 value;

    if (!power_can_shutdown()) {
        return 0;
    }
    value = (UINT16)(((UINT16)shutdown_sleep_type << ACPI_PM1_SLEEP_TYPE_SHIFT) | ACPI_PM1_SLEEP_ENABLE);
    __outword(pm1a_control_block, value);
    if (pm1b_control_block != 0) {
        __outword(pm1b_control_block, value);
    }
    return 1;
}

int power_sleep(void)
{
    UINT16 value;

    if (!power_can_sleep()) {
        return 0;
    }
    value = (UINT16)(((UINT16)sleep_sleep_type << ACPI_PM1_SLEEP_TYPE_SHIFT) | ACPI_PM1_SLEEP_ENABLE);
    __outword(pm1a_control_block, value);
    if (pm1b_control_block != 0) {
        __outword(pm1b_control_block, value);
    }
    return 1;
}

int power_reboot(void)
{
    __outbyte(RESET_CONTROL_PORT, 0x06);
    return 1;
}

int power_battery_namespace_present(void)
{
    return initialized && has_battery_namespace;
}

ASAS_BATTERY_STATUS power_battery_status(void)
{
    if (!power_battery_namespace_present()) {
        return ASAS_BATTERY_UNAVAILABLE;
    }
    return ASAS_BATTERY_STATUS_UNSUPPORTED;
}

int power_self_test(void)
{
    if (!initialized) {
        return 0;
    }
    logger_write("INFO", "ACPI power management initialized");
    if (power_can_shutdown()) {
        logger_write("INFO", "ACPI shutdown command available");
    }
    if (power_can_reboot()) {
        logger_write("INFO", "platform reboot command available");
    }
    if (power_can_sleep()) {
        logger_write("INFO", "ACPI sleep command available");
    } else {
        logger_write("INFO", "ACPI sleep command unavailable");
    }
    if (power_battery_namespace_present()) {
        logger_write("INFO", "ACPI battery namespace detected");
        logger_write("INFO", "ACPI battery charge reading unsupported");
    } else {
        logger_write("INFO", "ACPI battery namespace unavailable");
    }
    return power_can_shutdown() && power_can_reboot();
}
