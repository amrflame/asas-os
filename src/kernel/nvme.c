#include "nvme.h"
#include "block_device.h"
#include "logger.h"
#include "pci.h"

#define NVME_REG_CAP 0x0000
#define NVME_REG_CC 0x0014
#define NVME_REG_CSTS 0x001C
#define NVME_REG_AQA 0x0024
#define NVME_REG_ASQ 0x0028
#define NVME_REG_ACQ 0x0030
#define NVME_REG_DOORBELL_BASE 0x1000

#define NVME_CC_ENABLE 0x00000001U
#define NVME_CC_IOCQES_16 (4U << 20)
#define NVME_CC_IOSQES_64 (6U << 16)
#define NVME_CSTS_READY 0x00000001U
#define NVME_QUEUE_ENTRIES 16U
#define NVME_TIMEOUT 1000000U
#define NVME_SECTOR_SIZE 512U

typedef struct {
    UINT32 cdw0;
    UINT32 nsid;
    UINT64 reserved0;
    UINT64 metadata;
    UINT64 prp1;
    UINT64 prp2;
    UINT32 cdw10;
    UINT32 cdw11;
    UINT32 cdw12;
    UINT32 cdw13;
    UINT32 cdw14;
    UINT32 cdw15;
} NVME_SUBMISSION;

typedef struct {
    UINT32 result;
    UINT32 reserved;
    UINT16 sq_head;
    UINT16 sq_id;
    UINT16 command_id;
    UINT16 status;
} NVME_COMPLETION;

static volatile UINT8 *nvme_mmio;
static UINT32 doorbell_stride;
static UINT16 admin_sq_tail;
static UINT16 io_sq_tail;
static UINT16 admin_cq_head;
static UINT16 io_cq_head;
static UINT8 admin_phase = 1;
static UINT8 io_phase = 1;
static UINT16 last_completion_status;
static UINT64 admin_sq_phys;
static UINT64 admin_cq_phys;
static UINT64 io_sq_phys;
static UINT64 io_cq_phys;
static UINT64 nvme_dma_sector_phys;
static UINT64 nvme_block_count;
static UINT32 nvme_block_size;
static UINT8 nvme_initialized;
static UINT8 nvme_has_volatile_write_cache;

static NVME_SUBMISSION *admin_sq;
static volatile NVME_COMPLETION *admin_cq;
static NVME_SUBMISSION *io_sq;
static volatile NVME_COMPLETION *io_cq;
static UINT8 *nvme_dma_sector;

static UINT32 read32(UINT32 offset)
{
    return *(volatile UINT32 *)(nvme_mmio + offset);
}

static UINT64 read64(UINT32 offset)
{
    UINT32 low = read32(offset);
    UINT32 high = read32(offset + 4);

    return ((UINT64)high << 32) | low;
}

static void write32(UINT32 offset, UINT32 value)
{
    *(volatile UINT32 *)(nvme_mmio + offset) = value;
}

static void write64(UINT32 offset, UINT64 value)
{
    write32(offset, (UINT32)value);
    write32(offset + 4, (UINT32)(value >> 32));
}

static void zero_bytes(void *buffer, UINT32 size)
{
    UINT8 *bytes = (UINT8 *)buffer;
    UINT32 index;

    for (index = 0; index < size; index++) {
        bytes[index] = 0;
    }
}

static int wait_ready(UINT32 ready)
{
    UINT32 timeout;

    for (timeout = 0; timeout < NVME_TIMEOUT; timeout++) {
        if ((read32(NVME_REG_CSTS) & NVME_CSTS_READY) == ready) {
            return 1;
        }
    }
    return 0;
}

static void ring_sq(UINT16 qid, UINT16 tail)
{
    UINT32 offset = NVME_REG_DOORBELL_BASE + (2U * qid * doorbell_stride);

    write32(offset, tail);
}

static void ring_cq(UINT16 qid, UINT16 head)
{
    UINT32 offset = NVME_REG_DOORBELL_BASE + ((2U * qid + 1U) * doorbell_stride);

    write32(offset, head);
}

static int wait_completion(volatile NVME_COMPLETION *cq, UINT16 *head,
                           UINT8 *phase, UINT16 qid)
{
    UINT32 timeout;

    for (timeout = 0; timeout < NVME_TIMEOUT; timeout++) {
        if ((cq[*head].status & 1U) == *phase) {
            UINT16 status = (UINT16)(cq[*head].status >> 1);

            last_completion_status = status;
            *head = (UINT16)(*head + 1U);
            if (*head == NVME_QUEUE_ENTRIES) {
                *head = 0;
                *phase ^= 1U;
            }
            ring_cq(qid, *head);
            return status == 0;
        }
    }
    return 0;
}

static int admin_command(UINT8 opcode, UINT32 nsid, UINT32 cdw10,
                         UINT32 cdw11, UINT64 prp1)
{
    NVME_SUBMISSION *cmd = &admin_sq[admin_sq_tail];

    zero_bytes(cmd, sizeof(*cmd));
    cmd->cdw0 = opcode | ((UINT32)(admin_sq_tail + 1U) << 16);
    cmd->nsid = nsid;
    cmd->prp1 = prp1;
    cmd->cdw10 = cdw10;
    cmd->cdw11 = cdw11;
    admin_sq_tail = (UINT16)((admin_sq_tail + 1U) % NVME_QUEUE_ENTRIES);
    ring_sq(0, admin_sq_tail);
    return wait_completion(admin_cq, &admin_cq_head, &admin_phase, 0);
}

int nvme_initialize(const ASAS_PCI_DEVICE *device, ASAS_FRAME_ALLOCATOR *allocator)
{
    UINT64 cap;
    UINT64 bar = device->bars[0] & ~0x0FULL;
    UINT8 formatted_lba;
    UINT8 lba_shift;

    nvme_initialized = 0;
    nvme_block_count = 0;
    nvme_block_size = 0;
    nvme_has_volatile_write_cache = 0;

    if (bar == 0 || (device->bars[0] & 1U) != 0) {
        return 0;
    }
    if ((device->bars[0] & 0x04U) != 0) {
        bar |= (UINT64)device->bars[1] << 32;
    }

    pci_enable_bus_mastering(device);
    nvme_mmio = (volatile UINT8 *)(UINTN)bar;
    cap = read64(NVME_REG_CAP);
    doorbell_stride = 4U << ((cap >> 32) & 0x0FU);

    write32(NVME_REG_CC, read32(NVME_REG_CC) & ~NVME_CC_ENABLE);
    if (!wait_ready(0)) {
        logger_write("INFO", "NVMe disable timeout");
        return 0;
    }

    admin_sq_phys = frame_allocate(allocator);
    admin_cq_phys = frame_allocate(allocator);
    io_sq_phys = frame_allocate(allocator);
    io_cq_phys = frame_allocate(allocator);
    nvme_dma_sector_phys = frame_allocate(allocator);
    if (
        admin_sq_phys == 0 ||
        admin_cq_phys == 0 ||
        io_sq_phys == 0 ||
        io_cq_phys == 0 ||
        nvme_dma_sector_phys == 0
    ) {
        return 0;
    }
    admin_sq = (NVME_SUBMISSION *)(UINTN)admin_sq_phys;
    admin_cq = (volatile NVME_COMPLETION *)(UINTN)admin_cq_phys;
    io_sq = (NVME_SUBMISSION *)(UINTN)io_sq_phys;
    io_cq = (volatile NVME_COMPLETION *)(UINTN)io_cq_phys;
    nvme_dma_sector = (UINT8 *)(UINTN)nvme_dma_sector_phys;
    zero_bytes(admin_sq, 4096);
    zero_bytes((void *)admin_cq, 4096);
    zero_bytes(io_sq, 4096);
    zero_bytes((void *)io_cq, 4096);
    zero_bytes(nvme_dma_sector, 4096);
    admin_sq_tail = 0;
    io_sq_tail = 0;
    admin_cq_head = 0;
    io_cq_head = 0;
    admin_phase = 1;
    io_phase = 1;

    write32(NVME_REG_AQA, ((NVME_QUEUE_ENTRIES - 1U) << 16) | (NVME_QUEUE_ENTRIES - 1U));
    write64(NVME_REG_ASQ, admin_sq_phys);
    write64(NVME_REG_ACQ, admin_cq_phys);
    write32(NVME_REG_CC, NVME_CC_ENABLE | NVME_CC_IOCQES_16 | NVME_CC_IOSQES_64);
    if (!wait_ready(NVME_CSTS_READY)) {
        logger_write("INFO", "NVMe enable timeout");
        return 0;
    }

    zero_bytes(nvme_dma_sector, 4096);
    if (!admin_command(0x06, 0, 1, 0, nvme_dma_sector_phys)) {
        logger_write("INFO", "NVMe identify controller failed");
        return 0;
    }
    nvme_has_volatile_write_cache = nvme_dma_sector[525] & 1U;

    zero_bytes(nvme_dma_sector, 4096);
    if (!admin_command(0x06, 1, 0, 0, nvme_dma_sector_phys)) {
        logger_write("INFO", "NVMe identify namespace failed");
        return 0;
    }
    nvme_block_count = *(UINT64 *)(void *)nvme_dma_sector;
    formatted_lba = nvme_dma_sector[26] & 0x0FU;
    lba_shift = nvme_dma_sector[128U + (UINT32)formatted_lba * 4U + 2U];
    if (nvme_block_count == 0 || lba_shift < 9U || lba_shift > 12U) {
        logger_write("INFO", "NVMe namespace block format unsupported");
        return 0;
    }
    nvme_block_size = 1U << lba_shift;

    if (!admin_command(0x05, 0, ((NVME_QUEUE_ENTRIES - 1U) << 16) | 1U,
                       1U, io_cq_phys)) {
        logger_write("INFO", "NVMe create IO completion queue failed");
        return 0;
    }
    if (!admin_command(0x01, 0, ((NVME_QUEUE_ENTRIES - 1U) << 16) | 1U,
                       0x00010001U, io_sq_phys)) {
        logger_write("INFO", "NVMe create IO submission queue failed");
        return 0;
    }

    nvme_initialized = 1;
    return 1;
}

static int nvme_transfer(UINT8 opcode, UINT64 lba, UINT32 count, void *buffer)
{
    NVME_SUBMISSION *cmd;
    UINT32 index;

    UINT32 transfer_bytes;
    if (!nvme_initialized || buffer == 0 || count == 0 ||
        count > 4096U / nvme_block_size || lba >= nvme_block_count ||
        (UINT64)count > nvme_block_count - lba) {
        return 0;
    }
    transfer_bytes = count * nvme_block_size;

    if (opcode == 0x01U) {
        for (index = 0; index < transfer_bytes; index++) {
            nvme_dma_sector[index] = ((const UINT8 *)buffer)[index];
        }
    } else {
        zero_bytes(nvme_dma_sector, transfer_bytes);
    }
    cmd = &io_sq[io_sq_tail];
    zero_bytes(cmd, sizeof(*cmd));
    cmd->cdw0 = opcode | ((UINT32)(io_sq_tail + 1U) << 16);
    cmd->nsid = 1;
    cmd->prp1 = nvme_dma_sector_phys;
    cmd->cdw10 = (UINT32)lba;
    cmd->cdw11 = (UINT32)(lba >> 32);
    cmd->cdw12 = (count - 1U) | (opcode == 0x01U ? (1U << 30) : 0);
    io_sq_tail = (UINT16)((io_sq_tail + 1U) % NVME_QUEUE_ENTRIES);
    ring_sq(1, io_sq_tail);

    if (!wait_completion(io_cq, &io_cq_head, &io_phase, 1)) {
        return 0;
    }

    if (opcode == 0x02U) {
        for (index = 0; index < transfer_bytes; index++) {
            ((UINT8 *)buffer)[index] = nvme_dma_sector[index];
        }
    }
    return 1;
}

int nvme_read_sector(UINT64 lba, void *buffer)
{
    if (nvme_block_size != NVME_SECTOR_SIZE) return 0;
    return nvme_transfer(0x02U, lba, 1, buffer);
}

int nvme_write_sector(UINT64 lba, const void *buffer)
{
    if (nvme_block_size != NVME_SECTOR_SIZE) return 0;
    return nvme_transfer(0x01U, lba, 1, (void *)buffer);
}

int nvme_flush(void)
{
    if (!nvme_initialized) return 0;
    /* Writes use FUA, so no volatile data remains pending at this layer. */
    return 1;
}

static int nvme_device_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                            UINT32 count, void *buffer)
{
    UINT32 chunk;
    UINT32 maximum = 4096U / nvme_block_size;
    UINT8 *bytes = (UINT8 *)buffer;
    (void)device;
    while (count != 0) {
        chunk = count > maximum ? maximum : count;
        if (!nvme_transfer(0x02U, lba, chunk, bytes)) return 0;
        lba += chunk;
        bytes += (UINT64)chunk * nvme_block_size;
        count -= chunk;
    }
    return 1;
}

static int nvme_device_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                             UINT32 count, const void *buffer)
{
    UINT32 chunk;
    UINT32 maximum = 4096U / nvme_block_size;
    const UINT8 *bytes = (const UINT8 *)buffer;
    (void)device;
    while (count != 0) {
        chunk = count > maximum ? maximum : count;
        if (!nvme_transfer(0x01U, lba, chunk, (void *)bytes)) return 0;
        lba += chunk;
        bytes += (UINT64)chunk * nvme_block_size;
        count -= chunk;
    }
    return 1;
}

static int nvme_device_flush(ASAS_BLOCK_DEVICE *device)
{
    (void)device;
    return nvme_flush();
}

static const ASAS_BLOCK_DEVICE_OPS nvme_device_ops = {
    nvme_device_read,
    nvme_device_write,
    nvme_device_flush
};

int nvme_register_block_device(void)
{
    ASAS_BLOCK_DEVICE description = { 0 };
    ASAS_BLOCK_DEVICE *registered;
    static UINT8 multi_block_probe[8192];
    if (!nvme_initialized || block_device_find("nvme0") != 0) return 0;
    if (!nvme_device_read(0, 0, 2, multi_block_probe)) return 0;
    description.name[0] = 'n';
    description.name[1] = 'v';
    description.name[2] = 'm';
    description.name[3] = 'e';
    description.name[4] = '0';
    description.logical_block_size = nvme_block_size;
    description.physical_block_size = nvme_block_size;
    description.block_count = nvme_block_count;
    description.ops = &nvme_device_ops;
    registered = block_device_register(&description);
    if (registered == 0) return 0;
    logger_write("INFO", "NVMe direct block device registered");
    logger_write("INFO", "NVMe multi-block request verified");
    return 1;
}
