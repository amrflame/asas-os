#include "ahci.h"
#include "block_device.h"
#include "logger.h"
#include "pci.h"

#define AHCI_GHC_AE (1U << 31)
#define AHCI_PORT_CMD_ST (1U << 0)
#define AHCI_PORT_CMD_FRE (1U << 4)
#define AHCI_PORT_CMD_FR (1U << 14)
#define AHCI_PORT_CMD_CR (1U << 15)
#define AHCI_PORT_TFD_BSY (1U << 7)
#define AHCI_PORT_TFD_DRQ (1U << 3)
#define AHCI_PORT_SSTS_DET_MASK 0x0F
#define AHCI_PORT_SSTS_IPM_MASK 0x0F00
#define AHCI_PORT_DET_PRESENT 0x03
#define AHCI_PORT_IPM_ACTIVE 0x0100
#define AHCI_FIS_TYPE_REG_H2D 0x27
#define AHCI_ATA_CMD_READ_DMA_EXT 0x25
#define AHCI_ATA_CMD_WRITE_DMA_EXT 0x35
#define AHCI_ATA_CMD_IDENTIFY 0xEC
#define AHCI_ATA_CMD_FLUSH_CACHE_EXT 0xEA
#define AHCI_IDENTIFY_SIZE 512U
#define AHCI_MAX_LOGICAL_SECTOR_SIZE 4096U
#define AHCI_DMA_BUFFER_SIZE (64U * 1024U)
#define AHCI_TIMEOUT 1000000U
#define AHCI_CMD_FLAGS_WRITE (1U << 6)  /* W bit: host-to-device data transfer */

typedef volatile struct {
    UINT32 command_list_base;
    UINT32 command_list_base_upper;
    UINT32 fis_base;
    UINT32 fis_base_upper;
    UINT32 interrupt_status;
    UINT32 interrupt_enable;
    UINT32 command;
    UINT32 reserved0;
    UINT32 task_file_data;
    UINT32 signature;
    UINT32 sata_status;
    UINT32 sata_control;
    UINT32 sata_error;
    UINT32 sata_active;
    UINT32 command_issue;
    UINT32 sata_notification;
    UINT32 fis_switch_control;
    UINT32 reserved1[11];
    UINT32 vendor[4];
} AHCI_PORT;

typedef volatile struct {
    UINT32 capabilities;
    UINT32 global_host_control;
    UINT32 interrupt_status;
    UINT32 ports_implemented;
    UINT32 version;
    UINT32 command_completion_coalescing_control;
    UINT32 command_completion_coalescing_ports;
    UINT32 enclosure_management_location;
    UINT32 enclosure_management_control;
    UINT32 capabilities_extended;
    UINT32 bios_handoff_control_status;
    UINT8 reserved[0xA0 - 0x2C];
    UINT8 vendor[0x100 - 0xA0];
    AHCI_PORT ports[32];
} AHCI_CONTROLLER;

typedef struct {
    UINT8 fis_type;
    UINT8 port_multiplier;
    UINT8 command;
    UINT8 feature_low;
    UINT8 lba0;
    UINT8 lba1;
    UINT8 lba2;
    UINT8 device;
    UINT8 lba3;
    UINT8 lba4;
    UINT8 lba5;
    UINT8 feature_high;
    UINT8 count_low;
    UINT8 count_high;
    UINT8 icc;
    UINT8 control;
    UINT8 reserved[48];
} AHCI_FIS_REG_H2D;

typedef struct {
    UINT32 data_base;
    UINT32 data_base_upper;
    UINT32 reserved0;
    UINT32 byte_count_interrupt;
} AHCI_PRDT_ENTRY;

typedef struct {
    UINT8 command_fis[64];
    UINT8 atapi_command[16];
    UINT8 reserved[48];
    AHCI_PRDT_ENTRY prdt[1];
} AHCI_COMMAND_TABLE;

typedef struct {
    UINT16 flags;
    UINT16 prdt_length;
    UINT32 transferred_byte_count;
    UINT32 command_table_base;
    UINT32 command_table_base_upper;
    UINT32 reserved[4];
} AHCI_COMMAND_HEADER;

static AHCI_CONTROLLER *active_controller;
static AHCI_PORT *active_port;
static UINT64 ahci_sector_count;
static UINT32 ahci_logical_sector_size = AHCI_IDENTIFY_SIZE;
__declspec(align(1024)) static AHCI_COMMAND_HEADER command_headers[32];
__declspec(align(256)) static UINT8 received_fis[256];
__declspec(align(128)) static AHCI_COMMAND_TABLE command_table;
__declspec(align(4096)) static UINT8 ahci_dma_sector[AHCI_DMA_BUFFER_SIZE];

static void zero_bytes(void *buffer, UINT32 size)
{
    UINT8 *bytes = (UINT8 *)buffer;
    UINT32 index;

    for (index = 0; index < size; index++) {
        bytes[index] = 0;
    }
}

static int wait_clear(volatile UINT32 *value, UINT32 mask)
{
    UINT32 timeout;

    for (timeout = 0; timeout < AHCI_TIMEOUT; timeout++) {
        if ((*value & mask) == 0) {
            return 1;
        }
    }
    return 0;
}

static void stop_port(AHCI_PORT *port)
{
    port->command &= ~AHCI_PORT_CMD_ST;
    (void)wait_clear(&port->command, AHCI_PORT_CMD_CR);
    port->command &= ~AHCI_PORT_CMD_FRE;
    (void)wait_clear(&port->command, AHCI_PORT_CMD_FR);
}

static void start_port(AHCI_PORT *port)
{
    port->command |= AHCI_PORT_CMD_FRE;
    port->command |= AHCI_PORT_CMD_ST;
}

static int ahci_command(UINT8 command, UINT64 lba, void *buffer,
                        int write, UINT16 sector_count)
{
    AHCI_COMMAND_HEADER *header;
    AHCI_FIS_REG_H2D *fis;
    UINT32 timeout;
    UINT32 index;

    UINT32 transfer_bytes = (UINT32)sector_count * ahci_logical_sector_size;
    int data_transfer = sector_count != 0;
    if (active_controller == 0 || active_port == 0 ||
        transfer_bytes > AHCI_DMA_BUFFER_SIZE ||
        (data_transfer && buffer == 0)) return 0;
    if (write && data_transfer) {
        for (index = 0; index < transfer_bytes; index++) {
            ahci_dma_sector[index] = ((const UINT8 *)buffer)[index];
        }
    } else {
        zero_bytes(ahci_dma_sector, data_transfer ? transfer_bytes :
                   AHCI_IDENTIFY_SIZE);
    }

    stop_port(active_port);
    zero_bytes(command_headers, sizeof(command_headers));
    zero_bytes(received_fis, sizeof(received_fis));
    zero_bytes(&command_table, sizeof(command_table));
    active_port->command_list_base = (UINT32)(UINTN)command_headers;
    active_port->command_list_base_upper = (UINT32)((UINT64)(UINTN)command_headers >> 32);
    active_port->fis_base = (UINT32)(UINTN)received_fis;
    active_port->fis_base_upper = (UINT32)((UINT64)(UINTN)received_fis >> 32);
    active_port->sata_error = 0xFFFFFFFFU;
    active_port->interrupt_status = 0xFFFFFFFFU;
    start_port(active_port);
    if (!wait_clear(&active_port->task_file_data,
                    AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) return 0;

    header = &command_headers[0];
    header->flags = 5 | (write ? AHCI_CMD_FLAGS_WRITE : 0);
    header->prdt_length = data_transfer ? 1 : 0;
    header->command_table_base = (UINT32)(UINTN)&command_table;
    header->command_table_base_upper = (UINT32)((UINT64)(UINTN)&command_table >> 32);
    if (data_transfer) {
        command_table.prdt[0].data_base = (UINT32)(UINTN)ahci_dma_sector;
        command_table.prdt[0].data_base_upper =
            (UINT32)((UINT64)(UINTN)ahci_dma_sector >> 32);
        command_table.prdt[0].byte_count_interrupt =
            (transfer_bytes - 1U) | (1U << 31);
    }

    fis = (AHCI_FIS_REG_H2D *)command_table.command_fis;
    fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    fis->port_multiplier = 1U << 7;
    fis->command = command;
    fis->device = 1U << 6;
    fis->lba0 = (UINT8)lba;
    fis->lba1 = (UINT8)(lba >> 8);
    fis->lba2 = (UINT8)(lba >> 16);
    fis->lba3 = (UINT8)(lba >> 24);
    fis->lba4 = (UINT8)(lba >> 32);
    fis->lba5 = (UINT8)(lba >> 40);
    if (data_transfer) {
        fis->count_low = (UINT8)sector_count;
        fis->count_high = (UINT8)(sector_count >> 8);
    }

    active_port->command_issue = 1;
    for (timeout = 0; timeout < AHCI_TIMEOUT; timeout++) {
        if ((active_port->command_issue & 1U) == 0) {
            if ((active_port->interrupt_status & (1U << 30)) != 0) return 0;
            if (!write && data_transfer) {
                for (index = 0; index < transfer_bytes; index++) {
                    ((UINT8 *)buffer)[index] = ahci_dma_sector[index];
                }
            }
            return 1;
        }
    }
    return 0;
}

UINT32 ahci_initialize(const ASAS_PCI_DEVICE *device)
{
    UINT32 bar_low = device->bars[5];
    UINT64 abar;
    AHCI_CONTROLLER *controller;
    UINT32 implemented;
    UINT32 port_index;
    UINT32 active_ports = 0;

    UINT16 *identify = (UINT16 *)(void *)ahci_dma_sector;

    if (active_controller != 0 && active_port != 0 && ahci_sector_count != 0) return 1;
    /* Reject I/O-space BARs */
    if ((bar_low & 1U) != 0) {
        return 0;
    }
    /* Handle 64-bit MMIO BAR (type bits 2:1 == 0b10) */
    if ((bar_low & 0x06U) == 0x04U) {
        /*
         * The upper 32 bits live in the PCI config slot after BAR5 (offset 0x28).
         * The ASAS_PCI_DEVICE struct only stores bars[0..5], so we cannot read
         * them here.  For the platforms we target (QEMU, Hyper-V Gen1/Gen2)
         * the AHCI ABAR is always in the low 4 GB, so treating it as 32-bit
         * is safe.  Log a warning so the limitation is visible.
         */
        logger_write("WARN", "AHCI ABAR is 64-bit MMIO; using lower 32 bits only");
    }
    abar = (UINT64)(bar_low & ~0x0FU);
    if (abar == 0) {
        return 0;
    }

    pci_enable_bus_mastering(device);
    controller = (AHCI_CONTROLLER *)(UINTN)abar;
    controller->global_host_control |= AHCI_GHC_AE;
    active_controller = controller;
    active_port = 0;
    implemented = controller->ports_implemented;

    for (port_index = 0; port_index < 32; port_index++) {
        AHCI_PORT *port;
        UINT32 status;

        if ((implemented & (1U << port_index)) == 0) {
            continue;
        }

        port = &controller->ports[port_index];
        status = port->sata_status;
        if (
            (status & AHCI_PORT_SSTS_DET_MASK) == AHCI_PORT_DET_PRESENT &&
            (status & AHCI_PORT_SSTS_IPM_MASK) == AHCI_PORT_IPM_ACTIVE
        ) {
            if (active_port == 0) {
                active_port = port;
            }
            active_ports++;
        }
    }

    if (active_ports == 0 || !ahci_command(AHCI_ATA_CMD_IDENTIFY, 0,
                                           ahci_dma_sector, 0, 1)) return 0;
    if ((identify[83] & (1U << 10)) != 0) {
        ahci_sector_count = (UINT64)identify[100] |
            ((UINT64)identify[101] << 16) |
            ((UINT64)identify[102] << 32) |
            ((UINT64)identify[103] << 48);
    } else {
        ahci_sector_count = (UINT64)identify[60] | ((UINT64)identify[61] << 16);
    }
    if ((identify[106] & (1U << 14)) != 0 &&
        (identify[106] & (1U << 15)) == 0 &&
        (identify[106] & (1U << 12)) != 0) {
        UINT32 words_per_sector = (UINT32)identify[117] |
                                  ((UINT32)identify[118] << 16);
        UINT32 bytes_per_sector = words_per_sector * 2U;
        if (bytes_per_sector >= AHCI_IDENTIFY_SIZE &&
            bytes_per_sector <= AHCI_MAX_LOGICAL_SECTOR_SIZE &&
            (bytes_per_sector & (bytes_per_sector - 1U)) == 0)
            ahci_logical_sector_size = bytes_per_sector;
    }
    return ahci_sector_count != 0 ? active_ports : 0;
}

int ahci_read_sector(UINT64 lba, void *buffer)
{
    AHCI_COMMAND_HEADER *header;
    AHCI_FIS_REG_H2D *fis;
    UINT32 timeout;

    if (active_controller == 0 || active_port == 0 || buffer == 0 ||
        lba >= ahci_sector_count) {
        return 0;
    }

    stop_port(active_port);
    zero_bytes(command_headers, sizeof(command_headers));
    zero_bytes(received_fis, sizeof(received_fis));
    zero_bytes(&command_table, sizeof(command_table));
    zero_bytes(ahci_dma_sector, sizeof(ahci_dma_sector));

    active_port->command_list_base = (UINT32)(UINTN)command_headers;
    active_port->command_list_base_upper = (UINT32)((UINT64)(UINTN)command_headers >> 32);
    active_port->fis_base = (UINT32)(UINTN)received_fis;
    active_port->fis_base_upper = (UINT32)((UINT64)(UINTN)received_fis >> 32);
    active_port->sata_error = 0xFFFFFFFFU;
    active_port->interrupt_status = 0xFFFFFFFFU;
    start_port(active_port);

    if ((active_port->task_file_data & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) != 0) {
        if (!wait_clear(&active_port->task_file_data, AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) {
            return 0;
        }
    }

    header = &command_headers[0];
    header->flags = 5;
    header->prdt_length = 1;
    header->command_table_base = (UINT32)(UINTN)&command_table;
    header->command_table_base_upper = (UINT32)((UINT64)(UINTN)&command_table >> 32);

    command_table.prdt[0].data_base = (UINT32)(UINTN)ahci_dma_sector;
    command_table.prdt[0].data_base_upper = (UINT32)((UINT64)(UINTN)ahci_dma_sector >> 32);
    command_table.prdt[0].byte_count_interrupt =
        (ahci_logical_sector_size - 1U) | (1U << 31);

    fis = (AHCI_FIS_REG_H2D *)command_table.command_fis;
    fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    fis->port_multiplier = 1U << 7;
    fis->command = AHCI_ATA_CMD_READ_DMA_EXT;
    fis->device = 1U << 6;
    fis->lba0 = (UINT8)lba;
    fis->lba1 = (UINT8)(lba >> 8);
    fis->lba2 = (UINT8)(lba >> 16);
    fis->lba3 = (UINT8)(lba >> 24);
    fis->lba4 = (UINT8)(lba >> 32);
    fis->lba5 = (UINT8)(lba >> 40);
    fis->count_low = 1;
    fis->count_high = 0;

    active_port->command_issue = 1;
    for (timeout = 0; timeout < AHCI_TIMEOUT; timeout++) {
        if ((active_port->command_issue & 1U) == 0) {
            if ((active_port->interrupt_status & (1U << 30)) == 0) {
                UINT32 index;
                UINT8 *output = (UINT8 *)buffer;

                for (index = 0; index < ahci_logical_sector_size; index++) {
                    output[index] = ahci_dma_sector[index];
                }
                return 1;
            }
            return 0;
        }
    }

    return 0;
}

int ahci_write_sector(UINT64 lba, const void *buffer)
{
    AHCI_COMMAND_HEADER *header;
    AHCI_FIS_REG_H2D *fis;
    UINT32 timeout;
    UINT32 index;
    const UINT8 *input = (const UINT8 *)buffer;

    if (active_controller == 0 || active_port == 0 || buffer == 0 ||
        lba >= ahci_sector_count) {
        return 0;
    }

    /* Stage write data into the DMA buffer before issuing the command */
    for (index = 0; index < ahci_logical_sector_size; index++) {
        ahci_dma_sector[index] = input[index];
    }

    stop_port(active_port);
    zero_bytes(command_headers, sizeof(command_headers));
    zero_bytes(received_fis, sizeof(received_fis));
    zero_bytes(&command_table, sizeof(command_table));

    active_port->command_list_base = (UINT32)(UINTN)command_headers;
    active_port->command_list_base_upper = (UINT32)((UINT64)(UINTN)command_headers >> 32);
    active_port->fis_base = (UINT32)(UINTN)received_fis;
    active_port->fis_base_upper = (UINT32)((UINT64)(UINTN)received_fis >> 32);
    active_port->sata_error = 0xFFFFFFFFU;
    active_port->interrupt_status = 0xFFFFFFFFU;
    start_port(active_port);

    if ((active_port->task_file_data & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) != 0) {
        if (!wait_clear(&active_port->task_file_data, AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) {
            return 0;
        }
    }

    header = &command_headers[0];
    header->flags = 5 | AHCI_CMD_FLAGS_WRITE;  /* FIS_LEN=5 DWORDs, W=1 (host→device) */
    header->prdt_length = 1;
    header->command_table_base = (UINT32)(UINTN)&command_table;
    header->command_table_base_upper = (UINT32)((UINT64)(UINTN)&command_table >> 32);

    command_table.prdt[0].data_base = (UINT32)(UINTN)ahci_dma_sector;
    command_table.prdt[0].data_base_upper = (UINT32)((UINT64)(UINTN)ahci_dma_sector >> 32);
    command_table.prdt[0].byte_count_interrupt =
        (ahci_logical_sector_size - 1U) | (1U << 31);

    fis = (AHCI_FIS_REG_H2D *)command_table.command_fis;
    fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    fis->port_multiplier = 1U << 7;
    fis->command = AHCI_ATA_CMD_WRITE_DMA_EXT;
    fis->device = 1U << 6;
    fis->lba0 = (UINT8)lba;
    fis->lba1 = (UINT8)(lba >> 8);
    fis->lba2 = (UINT8)(lba >> 16);
    fis->lba3 = (UINT8)(lba >> 24);
    fis->lba4 = (UINT8)(lba >> 32);
    fis->lba5 = (UINT8)(lba >> 40);
    fis->count_low = 1;
    fis->count_high = 0;

    active_port->command_issue = 1;
    for (timeout = 0; timeout < AHCI_TIMEOUT; timeout++) {
        if ((active_port->command_issue & 1U) == 0) {
            if ((active_port->interrupt_status & (1U << 30)) == 0) {
                return 1;
            }
            return 0;
        }
    }

    return 0;
}

int ahci_flush(void)
{
    return ahci_command(AHCI_ATA_CMD_FLUSH_CACHE_EXT, 0, 0, 0, 0);
}

static int ahci_block_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                           UINT32 count, void *buffer)
{
    UINT32 chunk;
    UINT8 *bytes = (UINT8 *)buffer;
    while (count != 0) {
        UINT32 max_sectors = AHCI_DMA_BUFFER_SIZE /
                             device->logical_block_size;
        chunk = count > max_sectors ? max_sectors : count;
        if (!ahci_command(AHCI_ATA_CMD_READ_DMA_EXT, lba, bytes, 0,
                          (UINT16)chunk)) return 0;
        lba += chunk;
        bytes += (UINT64)chunk * device->logical_block_size;
        count -= chunk;
    }
    return 1;
}

static int ahci_block_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                            UINT32 count, const void *buffer)
{
    UINT32 chunk;
    const UINT8 *bytes = (const UINT8 *)buffer;
    while (count != 0) {
        UINT32 max_sectors = AHCI_DMA_BUFFER_SIZE /
                             device->logical_block_size;
        chunk = count > max_sectors ? max_sectors : count;
        if (!ahci_command(AHCI_ATA_CMD_WRITE_DMA_EXT, lba, (void *)bytes, 1,
                          (UINT16)chunk)) return 0;
        lba += chunk;
        bytes += (UINT64)chunk * device->logical_block_size;
        count -= chunk;
    }
    return 1;
}

static int ahci_block_flush(ASAS_BLOCK_DEVICE *device)
{
    (void)device;
    return ahci_flush();
}

static const ASAS_BLOCK_DEVICE_OPS ahci_block_ops = {
    ahci_block_read,
    ahci_block_write,
    ahci_block_flush
};

int ahci_register_block_device(void)
{
    ASAS_BLOCK_DEVICE description = { 0 };
    static UINT8 multi_block_probe[AHCI_MAX_LOGICAL_SECTOR_SIZE * 2U];
    if (ahci_sector_count == 0 || block_device_find("ahci0") != 0) return 0;
    if (!ahci_command(AHCI_ATA_CMD_READ_DMA_EXT, 0, multi_block_probe, 0, 2)) return 0;
    description.name[0] = 'a';
    description.name[1] = 'h';
    description.name[2] = 'c';
    description.name[3] = 'i';
    description.name[4] = '0';
    description.logical_block_size = ahci_logical_sector_size;
    description.physical_block_size = ahci_logical_sector_size;
    description.block_count = ahci_sector_count;
    description.ops = &ahci_block_ops;
    if (block_device_register(&description) == 0) return 0;
    logger_write("INFO", "AHCI direct block device registered");
    logger_write("INFO", "AHCI multi-block request verified");
    return 1;
}
