#include "virtual_disk.h"
#include "filesystem.h"
#include "heap.h"
#include "vfs.h"

#define VDISK_SECTOR_SIZE 512U

typedef struct {
    ASAS_VDISK_INFO info;
} VDISK_SLOT;

static VDISK_SLOT vdisks[VDISK_MAX_COUNT];

static int validate_fixed_vhd_footer(const UINT8 *footer, UINT64 file_size,
                                     UINT64 *data_size);

static int strings_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) return 0;
        left++;
        right++;
    }
    return *left == *right;
}

static char lower_char(char value)
{
    if (value >= 'A' && value <= 'Z') return (char)(value - 'A' + 'a');
    return value;
}

static int has_suffix(const char *name, const char *suffix)
{
    UINT32 name_len = 0;
    UINT32 suffix_len = 0;
    UINT32 index;
    while (name[name_len] != '\0') name_len++;
    while (suffix[suffix_len] != '\0') suffix_len++;
    if (suffix_len == 0 || name_len < suffix_len) return 0;
    for (index = 0; index < suffix_len; index++) {
        if (lower_char(name[name_len - suffix_len + index]) !=
            lower_char(suffix[index])) return 0;
    }
    return 1;
}

static void copy_string(char *destination, const char *source, UINT32 capacity)
{
    UINT32 index = 0;
    while (index + 1U < capacity && source[index] != '\0') {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

void virtual_disk_initialize(void)
{
    UINT32 index;
    for (index = 0; index < VDISK_MAX_COUNT; index++) {
        vdisks[index].info.active = 0;
        vdisks[index].info.name[0] = '\0';
        vdisks[index].info.format[0] = '\0';
        vdisks[index].info.path[0] = '\0';
        vdisks[index].info.device = 0;
        vdisks[index].info.size = 0;
        vdisks[index].info.file_size = 0;
        vdisks[index].info.read_only = 0;
    }
}

static UINT32 read_be32(const UINT8 *data)
{
    return ((UINT32)data[0] << 24) | ((UINT32)data[1] << 16) |
           ((UINT32)data[2] << 8) | (UINT32)data[3];
}

static UINT64 read_be64(const UINT8 *data)
{
    return ((UINT64)read_be32(data) << 32) | read_be32(data + 4U);
}

static void write_be32(UINT8 *data, UINT32 value)
{
    data[0] = (UINT8)(value >> 24);
    data[1] = (UINT8)(value >> 16);
    data[2] = (UINT8)(value >> 8);
    data[3] = (UINT8)value;
}

static int image_read_all(const char *path, UINT8 **out, UINT64 *size)
{
    UINT64 image_size;
    UINT64 handle;
    UINT8 *image;
    UINT64 bytes;
    if (path == 0 || out == 0 || size == 0) return 0;
    image_size = vfs_file_size(path);
    if (image_size == 0) return 0;
    image = (UINT8 *)kmalloc((UINTN)image_size);
    if (image == 0) return 0;
    handle = vfs_open(path);
    if (handle == 0) {
        kfree(image);
        return 0;
    }
    bytes = vfs_read(handle, image, image_size);
    (void)vfs_close(handle);
    if (bytes < image_size) {
        kfree(image);
        return 0;
    }
    *out = image;
    *size = image_size;
    return 1;
}

static int rewrite_fixed_vhd_footer(const char *path)
{
    UINT8 *image;
    UINT64 file_size;
    UINT64 data_size = 0;
    UINT8 *footer;
    UINT32 index;
    UINT32 sum = 0;
    int ok;
    if (!image_read_all(path, &image, &file_size)) return 0;
    footer = image + file_size - VDISK_SECTOR_SIZE;
    ok = validate_fixed_vhd_footer(footer, file_size, &data_size);
    if (ok) {
        footer[64] = footer[65] = footer[66] = footer[67] = 0;
        for (index = 0; index < VDISK_SECTOR_SIZE; index++)
            sum += footer[index];
        write_be32(footer + 64U, ~sum);
        ok = vfs_write_file(path, image, file_size);
    }
    kfree(image);
    return ok;
}

static int raw_read_blocks(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                           UINT32 count, void *buffer)
{
    VDISK_SLOT *slot;
    UINT64 image_size;
    UINT64 handle;
    UINT8 *image;
    UINT64 bytes;
    UINT64 offset;
    UINT64 length;
    UINT64 index;
    if (device == 0 || buffer == 0 || device->driver_context == 0)
        return 0;
    slot = (VDISK_SLOT *)device->driver_context;
    if (!slot->info.active) return 0;
    image_size = vfs_file_size(slot->info.path);
    offset = lba * (UINT64)VDISK_SECTOR_SIZE;
    length = (UINT64)count * VDISK_SECTOR_SIZE;
    if (image_size == 0 || image_size != slot->info.file_size ||
        offset > slot->info.size || length > slot->info.size - offset)
        return 0;
    image = (UINT8 *)kmalloc((UINTN)image_size);
    if (image == 0) return 0;
    handle = vfs_open(slot->info.path);
    if (handle == 0) {
        kfree(image);
        return 0;
    }
    bytes = vfs_read(handle, image, image_size);
    (void)vfs_close(handle);
    if (bytes < image_size) {
        kfree(image);
        return 0;
    }
    for (index = 0; index < length; index++)
        ((UINT8 *)buffer)[index] = image[offset + index];
    kfree(image);
    return 1;
}

static int raw_write_blocks(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                            UINT32 count, const void *buffer)
{
    VDISK_SLOT *slot;
    UINT64 image_size;
    UINT64 handle;
    UINT8 *image;
    UINT64 bytes;
    UINT64 offset;
    UINT64 length;
    UINT64 index;
    if (device == 0 || buffer == 0 || device->driver_context == 0)
        return 0;
    slot = (VDISK_SLOT *)device->driver_context;
    if (!slot->info.active || slot->info.read_only) return 0;
    image_size = vfs_file_size(slot->info.path);
    offset = lba * (UINT64)VDISK_SECTOR_SIZE;
    length = (UINT64)count * VDISK_SECTOR_SIZE;
    if (image_size == 0 || image_size != slot->info.file_size ||
        offset > slot->info.size || length > slot->info.size - offset)
        return 0;
    image = (UINT8 *)kmalloc((UINTN)image_size);
    if (image == 0) return 0;
    handle = vfs_open(slot->info.path);
    if (handle == 0) {
        kfree(image);
        return 0;
    }
    bytes = vfs_read(handle, image, image_size);
    (void)vfs_close(handle);
    if (bytes < image_size) {
        kfree(image);
        return 0;
    }
    for (index = 0; index < length; index++)
        image[offset + index] = ((const UINT8 *)buffer)[index];
    if (!vfs_write_file(slot->info.path, image, image_size)) {
        kfree(image);
        return 0;
    }
    kfree(image);
    return 1;
}

static int raw_flush(ASAS_BLOCK_DEVICE *device)
{
    VDISK_SLOT *slot;
    if (device == 0 || device->driver_context == 0) return 0;
    slot = (VDISK_SLOT *)device->driver_context;
    if (!slot->info.active) return 0;
    if (strings_equal(slot->info.format, "vhd-fixed") && !slot->info.read_only)
        return rewrite_fixed_vhd_footer(slot->info.path);
    return 1;
}

static const ASAS_BLOCK_DEVICE_OPS raw_ops = {
    raw_read_blocks,
    raw_write_blocks,
    raw_flush
};

static ASAS_BLOCK_DEVICE *attach_virtual_disk(const char *path, UINT32 flags,
                                              const char *format,
                                              UINT64 data_size,
                                              UINT64 file_size)
{
    UINT32 index;
    ASAS_BLOCK_DEVICE description;
    if (path == 0 || path[0] == '\0') return 0;
    if (data_size == 0 || file_size == 0 ||
        (data_size % VDISK_SECTOR_SIZE) != 0) return 0;
    for (index = 0; index < VDISK_MAX_COUNT; index++) {
        if (vdisks[index].info.active &&
            strings_equal(vdisks[index].info.path, path)) return 0;
    }
    for (index = 0; index < VDISK_MAX_COUNT; index++)
        if (!vdisks[index].info.active) break;
    if (index == VDISK_MAX_COUNT) return 0;

    vdisks[index].info.active = 1;
    vdisks[index].info.read_only = (flags & VDISK_FLAG_READ_ONLY) != 0;
    vdisks[index].info.size = data_size;
    vdisks[index].info.file_size = file_size;
    copy_string(vdisks[index].info.format, format,
                sizeof(vdisks[index].info.format));
    copy_string(vdisks[index].info.path, path, sizeof(vdisks[index].info.path));
    vdisks[index].info.name[0] = 'v';
    vdisks[index].info.name[1] = 'd';
    vdisks[index].info.name[2] = 'i';
    vdisks[index].info.name[3] = 's';
    vdisks[index].info.name[4] = 'k';
    vdisks[index].info.name[5] = (char)('0' + index);
    vdisks[index].info.name[6] = '\0';

    description.id = 0;
    copy_string(description.name, vdisks[index].info.name,
                sizeof(description.name));
    description.logical_block_size = VDISK_SECTOR_SIZE;
    description.physical_block_size = VDISK_SECTOR_SIZE;
    description.block_count = data_size / VDISK_SECTOR_SIZE;
    description.flags = vdisks[index].info.read_only
        ? BLOCK_DEVICE_FLAG_READ_ONLY : 0;
    description.target = 0;
    description.lun = 0;
    description.start_lba = 0;
    description.parent = 0;
    description.ops = &raw_ops;
    description.driver_context = &vdisks[index];
    vdisks[index].info.device = block_device_register(&description);
    if (vdisks[index].info.device == 0) {
        vdisks[index].info.active = 0;
        return 0;
    }
    return vdisks[index].info.device;
}

ASAS_BLOCK_DEVICE *virtual_disk_attach_raw(const char *path, UINT32 flags)
{
    UINT64 size;
    if (path == 0 || path[0] == '\0') return 0;
    size = vfs_file_size(path);
    return attach_virtual_disk(path, flags, "raw", size, size);
}

static int validate_fixed_vhd_footer(const UINT8 *footer, UINT64 file_size,
                                     UINT64 *data_size)
{
    UINT32 index;
    UINT32 sum = 0;
    UINT32 stored;
    UINT64 current_size;
    if (footer == 0 || data_size == 0 || file_size <= VDISK_SECTOR_SIZE)
        return 0;
    if (footer[0] != 'c' || footer[1] != 'o' || footer[2] != 'n' ||
        footer[3] != 'e' || footer[4] != 'c' || footer[5] != 't' ||
        footer[6] != 'i' || footer[7] != 'x') return 0;
    if (read_be32(footer + 60U) != 2U) return 0;
    stored = read_be32(footer + 64U);
    for (index = 0; index < VDISK_SECTOR_SIZE; index++) {
        if (index >= 64U && index < 68U) continue;
        sum += footer[index];
    }
    if ((~sum) != stored) return 0;
    current_size = read_be64(footer + 48U);
    if (current_size == 0 || current_size + VDISK_SECTOR_SIZE != file_size ||
        (current_size % VDISK_SECTOR_SIZE) != 0) return 0;
    *data_size = current_size;
    return 1;
}

ASAS_BLOCK_DEVICE *virtual_disk_attach_fixed_vhd(const char *path, UINT32 flags)
{
    UINT64 file_size;
    UINT64 data_size = 0;
    UINT64 handle;
    UINT8 *image;
    UINT64 bytes;
    ASAS_BLOCK_DEVICE *device;
    if (path == 0 || path[0] == '\0') return 0;
    file_size = vfs_file_size(path);
    if (file_size <= VDISK_SECTOR_SIZE) return 0;
    image = (UINT8 *)kmalloc((UINTN)file_size);
    if (image == 0) return 0;
    handle = vfs_open(path);
    if (handle == 0) {
        kfree(image);
        return 0;
    }
    bytes = vfs_read(handle, image, file_size);
    (void)vfs_close(handle);
    if (bytes < file_size ||
        !validate_fixed_vhd_footer(image + file_size - VDISK_SECTOR_SIZE,
                                   file_size, &data_size)) {
        kfree(image);
        return 0;
    }
    kfree(image);
    device = attach_virtual_disk(path, flags, "vhd-fixed", data_size, file_size);
    return device;
}

static int validate_qcow2_image(const UINT8 *image, UINT64 size)
{
    UINT32 version;
    UINT32 cluster_bits;
    UINT64 backing_offset;
    UINT32 backing_size;
    UINT64 l1_size;
    UINT64 l1_offset;
    UINT64 refcount_offset;
    UINT32 refcount_clusters;
    if (image == 0 || size < 104U) return 0;
    if (image[0] != 'Q' || image[1] != 'F' || image[2] != 'I' ||
        image[3] != 0xFB) return 0;
    version = read_be32(image + 4U);
    if (version < 2U || version > 3U) return 0;
    backing_offset = read_be64(image + 8U);
    backing_size = read_be32(image + 16U);
    cluster_bits = read_be32(image + 20U);
    if (cluster_bits < 9U || cluster_bits > 21U) return 0;
    l1_size = read_be32(image + 36U);
    l1_offset = read_be64(image + 40U);
    refcount_offset = read_be64(image + 48U);
    refcount_clusters = read_be32(image + 56U);
    if (l1_size == 0 || l1_offset == 0 || refcount_offset == 0 ||
        refcount_clusters == 0) return 0;
    if (l1_offset >= size || refcount_offset >= size) return 0;
    if (backing_offset != 0 &&
        (backing_offset >= size || backing_size == 0 ||
         backing_offset + backing_size > size)) return 0;
    return 1;
}

static int validate_vhdx_image(const UINT8 *image, UINT64 size)
{
    UINT64 offset;
    int header_found = 0;
    int region_found = 0;
    if (image == 0 || size < 4U * 1024U * 1024U) return 0;
    if (image[0] != 'v' || image[1] != 'h' || image[2] != 'd' ||
        image[3] != 'x' || image[4] != 'f' || image[5] != 'i' ||
        image[6] != 'l' || image[7] != 'e') return 0;
    for (offset = 64U * 1024U; offset <= 128U * 1024U; offset += 64U * 1024U) {
        if (offset + 4U <= size && image[offset] == 'h' &&
            image[offset + 1U] == 'e' && image[offset + 2U] == 'a' &&
            image[offset + 3U] == 'd') header_found = 1;
    }
    for (offset = 192U * 1024U; offset <= 256U * 1024U; offset += 64U * 1024U) {
        if (offset + 4U <= size && image[offset] == 'r' &&
            image[offset + 1U] == 'e' && image[offset + 2U] == 'g' &&
            image[offset + 3U] == 'i') region_found = 1;
    }
    return header_found && region_found;
}

int virtual_disk_validate_image(const char *path, const char *format)
{
    UINT8 *image;
    UINT64 size;
    UINT64 data_size = 0;
    int ok = 0;
    if (path == 0 || format == 0) return 0;
    if (!image_read_all(path, &image, &size)) return 0;
    if (strings_equal(format, "raw")) {
        ok = size != 0 && (size % VDISK_SECTOR_SIZE) == 0;
    } else if (strings_equal(format, "vhd-fixed")) {
        ok = size > VDISK_SECTOR_SIZE &&
             validate_fixed_vhd_footer(image + size - VDISK_SECTOR_SIZE,
                                       size, &data_size);
    } else if (strings_equal(format, "qcow2")) {
        ok = validate_qcow2_image(image, size);
    } else if (strings_equal(format, "vhdx")) {
        ok = validate_vhdx_image(image, size);
    }
    kfree(image);
    return ok;
}

ASAS_BLOCK_DEVICE *virtual_disk_attach_auto(const char *path, UINT32 flags)
{
    UINT64 size;
    UINT8 *image;
    UINT64 image_size;
    UINT64 data_size = 0;
    ASAS_BLOCK_DEVICE *device = 0;
    if (path == 0 || path[0] == '\0') return 0;
    size = vfs_file_size(path);
    if (size == 0) return 0;
    if (!image_read_all(path, &image, &image_size)) return 0;
    if (image_size > VDISK_SECTOR_SIZE &&
        validate_fixed_vhd_footer(image + image_size - VDISK_SECTOR_SIZE,
                                  image_size, &data_size)) {
        kfree(image);
        return virtual_disk_attach_fixed_vhd(path, flags);
    }
    if (validate_qcow2_image(image, image_size) ||
        validate_vhdx_image(image, image_size)) {
        kfree(image);
        return 0;
    }
    if (has_suffix(path, ".qcow2") || has_suffix(path, ".vhdx")) {
        kfree(image);
        return 0;
    }
    kfree(image);
    device = virtual_disk_attach_raw(path, flags);
    return device;
}

int virtual_disk_detach(const char *name)
{
    UINT32 index;
    if (name == 0 || name[0] == '\0') return 0;
    for (index = 0; index < VDISK_MAX_COUNT; index++) {
        if (!vdisks[index].info.active) continue;
        if (strings_equal(vdisks[index].info.name, name)) {
            if (vdisks[index].info.device != 0 &&
                filesystem_device_is_mounted(vdisks[index].info.device))
                return 0;
            vdisks[index].info.active = 0;
            if (vdisks[index].info.device != 0)
                vdisks[index].info.device->flags |= BLOCK_DEVICE_FLAG_READ_ONLY;
            return 1;
        }
    }
    return 0;
}

int virtual_disk_check(const char *name)
{
    const ASAS_VDISK_INFO *info = virtual_disk_find(name);
    UINT64 size;
    UINT64 handle;
    UINT8 *image;
    UINT64 bytes;
    UINT64 data_size = 0;
    if (info == 0 || !info->active) return 0;
    size = vfs_file_size(info->path);
    if (size == 0 || size != info->file_size ||
        (info->size % VDISK_SECTOR_SIZE) != 0)
        return 0;
    if (strings_equal(info->format, "vhd-fixed")) {
        image = (UINT8 *)kmalloc((UINTN)size);
        if (image == 0) return 0;
        handle = vfs_open(info->path);
        if (handle == 0) {
            kfree(image);
            return 0;
        }
        bytes = vfs_read(handle, image, size);
        (void)vfs_close(handle);
        if (bytes < size ||
            !validate_fixed_vhd_footer(image + size - VDISK_SECTOR_SIZE,
                                       size, &data_size) ||
            data_size != info->size) {
            kfree(image);
            return 0;
        }
        kfree(image);
        return 1;
    }
    image = (UINT8 *)kmalloc(VDISK_SECTOR_SIZE);
    if (image == 0) return 0;
    handle = vfs_open(info->path);
    if (handle == 0) {
        kfree(image);
        return 0;
    }
    (void)vfs_read(handle, image, VDISK_SECTOR_SIZE);
    (void)vfs_close(handle);
    kfree(image);
    return 1;
}

UINT32 virtual_disk_count(void)
{
    UINT32 index;
    UINT32 count = 0;
    for (index = 0; index < VDISK_MAX_COUNT; index++)
        if (vdisks[index].info.active) count++;
    return count;
}

const ASAS_VDISK_INFO *virtual_disk_get(UINT32 index)
{
    UINT32 slot;
    UINT32 found = 0;
    for (slot = 0; slot < VDISK_MAX_COUNT; slot++) {
        if (!vdisks[slot].info.active) continue;
        if (found++ == index) return &vdisks[slot].info;
    }
    return 0;
}

const ASAS_VDISK_INFO *virtual_disk_find(const char *name)
{
    UINT32 index;
    if (name == 0) return 0;
    for (index = 0; index < VDISK_MAX_COUNT; index++) {
        if (!vdisks[index].info.active) continue;
        if (strings_equal(vdisks[index].info.name, name)) return &vdisks[index].info;
    }
    return 0;
}
