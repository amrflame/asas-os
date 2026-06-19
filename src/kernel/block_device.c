#include "block_device.h"
#include "virtio_block.h"

#define BLOCK_CACHE_ENTRY_COUNT 16U
#define BLOCK_CACHE_MAX_BLOCK_SIZE 4096U
#define BLOCK_RETRY_DEFAULT 2U

typedef struct {
    UINT8 valid;
    UINT32 device_id;
    UINT64 lba;
    UINT32 block_size;
    UINT8 data[BLOCK_CACHE_MAX_BLOCK_SIZE];
} BLOCK_CACHE_ENTRY;

static ASAS_BLOCK_DEVICE devices[BLOCK_DEVICE_MAX_COUNT];
static ASAS_BLOCK_DEVICE_TELEMETRY telemetry[BLOCK_DEVICE_MAX_COUNT];
static BLOCK_CACHE_ENTRY cache_entries[BLOCK_CACHE_ENTRY_COUNT];
static UINT32 device_count;
static UINT32 next_cache_slot;

static void copy_name(char *destination, const char *source, UINT32 capacity)
{
    UINT32 index = 0;
    while (index + 1 < capacity && source[index] != '\0') {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

static int names_equal(const char *left, const char *right)
{
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    return *left == *right;
}

void block_device_initialize(void)
{
    UINT32 index;
    device_count = 0;
    next_cache_slot = 0;
    for (index = 0; index < BLOCK_DEVICE_MAX_COUNT; index++) {
        devices[index].id = 0;
        devices[index].name[0] = '\0';
        telemetry[index].read_ops = 0;
        telemetry[index].write_ops = 0;
        telemetry[index].flush_ops = 0;
        telemetry[index].blocks_read = 0;
        telemetry[index].blocks_written = 0;
        telemetry[index].cache_hits = 0;
        telemetry[index].cache_misses = 0;
        telemetry[index].read_ahead_ops = 0;
        telemetry[index].retries = 0;
        telemetry[index].errors = 0;
        telemetry[index].out_of_bounds = 0;
    }
    for (index = 0; index < BLOCK_CACHE_ENTRY_COUNT; index++) {
        cache_entries[index].valid = 0;
    }
}

ASAS_BLOCK_DEVICE *block_device_register(const ASAS_BLOCK_DEVICE *description)
{
    ASAS_BLOCK_DEVICE *device;
    if (description == 0 || description->ops == 0 ||
        description->logical_block_size < 512 ||
        description->logical_block_size > 4096 ||
        (description->logical_block_size &
         (description->logical_block_size - 1U)) != 0 ||
        (description->physical_block_size != 0 &&
         (description->physical_block_size <
              description->logical_block_size ||
          (description->physical_block_size &
           (description->physical_block_size - 1U)) != 0)) ||
        device_count >= BLOCK_DEVICE_MAX_COUNT) {
        return 0;
    }

    device = &devices[device_count];
    *device = *description;
    if ((device->flags & BLOCK_DEVICE_FLAG_OPTICAL) != 0) {
        device->flags |= BLOCK_DEVICE_FLAG_READ_ONLY;
    }
    device->id = device_count;
    telemetry[device->id].read_ops = 0;
    telemetry[device->id].write_ops = 0;
    telemetry[device->id].flush_ops = 0;
    telemetry[device->id].blocks_read = 0;
    telemetry[device->id].blocks_written = 0;
    telemetry[device->id].cache_hits = 0;
    telemetry[device->id].cache_misses = 0;
    telemetry[device->id].read_ahead_ops = 0;
    telemetry[device->id].retries = 0;
    telemetry[device->id].errors = 0;
    telemetry[device->id].out_of_bounds = 0;
    copy_name(device->name, description->name, sizeof(device->name));
    device_count++;
    return device;
}

static int partition_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                          UINT32 count, void *buffer)
{
    if (device == 0 || lba > ~(UINT64)0 - device->start_lba) return 0;
    return block_device_read(device->parent, device->start_lba + lba,
                             count, buffer);
}

static int partition_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                           UINT32 count, const void *buffer)
{
    if (device == 0 || lba > ~(UINT64)0 - device->start_lba) return 0;
    return block_device_write(device->parent, device->start_lba + lba,
                              count, buffer);
}

static int partition_flush(ASAS_BLOCK_DEVICE *device)
{
    return block_device_flush(device->parent);
}

static const ASAS_BLOCK_DEVICE_OPS partition_ops = {
    partition_read,
    partition_write,
    partition_flush
};

ASAS_BLOCK_DEVICE *block_device_register_partition(ASAS_BLOCK_DEVICE *parent,
                                                   UINT64 start_lba,
                                                   UINT64 block_count,
                                                   const char *name,
                                                   UINT32 flags)
{
    ASAS_BLOCK_DEVICE description;
    if (parent == 0 || block_count == 0 ||
        (parent->block_count != 0 &&
         (start_lba >= parent->block_count ||
          block_count > parent->block_count - start_lba))) {
        return 0;
    }

    description.id = 0;
    copy_name(description.name, name, sizeof(description.name));
    description.logical_block_size = parent->logical_block_size;
    description.physical_block_size = parent->physical_block_size;
    description.block_count = block_count;
    description.flags = parent->flags | flags | BLOCK_DEVICE_FLAG_PARTITION;
    description.target = parent->target;
    description.lun = parent->lun;
    description.start_lba = start_lba;
    description.parent = parent;
    description.ops = &partition_ops;
    description.driver_context = 0;
    return block_device_register(&description);
}

UINT32 block_device_count(void) { return device_count; }

ASAS_BLOCK_DEVICE *block_device_get(UINT32 index)
{
    return index < device_count ? &devices[index] : 0;
}

ASAS_BLOCK_DEVICE *block_device_find(const char *name)
{
    UINT32 index;
    for (index = 0; index < device_count; index++) {
        if (names_equal(devices[index].name, name)) return &devices[index];
    }
    return 0;
}

static int request_in_bounds(ASAS_BLOCK_DEVICE *device, UINT64 lba, UINT32 count)
{
    if (device == 0 || count == 0) return 0;
    if ((UINT64)(count - 1U) > ~(UINT64)0 - lba) return 0;
    if (device->block_count == 0) return 1;
    if (lba >= device->block_count) return 0;
    return
           (UINT64)count <= device->block_count - lba;
}

static UINT32 retry_budget(const ASAS_BLOCK_DEVICE *device)
{
    if (device == 0) return 0;
    if ((device->flags & BLOCK_DEVICE_FLAG_HOT_PLUG) != 0) return 1U;
    if ((device->flags & BLOCK_DEVICE_FLAG_REMOVABLE) != 0) return 3U;
    return BLOCK_RETRY_DEFAULT;
}

static BLOCK_CACHE_ENTRY *cache_find(ASAS_BLOCK_DEVICE *device, UINT64 lba)
{
    UINT32 index;
    if (device == 0 || (device->flags & BLOCK_DEVICE_FLAG_NO_CACHE) != 0)
        return 0;
    for (index = 0; index < BLOCK_CACHE_ENTRY_COUNT; index++) {
        if (cache_entries[index].valid &&
            cache_entries[index].device_id == device->id &&
            cache_entries[index].lba == lba &&
            cache_entries[index].block_size == device->logical_block_size)
            return &cache_entries[index];
    }
    return 0;
}

static BLOCK_CACHE_ENTRY *cache_allocate(ASAS_BLOCK_DEVICE *device, UINT64 lba)
{
    BLOCK_CACHE_ENTRY *entry;
    if (device == 0 || (device->flags & BLOCK_DEVICE_FLAG_NO_CACHE) != 0 ||
        device->logical_block_size > BLOCK_CACHE_MAX_BLOCK_SIZE) return 0;
    entry = &cache_entries[next_cache_slot++ % BLOCK_CACHE_ENTRY_COUNT];
    entry->valid = 1;
    entry->device_id = device->id;
    entry->lba = lba;
    entry->block_size = device->logical_block_size;
    return entry;
}

static void cache_store(ASAS_BLOCK_DEVICE *device, UINT64 lba, const UINT8 *data)
{
    BLOCK_CACHE_ENTRY *entry;
    UINT32 index;
    if (data == 0) return;
    entry = cache_find(device, lba);
    if (entry == 0) entry = cache_allocate(device, lba);
    if (entry == 0) return;
    for (index = 0; index < device->logical_block_size; index++)
        entry->data[index] = data[index];
}

static void cache_invalidate_device(ASAS_BLOCK_DEVICE *device)
{
    UINT32 index;
    if (device == 0) return;
    for (index = 0; index < BLOCK_CACHE_ENTRY_COUNT; index++) {
        if (cache_entries[index].valid &&
            cache_entries[index].device_id == device->id)
            cache_entries[index].valid = 0;
    }
}

static int raw_read_with_retry(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                               UINT32 count, void *buffer)
{
    UINT32 attempt;
    UINT32 retries = retry_budget(device);
    for (attempt = 0; attempt <= retries; attempt++) {
        if (device->ops->read_blocks(device, lba, count, buffer)) {
            if (attempt != 0) telemetry[device->id].retries += attempt;
            return 1;
        }
    }
    telemetry[device->id].retries += retries;
    telemetry[device->id].errors++;
    return 0;
}

static int raw_write_with_retry(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                                UINT32 count, const void *buffer)
{
    UINT32 attempt;
    UINT32 retries = retry_budget(device);
    for (attempt = 0; attempt <= retries; attempt++) {
        if (device->ops->write_blocks(device, lba, count, buffer)) {
            if (attempt != 0) telemetry[device->id].retries += attempt;
            return 1;
        }
    }
    telemetry[device->id].retries += retries;
    telemetry[device->id].errors++;
    return 0;
}

int block_device_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                      UINT32 count, void *buffer)
{
    UINT32 index;
    UINT8 *bytes = (UINT8 *)buffer;
    if (!request_in_bounds(device, lba, count) || buffer == 0 ||
        device->ops == 0 || device->ops->read_blocks == 0) {
        if (device != 0 && device->id < BLOCK_DEVICE_MAX_COUNT)
            telemetry[device->id].out_of_bounds++;
        return 0;
    }
    telemetry[device->id].read_ops++;
    telemetry[device->id].blocks_read += count;
    for (index = 0; index < count; index++) {
        UINT64 current_lba = lba + index;
        BLOCK_CACHE_ENTRY *entry = cache_find(device, current_lba);
        UINT32 byte_index;
        if (entry != 0) {
            telemetry[device->id].cache_hits++;
            for (byte_index = 0; byte_index < device->logical_block_size; byte_index++)
                bytes[index * device->logical_block_size + byte_index] =
                    entry->data[byte_index];
        } else {
            telemetry[device->id].cache_misses++;
            if (!raw_read_with_retry(device, current_lba, 1,
                                     bytes + index * device->logical_block_size))
                return 0;
            cache_store(device, current_lba,
                        bytes + index * device->logical_block_size);
        }
        if (index + 1U == count && current_lba != ~(UINT64)0 &&
            request_in_bounds(device, current_lba + 1U, 1U) &&
            cache_find(device, current_lba + 1U) == 0) {
            BLOCK_CACHE_ENTRY *ahead = cache_allocate(device, current_lba + 1U);
            if (ahead != 0 &&
                raw_read_with_retry(device, current_lba + 1U, 1, ahead->data))
                telemetry[device->id].read_ahead_ops++;
            else if (ahead != 0) ahead->valid = 0;
        }
    }
    return 1;
}

int block_device_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                       UINT32 count, const void *buffer)
{
    UINT32 index;
    const UINT8 *bytes = (const UINT8 *)buffer;
    if (!request_in_bounds(device, lba, count) || buffer == 0 ||
        (device->flags & BLOCK_DEVICE_FLAG_READ_ONLY) != 0 ||
        device->ops == 0 || device->ops->write_blocks == 0) {
        if (device != 0 && device->id < BLOCK_DEVICE_MAX_COUNT)
            telemetry[device->id].out_of_bounds++;
        return 0;
    }
    telemetry[device->id].write_ops++;
    telemetry[device->id].blocks_written += count;
    if (!raw_write_with_retry(device, lba, count, buffer)) return 0;
    for (index = 0; index < count; index++) {
        cache_store(device, lba + index,
                    bytes + index * device->logical_block_size);
    }
    return 1;
}

int block_device_flush(ASAS_BLOCK_DEVICE *device)
{
    if (device == 0 || device->ops == 0) return 0;
    telemetry[device->id].flush_ops++;
    return device->ops->flush == 0 ? 1 : device->ops->flush(device);
}

int block_device_flush_barrier(ASAS_BLOCK_DEVICE *device)
{
    if (device == 0) return 0;
    cache_invalidate_device(device);
    if (device->parent != 0) cache_invalidate_device(device->parent);
    return block_device_flush(device);
}

int block_device_get_telemetry(const ASAS_BLOCK_DEVICE *device,
                               ASAS_BLOCK_DEVICE_TELEMETRY *out)
{
    if (device == 0 || out == 0 || device->id >= BLOCK_DEVICE_MAX_COUNT)
        return 0;
    *out = telemetry[device->id];
    return 1;
}

int block_device_has_capability(const ASAS_BLOCK_DEVICE *device,
                                UINT32 capability)
{
    if (device == 0 || capability == 0 ||
        (capability & ~BLOCK_DEVICE_CAPABILITY_MASK) != 0) return 0;
    return (device->flags & capability) == capability;
}

typedef struct {
    UINT8 *storage;
    UINT32 block_size;
    UINT32 block_count;
    UINT32 fail_reads;
    UINT32 fail_writes;
    UINT32 removed;
    UINT32 flushes;
} SELF_TEST_DISK;

static UINT8 self_test_storage_512[128U * 512U];
static UINT8 self_test_storage_4096[4U * 4096U];

static void self_test_fill(UINT8 *buffer, UINT32 size, UINT8 seed)
{
    UINT32 index;
    for (index = 0; index < size; index++)
        buffer[index] = (UINT8)(seed + (UINT8)index);
}

static int self_test_copy(UINT8 *destination, const UINT8 *source, UINT32 size)
{
    UINT32 index;
    if (destination == 0 || source == 0) return 0;
    for (index = 0; index < size; index++) destination[index] = source[index];
    return 1;
}

static int self_test_disk_bounds(SELF_TEST_DISK *disk, UINT64 lba, UINT32 count)
{
    if (disk == 0 || disk->removed || count == 0) return 0;
    if (lba >= disk->block_count) return 0;
    return (UINT64)count <= (UINT64)disk->block_count - lba;
}

static int self_test_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                          UINT32 count, void *buffer)
{
    SELF_TEST_DISK *disk = (SELF_TEST_DISK *)device->driver_context;
    UINT32 index;
    if (!self_test_disk_bounds(disk, lba, count)) return 0;
    if (disk->fail_reads != 0) {
        disk->fail_reads--;
        return 0;
    }
    for (index = 0; index < count; index++) {
        UINT32 offset = (UINT32)(lba + index) * disk->block_size;
        if (!self_test_copy((UINT8 *)buffer + index * disk->block_size,
                            disk->storage + offset, disk->block_size))
            return 0;
    }
    return 1;
}

static int self_test_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                           UINT32 count, const void *buffer)
{
    SELF_TEST_DISK *disk = (SELF_TEST_DISK *)device->driver_context;
    UINT32 index;
    if (!self_test_disk_bounds(disk, lba, count)) return 0;
    if (disk->fail_writes != 0) {
        disk->fail_writes--;
        return 0;
    }
    for (index = 0; index < count; index++) {
        UINT32 offset = (UINT32)(lba + index) * disk->block_size;
        if (!self_test_copy(disk->storage + offset,
                            (const UINT8 *)buffer + index * disk->block_size,
                            disk->block_size))
            return 0;
    }
    return 1;
}

static int self_test_flush(ASAS_BLOCK_DEVICE *device)
{
    SELF_TEST_DISK *disk = (SELF_TEST_DISK *)device->driver_context;
    if (disk == 0 || disk->removed) return 0;
    disk->flushes++;
    return 1;
}

static const ASAS_BLOCK_DEVICE_OPS self_test_ops = {
    self_test_read,
    self_test_write,
    self_test_flush
};

int block_device_self_test(void)
{
    ASAS_BLOCK_DEVICE description = { 0 };
    ASAS_BLOCK_DEVICE *disk;
    ASAS_BLOCK_DEVICE *partition;
    ASAS_BLOCK_DEVICE *writable;
    ASAS_BLOCK_DEVICE *large_sector;
    ASAS_BLOCK_DEVICE_TELEMETRY stats;
    SELF_TEST_DISK optical_ctx;
    SELF_TEST_DISK writable_ctx;
    SELF_TEST_DISK large_ctx;
    UINT8 buffer[4096];
    UINT8 write_buffer[4096];

    self_test_fill(self_test_storage_512, sizeof(self_test_storage_512), 0x10);
    self_test_fill(self_test_storage_4096, sizeof(self_test_storage_4096), 0x80);
    self_test_fill(buffer, sizeof(buffer), 0);
    self_test_fill(write_buffer, sizeof(write_buffer), 0x5a);

    optical_ctx.storage = self_test_storage_512;
    optical_ctx.block_size = 512;
    optical_ctx.block_count = 128;
    optical_ctx.fail_reads = 0;
    optical_ctx.fail_writes = 0;
    optical_ctx.removed = 0;
    optical_ctx.flushes = 0;

    description.name[0] = 't';
    description.name[1] = 'e';
    description.name[2] = 's';
    description.name[3] = 't';
    description.name[4] = '0';
    description.logical_block_size = 512;
    description.physical_block_size = 4096;
    description.block_count = 128;
    description.flags = BLOCK_DEVICE_FLAG_REMOVABLE |
                        BLOCK_DEVICE_FLAG_OPTICAL |
                        BLOCK_DEVICE_FLAG_HOT_PLUG;
    description.ops = &self_test_ops;
    description.driver_context = &optical_ctx;

    block_device_initialize();
    disk = block_device_register(&description);
    if (disk == 0 ||
        !block_device_has_capability(disk, BLOCK_DEVICE_FLAG_READ_ONLY) ||
        !block_device_has_capability(disk, BLOCK_DEVICE_FLAG_REMOVABLE) ||
        !block_device_has_capability(disk, BLOCK_DEVICE_FLAG_OPTICAL) ||
        !block_device_has_capability(disk, BLOCK_DEVICE_FLAG_HOT_PLUG) ||
        block_device_write(disk, 0, 1, buffer) ||
        block_device_read(disk, 127, 2, buffer)) return 0;

    partition = block_device_register_partition(disk, 8, 32, "test0p1", 0);
    if (partition == 0 ||
        !block_device_has_capability(partition, BLOCK_DEVICE_FLAG_READ_ONLY) ||
        !block_device_has_capability(partition, BLOCK_DEVICE_FLAG_REMOVABLE) ||
        !block_device_has_capability(partition, BLOCK_DEVICE_FLAG_HOT_PLUG) ||
        block_device_write(partition, 0, 1, buffer) ||
        block_device_read(partition, 31, 2, buffer)) return 0;

    writable_ctx.storage = self_test_storage_512;
    writable_ctx.block_size = 512;
    writable_ctx.block_count = 128;
    writable_ctx.fail_reads = 1;
    writable_ctx.fail_writes = 1;
    writable_ctx.removed = 0;
    writable_ctx.flushes = 0;
    copy_name(description.name, "cache0", sizeof(description.name));
    description.logical_block_size = 512;
    description.physical_block_size = 512;
    description.block_count = 128;
    description.flags = BLOCK_DEVICE_FLAG_REMOVABLE;
    description.start_lba = 0;
    description.parent = 0;
    description.ops = &self_test_ops;
    description.driver_context = &writable_ctx;
    writable = block_device_register(&description);
    if (writable == 0 ||
        !block_device_read(writable, 2, 1, buffer) ||
        !block_device_get_telemetry(writable, &stats) ||
        stats.cache_misses == 0 || stats.read_ahead_ops == 0 ||
        stats.retries == 0) return 0;
    self_test_storage_512[2U * 512U] = 0xaa;
    if (!block_device_read(writable, 2, 1, buffer) ||
        buffer[0] == 0xaa ||
        !block_device_flush_barrier(writable) ||
        !block_device_read(writable, 2, 1, buffer) ||
        buffer[0] != 0xaa) return 0;
    if (!block_device_write(writable, 4, 1, write_buffer) ||
        self_test_storage_512[4U * 512U] != write_buffer[0] ||
        block_device_write(writable, 128, 1, write_buffer))
        return 0;
    writable_ctx.removed = 1;
    if (block_device_read(writable, 5, 1, buffer)) return 0;
    writable_ctx.removed = 0;

    large_ctx.storage = self_test_storage_4096;
    large_ctx.block_size = 4096;
    large_ctx.block_count = 4;
    large_ctx.fail_reads = 0;
    large_ctx.fail_writes = 4;
    large_ctx.removed = 0;
    large_ctx.flushes = 0;
    copy_name(description.name, "sect4k", sizeof(description.name));
    description.logical_block_size = 4096;
    description.physical_block_size = 4096;
    description.block_count = 4;
    description.flags = 0;
    description.driver_context = &large_ctx;
    large_sector = block_device_register(&description);
    if (large_sector == 0 ||
        !block_device_read(large_sector, 3, 1, buffer) ||
        block_device_read(large_sector, 4, 1, buffer) ||
        block_device_write(large_sector, 1, 1, write_buffer) ||
        self_test_storage_4096[4096U] == write_buffer[0])
        return 0;
    return 1;
}

static int legacy_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                       UINT32 count, void *buffer)
{
    UINT32 index;
    UINT8 *bytes = (UINT8 *)buffer;
    virtio_block_select_device(device->target, device->lun);
    for (index = 0; index < count; index++) {
        if (!virtio_block_read_sector(lba + index,
                                      bytes + index * device->logical_block_size)) {
            return 0;
        }
    }
    return 1;
}

static int legacy_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                        UINT32 count, const void *buffer)
{
    UINT32 index;
    const UINT8 *bytes = (const UINT8 *)buffer;
    virtio_block_select_device(device->target, device->lun);
    for (index = 0; index < count; index++) {
        if (!virtio_block_write_sector(lba + index,
                                       bytes + index * device->logical_block_size)) {
            return 0;
        }
    }
    return 1;
}

static int legacy_flush(ASAS_BLOCK_DEVICE *device)
{
    (void)device;
    return 1;
}

static const ASAS_BLOCK_DEVICE_OPS legacy_ops = {
    legacy_read,
    legacy_write,
    legacy_flush
};

static ASAS_BLOCK_DEVICE *register_legacy(UINT8 target, UINT8 lun,
                                          UINT64 blocks, UINT32 block_size,
                                          UINT32 flags, UINT32 number)
{
    ASAS_BLOCK_DEVICE description;
    char name[16] = "disk0";
    name[4] = (char)('0' + number);
    description.id = 0;
    copy_name(description.name, name, sizeof(description.name));
    description.logical_block_size = block_size == 0 ? 512U : block_size;
    description.physical_block_size = description.logical_block_size;
    description.block_count = blocks;
    description.flags = flags;
    description.target = target;
    description.lun = lun;
    description.start_lba = 0;
    description.parent = 0;
    description.ops = &legacy_ops;
    description.driver_context = 0;
    return block_device_register(&description);
}

int block_device_register_legacy_devices(void)
{
    const ASAS_STORAGE_DEVICE *storage_devices;
    int count = virtio_block_get_storage_device_count();
    int index;

    if (device_count != 0) return (int)device_count;
    storage_devices = virtio_block_get_storage_devices();
    if (count > 0 && storage_devices != 0) {
        for (index = 0; index < count && device_count < BLOCK_DEVICE_MAX_COUNT; index++) {
            UINT32 flags = 0;
            if (!storage_devices[index].valid) continue;
            if (storage_devices[index].is_cdrom) {
                flags |= BLOCK_DEVICE_FLAG_OPTICAL | BLOCK_DEVICE_FLAG_READ_ONLY;
            }
            (void)register_legacy(storage_devices[index].target,
                                  storage_devices[index].lun,
                                  storage_devices[index].sector_count,
                                  storage_devices[index].sector_size,
                                  flags, device_count);
        }
    }

    if (device_count == 0) {
        (void)register_legacy(virtio_block_get_current_target(),
                              virtio_block_get_current_lun(),
                              0, 512, 0, 0);
    }
    return (int)device_count;
}
