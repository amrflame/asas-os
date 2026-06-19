#include "virtio_block.h"
#include "block_device.h"
#include "ahci.h"
#include "hyperv_storage.h"
#include "ide_ata.h"
#include "uefi.h"
#include "logger.h"

#pragma intrinsic(__inbyte)
unsigned char __inbyte(unsigned short port);
#pragma intrinsic(__inword)
unsigned short __inword(unsigned short port);
#pragma intrinsic(__indword)
unsigned long __indword(unsigned short port);
#pragma intrinsic(__outbyte)
void __outbyte(unsigned short port, unsigned char value);
#pragma intrinsic(__outword)
void __outword(unsigned short port, unsigned short value);
#pragma intrinsic(__outdword)
void __outdword(unsigned short port, unsigned long value);

#define VIRTIO_PCI_HOST_FEATURES 0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN 0x08
#define VIRTIO_PCI_QUEUE_NUM 0x0C
#define VIRTIO_PCI_QUEUE_SELECT 0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10
#define VIRTIO_PCI_STATUS 0x12
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTIO_BLOCK_READ 0
#define VIRTIO_BLOCK_WRITE 1
#define VIRTIO_BLK_F_BLK_SIZE (1U << 6)

#pragma pack(push, 1)
typedef struct {
    UINT64 address;
    UINT32 length;
    UINT16 flags;
    UINT16 next;
} VIRTQ_DESCRIPTOR;

typedef struct {
    UINT16 flags;
    volatile UINT16 index;
    UINT16 ring[256];
} VIRTQ_AVAILABLE;

typedef struct {
    UINT32 id;
    UINT32 length;
} VIRTQ_USED_ELEMENT;

typedef struct {
    UINT16 flags;
    volatile UINT16 index;
    VIRTQ_USED_ELEMENT ring[256];
} VIRTQ_USED;

typedef struct {
    UINT32 type;
    UINT32 reserved;
    UINT64 sector;
} VIRTIO_BLOCK_REQUEST;
#pragma pack(pop)

__declspec(align(4096)) static UINT8 queue_memory[16384];
__declspec(align(16)) static VIRTIO_BLOCK_REQUEST request_header;
__declspec(align(16)) static volatile UINT8 request_status;
__declspec(align(4096)) static UINT8 legacy_sector_staging[4096];

static UINT16 io_base;
static UINT16 queue_size;
static VIRTQ_DESCRIPTOR *descriptors;
static VIRTQ_AVAILABLE *available;
static VIRTQ_USED *used;
static UINT8 active_backend;
static UINT8 block_current_target;
static UINT8 block_current_lun;
static UINT64 virtio_sector_count;
static UINT32 virtio_logical_block_size = 512U;

#define BLOCK_BACKEND_NONE 0
#define BLOCK_BACKEND_VIRTIO 1
#define BLOCK_BACKEND_IDE_ATA 2
#define BLOCK_BACKEND_HYPERV 3
#define BLOCK_BACKEND_AHCI 4

extern void memory_fence(void);

static void clear_bytes(void *buffer, UINTN size)
{
    UINT8 *bytes = (UINT8 *)buffer;
    UINTN index;

    for (index = 0; index < size; index++) {
        bytes[index] = 0;
    }
}

int virtio_block_initialize(const ASAS_PCI_DEVICE *device)
{
    UINTN available_offset;
    UINTN used_offset;
    UINT32 bar_index;
    UINT32 host_features;

    io_base = 0;
    for (bar_index = 0; bar_index < 6; bar_index++) {
        if ((device->bars[bar_index] & 1U) != 0) {
            io_base = (UINT16)(device->bars[bar_index] & ~3U);
            if (io_base == 0) {
                io_base = 0xC000;
                pci_write_bar(device, bar_index, (UINT32)io_base | 1U);
            }
            break;
        }
    }

    if (io_base == 0) {
        return 0;
    }
    logger_write("INFO", "VirtIO legacy IO BAR ready");

    pci_enable_bus_mastering(device);

    __outbyte(io_base + VIRTIO_PCI_STATUS, 0);
    __outbyte(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    __outbyte(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    host_features = __indword(io_base + VIRTIO_PCI_HOST_FEATURES);
    __outdword(io_base + VIRTIO_PCI_GUEST_FEATURES,
               host_features & VIRTIO_BLK_F_BLK_SIZE);

    __outword(io_base + VIRTIO_PCI_QUEUE_SELECT, 0);
    queue_size = __inword(io_base + VIRTIO_PCI_QUEUE_NUM);
    if (queue_size == 0 || queue_size > 256) {
        logger_write("ERROR", "VirtIO queue size is invalid");
        return 0;
    }
    logger_write("INFO", "VirtIO queue discovered");

    clear_bytes(queue_memory, sizeof(queue_memory));
    descriptors = (VIRTQ_DESCRIPTOR *)&queue_memory[0];
    available_offset = sizeof(VIRTQ_DESCRIPTOR) * queue_size;
    available = (VIRTQ_AVAILABLE *)&queue_memory[available_offset];
    used_offset = (available_offset + 6 + sizeof(UINT16) * queue_size + 4095) & ~4095ULL;
    used = (VIRTQ_USED *)&queue_memory[used_offset];

    __outdword(io_base + VIRTIO_PCI_QUEUE_PFN, (UINT32)((UINTN)queue_memory >> 12));
    __outbyte(
        io_base + VIRTIO_PCI_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK
    );
    virtio_sector_count = (UINT64)__indword(io_base + 0x14) |
                          ((UINT64)__indword(io_base + 0x18) << 32);
    if ((host_features & VIRTIO_BLK_F_BLK_SIZE) != 0) {
        UINT32 configured = (UINT32)__indword(io_base + 0x28);
        if (configured >= 512U && configured <= 4096U &&
            (configured & (configured - 1U)) == 0)
            virtio_logical_block_size = configured;
    }
    active_backend = BLOCK_BACKEND_VIRTIO;
    return 1;
}

int virtio_block_use_ide_ata(void)
{
    if (!ide_ata_initialize()) {
        return 0;
    }
    active_backend = BLOCK_BACKEND_IDE_ATA;
    return 1;
}

int virtio_block_use_hyperv_storage(ASAS_FRAME_ALLOCATOR *allocator)
{
    if (!hyperv_storage_initialize(allocator)) {
        return 0;
    }
    active_backend = BLOCK_BACKEND_HYPERV;
    return 1;
}

int virtio_block_use_ahci(const ASAS_PCI_DEVICE *device)
{
    if (ahci_initialize(device) == 0) {
        return 0;
    }
    active_backend = BLOCK_BACKEND_AHCI;
    logger_write("INFO", "AHCI block backend ready");
    return 1;
}

const char *virtio_block_backend_name(void)
{
    if (active_backend == BLOCK_BACKEND_VIRTIO) return "VirtIO";
    if (active_backend == BLOCK_BACKEND_IDE_ATA) return "IDE ATA";
    if (active_backend == BLOCK_BACKEND_HYPERV) return "Hyper-V StorVSC";
    if (active_backend == BLOCK_BACKEND_AHCI) return "AHCI";
    return "none";
}

static int virtio_block_transfer(UINT64 sector, UINT32 count,
                                 UINT32 block_size, void *buffer, UINT32 type)
{
    UINT16 previous_used_index = used->index;
    UINT32 timeout;
    if (count == 0 || block_size < 512U ||
        count > 0xFFFFFFFFU / block_size) return 0;

    request_header.type = type;
    request_header.reserved = 0;
    request_header.sector = sector;
    request_status = 0xFF;

    descriptors[0].address = (UINT64)(UINTN)&request_header;
    descriptors[0].length = sizeof(request_header);
    descriptors[0].flags = VIRTQ_DESC_F_NEXT;
    descriptors[0].next = 1;

    descriptors[1].address = (UINT64)(UINTN)buffer;
    descriptors[1].length = count * block_size;
    descriptors[1].flags = VIRTQ_DESC_F_NEXT;
    if (type == VIRTIO_BLOCK_READ) {
        descriptors[1].flags |= VIRTQ_DESC_F_WRITE;
    }
    descriptors[1].next = 2;

    descriptors[2].address = (UINT64)(UINTN)&request_status;
    descriptors[2].length = 1;
    descriptors[2].flags = VIRTQ_DESC_F_WRITE;
    descriptors[2].next = 0;

    available->ring[available->index % queue_size] = 0;
    memory_fence();
    available->index++;
    memory_fence();
    __outword(io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    for (timeout = 0; timeout < 100000000U; timeout++) {
        if (used->index != previous_used_index) {
            memory_fence();
            return request_status == 0;
        }
    }

    logger_write_hex("ERROR", "VirtIO block request timed out", sector);
    return 0;
}

int virtio_block_read_sector(UINT64 sector, void *buffer)
{
    if (active_backend == BLOCK_BACKEND_IDE_ATA) {
        return ide_ata_read_sector(sector, buffer);
    }
    if (active_backend == BLOCK_BACKEND_HYPERV) {
        return hyperv_storage_read_sector_ex(block_current_target,
                                             block_current_lun,
                                             sector, buffer);
    }
    if (active_backend == BLOCK_BACKEND_AHCI) {
        return ahci_read_sector(sector, buffer);
    }
    if (virtio_logical_block_size == 512U)
        return virtio_block_transfer(sector, 1, 512U, buffer,
                                     VIRTIO_BLOCK_READ);
    {
        UINT32 factor = virtio_logical_block_size / 512U;
        UINT32 offset = (UINT32)(sector % factor) * 512U;
        UINT32 index;
        if (!virtio_block_transfer(sector - (sector % factor), 1,
                                   virtio_logical_block_size,
                                   legacy_sector_staging,
                                   VIRTIO_BLOCK_READ)) return 0;
        for (index = 0; index < 512U; index++)
            ((UINT8 *)buffer)[index] = legacy_sector_staging[offset + index];
        return 1;
    }
}

int virtio_block_write_sector(UINT64 sector, const void *buffer)
{
    if (active_backend == BLOCK_BACKEND_IDE_ATA) {
        return ide_ata_write_sector(sector, buffer);
    }
    if (active_backend == BLOCK_BACKEND_HYPERV) {
        return hyperv_storage_write_sector_ex(block_current_target,
                                              block_current_lun,
                                              sector, buffer);
    }
    if (active_backend == BLOCK_BACKEND_AHCI) {
        return ahci_write_sector(sector, buffer);
    }
    if (virtio_logical_block_size == 512U)
        return virtio_block_transfer(sector, 1, 512U, (void *)buffer,
                                     VIRTIO_BLOCK_WRITE);
    {
        UINT32 factor = virtio_logical_block_size / 512U;
        UINT32 offset = (UINT32)(sector % factor) * 512U;
        UINT32 index;
        if (!virtio_block_transfer(sector - (sector % factor), 1,
                                   virtio_logical_block_size,
                                   legacy_sector_staging,
                                   VIRTIO_BLOCK_READ)) return 0;
        for (index = 0; index < 512U; index++)
            legacy_sector_staging[offset + index] =
                ((const UINT8 *)buffer)[index];
        return virtio_block_transfer(sector - (sector % factor), 1,
                                     virtio_logical_block_size,
                                     legacy_sector_staging,
                                     VIRTIO_BLOCK_WRITE);
    }
}

void virtio_block_select_device(UINT8 target, UINT8 lun)
{
    block_current_target = target;
    block_current_lun    = lun;
}

static int virtio_direct_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                              UINT32 count, void *buffer)
{
    UINT32 factor = device->logical_block_size / 512U;
    return virtio_block_transfer(lba * factor, count,
                                 device->logical_block_size, buffer,
                                 VIRTIO_BLOCK_READ);
}

static int virtio_direct_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                               UINT32 count, const void *buffer)
{
    UINT32 factor = device->logical_block_size / 512U;
    return virtio_block_transfer(lba * factor, count,
                                 device->logical_block_size, (void *)buffer,
                                 VIRTIO_BLOCK_WRITE);
}

static int virtio_direct_flush(ASAS_BLOCK_DEVICE *device)
{
    (void)device;
    return 1;
}

static const ASAS_BLOCK_DEVICE_OPS virtio_direct_ops = {
    virtio_direct_read,
    virtio_direct_write,
    virtio_direct_flush
};

int virtio_block_register_device(void)
{
    ASAS_BLOCK_DEVICE description = { 0 };
    static UINT8 multi_block_probe[8192];
    if (active_backend != BLOCK_BACKEND_VIRTIO || virtio_sector_count == 0 ||
        block_device_find("virtio0") != 0) return 0;
    if (!virtio_block_transfer(0, 2, virtio_logical_block_size,
                               multi_block_probe, VIRTIO_BLOCK_READ)) return 0;
    description.name[0] = 'v';
    description.name[1] = 'i';
    description.name[2] = 'r';
    description.name[3] = 't';
    description.name[4] = 'i';
    description.name[5] = 'o';
    description.name[6] = '0';
    description.logical_block_size = virtio_logical_block_size;
    description.physical_block_size = virtio_logical_block_size;
    description.block_count = virtio_sector_count /
                              (virtio_logical_block_size / 512U);
    description.ops = &virtio_direct_ops;
    if (block_device_register(&description) == 0) return 0;
    logger_write("INFO", "VirtIO direct block device registered");
    logger_write("INFO", "VirtIO multi-block request verified");
    return 1;
}

int virtio_block_legacy_backend_active(void)
{
    return active_backend == BLOCK_BACKEND_IDE_ATA;
}

UINT8 virtio_block_get_current_target(void) { return block_current_target; }
UINT8 virtio_block_get_current_lun(void)    { return block_current_lun;    }

int virtio_block_get_storage_device_count(void)
{
    if (active_backend == BLOCK_BACKEND_HYPERV) {
        return hyperv_storage_get_device_count();
    }
    return 0;
}

const ASAS_STORAGE_DEVICE *virtio_block_get_storage_devices(void)
{
    if (active_backend == BLOCK_BACKEND_HYPERV) {
        return hyperv_storage_get_devices();
    }
    return (const ASAS_STORAGE_DEVICE *)0;
}
