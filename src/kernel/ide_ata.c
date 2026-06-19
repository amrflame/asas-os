#include "ide_ata.h"
#include "logger.h"

#pragma intrinsic(__inbyte)
unsigned char __inbyte(unsigned short port);
#pragma intrinsic(__inword)
unsigned short __inword(unsigned short port);
#pragma intrinsic(__outbyte)
void __outbyte(unsigned short port, unsigned char value);
#pragma intrinsic(__outword)
void __outword(unsigned short port, unsigned short value);

#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376
#define ATA_REG_DATA 0
#define ATA_REG_ERROR 1
#define ATA_REG_SECTOR_COUNT 2
#define ATA_REG_LBA_LOW 3
#define ATA_REG_LBA_MID 4
#define ATA_REG_LBA_HIGH 5
#define ATA_REG_DRIVE 6
#define ATA_REG_STATUS 7
#define ATA_REG_COMMAND 7
#define ATA_STATUS_ERR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_BSY 0x80
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY 0xEC

typedef struct {
    UINT16 io_base;
    UINT16 control_base;
    UINT8 drive_select;
} IDE_ATA_DEVICE;

static IDE_ATA_DEVICE active_device;
static int active_device_ready;

static void io_delay(UINT16 control_base)
{
    (void)__inbyte(control_base);
    (void)__inbyte(control_base);
    (void)__inbyte(control_base);
    (void)__inbyte(control_base);
}

static int wait_not_busy(UINT16 io_base)
{
    UINT32 timeout;

    for (timeout = 0; timeout < 1000000U; timeout++) {
        if ((__inbyte((UINT16)(io_base + ATA_REG_STATUS)) & ATA_STATUS_BSY) == 0) {
            return 1;
        }
    }
    return 0;
}

static int wait_drq(UINT16 io_base)
{
    UINT32 timeout;

    for (timeout = 0; timeout < 1000000U; timeout++) {
        UINT8 status = __inbyte((UINT16)(io_base + ATA_REG_STATUS));

        if ((status & ATA_STATUS_ERR) != 0) {
            return 0;
        }
        if ((status & ATA_STATUS_BSY) == 0 && (status & ATA_STATUS_DRQ) != 0) {
            return 1;
        }
    }
    return 0;
}

static int identify_device(UINT16 io_base, UINT16 control_base, UINT8 drive_select)
{
    UINT32 index;

    __outbyte(control_base, 0x02);
    __outbyte((UINT16)(io_base + ATA_REG_DRIVE), drive_select);
    io_delay(control_base);
    __outbyte((UINT16)(io_base + ATA_REG_SECTOR_COUNT), 0);
    __outbyte((UINT16)(io_base + ATA_REG_LBA_LOW), 0);
    __outbyte((UINT16)(io_base + ATA_REG_LBA_MID), 0);
    __outbyte((UINT16)(io_base + ATA_REG_LBA_HIGH), 0);
    __outbyte((UINT16)(io_base + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);

    if (__inbyte((UINT16)(io_base + ATA_REG_STATUS)) == 0) {
        return 0;
    }
    if (!wait_not_busy(io_base)) {
        return 0;
    }
    if (
        __inbyte((UINT16)(io_base + ATA_REG_LBA_MID)) != 0 ||
        __inbyte((UINT16)(io_base + ATA_REG_LBA_HIGH)) != 0 ||
        !wait_drq(io_base)
    ) {
        return 0;
    }

    for (index = 0; index < 256; index++) {
        (void)__inword((UINT16)(io_base + ATA_REG_DATA));
    }
    return 1;
}

int ide_ata_initialize(void)
{
    static const IDE_ATA_DEVICE candidates[] = {
        { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, 0xE0 },
        { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, 0xF0 },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0xE0 },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0xF0 }
    };
    UINT32 index;

    active_device_ready = 0;
    for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
        if (
            identify_device(
                candidates[index].io_base,
                candidates[index].control_base,
                candidates[index].drive_select
            )
        ) {
            active_device = candidates[index];
            active_device_ready = 1;
            logger_write("INFO", "IDE ATA device initialized");
            return 1;
        }
    }

    logger_write("INFO", "IDE ATA device unavailable");
    return 0;
}

static int select_lba28(UINT64 lba)
{
    if (!active_device_ready || lba > 0x0FFFFFFFULL || !wait_not_busy(active_device.io_base)) {
        return 0;
    }

    __outbyte(
        (UINT16)(active_device.io_base + ATA_REG_DRIVE),
        (UINT8)(active_device.drive_select | ((lba >> 24) & 0x0F))
    );
    io_delay(active_device.control_base);
    __outbyte((UINT16)(active_device.io_base + ATA_REG_SECTOR_COUNT), 1);
    __outbyte((UINT16)(active_device.io_base + ATA_REG_LBA_LOW), (UINT8)lba);
    __outbyte((UINT16)(active_device.io_base + ATA_REG_LBA_MID), (UINT8)(lba >> 8));
    __outbyte((UINT16)(active_device.io_base + ATA_REG_LBA_HIGH), (UINT8)(lba >> 16));
    return 1;
}

int ide_ata_read_sector(UINT64 lba, void *buffer)
{
    UINT16 *words = (UINT16 *)buffer;
    UINT32 index;

    if (!select_lba28(lba)) {
        return 0;
    }

    __outbyte((UINT16)(active_device.io_base + ATA_REG_COMMAND), ATA_CMD_READ_SECTORS);
    if (!wait_drq(active_device.io_base)) {
        return 0;
    }

    for (index = 0; index < 256; index++) {
        words[index] = __inword((UINT16)(active_device.io_base + ATA_REG_DATA));
    }
    return 1;
}

int ide_ata_write_sector(UINT64 lba, const void *buffer)
{
    const UINT16 *words = (const UINT16 *)buffer;
    UINT32 index;

    if (!select_lba28(lba)) {
        return 0;
    }

    __outbyte((UINT16)(active_device.io_base + ATA_REG_COMMAND), ATA_CMD_WRITE_SECTORS);
    if (!wait_drq(active_device.io_base)) {
        return 0;
    }

    for (index = 0; index < 256; index++) {
        __outword((UINT16)(active_device.io_base + ATA_REG_DATA), words[index]);
    }
    __outbyte((UINT16)(active_device.io_base + ATA_REG_COMMAND), ATA_CMD_CACHE_FLUSH);
    return wait_not_busy(active_device.io_base);
}
