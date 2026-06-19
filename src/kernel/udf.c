#include "udf.h"
#include "heap.h"

#define UDF_SECTOR_SIZE 2048U
#define UDF_ANCHOR_SECTOR 256U
#define UDF_TAG_ANCHOR 2U
#define UDF_TAG_PARTITION 5U
#define UDF_TAG_LOGICAL_VOLUME 6U
#define UDF_TAG_TERMINATING 8U
#define UDF_TAG_FILE_SET 256U
#define UDF_TAG_FILE_IDENTIFIER 257U
#define UDF_TAG_FILE_ENTRY 261U
#define UDF_TAG_EXTENDED_FILE_ENTRY 266U
#define UDF_MAX_EXTENTS 32U
#define UDF_FILE_TYPE_DIRECTORY 4U
#define UDF_FILE_TYPE_FILE 5U

typedef struct {
    UINT32 block;
    UINT32 length;
    UINT16 partition;
} UDF_EXTENT;

typedef struct {
    UDF_EXTENT extents[UDF_MAX_EXTENTS];
    UINT32 extent_count;
    UINT64 size;
    UINT8 type;
    UINT8 embedded;
    UINT8 read_only;
    UINT8 embedded_data[UDF_SECTOR_SIZE];
    UINT32 embedded_length;
} UDF_NODE;

struct UDF_CONTEXT {
    ASAS_BLOCK_DEVICE *device;
    UINT32 logical_block_size;
    UINT16 partition_number;
    UINT32 partition_start;
    UINT32 partition_length;
    UDF_EXTENT file_set_extent;
    UDF_NODE root;
};

static UINT16 read_le16(const UINT8 *data)
{
    return (UINT16)data[0] | ((UINT16)data[1] << 8);
}

static UINT32 read_le32(const UINT8 *data)
{
    return (UINT32)data[0] | ((UINT32)data[1] << 8) |
           ((UINT32)data[2] << 16) | ((UINT32)data[3] << 24);
}

static UINT64 read_le64(const UINT8 *data)
{
    return (UINT64)read_le32(data) | ((UINT64)read_le32(data + 4U) << 32);
}

static char lower_char(char value)
{
    if (value >= 'A' && value <= 'Z') return (char)(value - 'A' + 'a');
    return value;
}

static int strings_equal_ci(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (lower_char(*left) != lower_char(*right)) return 0;
        left++;
        right++;
    }
    return *left == *right;
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

static int descriptor_tag_valid(const UINT8 *sector, UINT16 expected)
{
    UINT32 index;
    UINT8 checksum = 0;
    if (sector == 0 || read_le16(sector) != expected ||
        sector[5] != 2U) return 0;
    for (index = 0; index < 16U; index++) {
        if (index != 4U) checksum = (UINT8)(checksum + sector[index]);
    }
    return checksum == sector[4U];
}

static int read_udf_sector(UDF_CONTEXT *context, UINT64 sector, UINT8 *buffer)
{
    UINT8 raw[4096];
    UINT64 byte_offset;
    UINT64 block;
    UINT32 skip;
    if (context == 0 || context->device == 0 || buffer == 0 ||
        context->device->logical_block_size < 512U ||
        context->device->logical_block_size > sizeof(raw)) return 0;
    byte_offset = sector * (UINT64)UDF_SECTOR_SIZE;
    block = byte_offset / context->device->logical_block_size;
    skip = (UINT32)(byte_offset % context->device->logical_block_size);
    if (skip == 0 && UDF_SECTOR_SIZE % context->device->logical_block_size == 0)
        return block_device_read(context->device, block,
                                 UDF_SECTOR_SIZE /
                                     context->device->logical_block_size,
                                 buffer);
    if (!block_device_read(context->device, block, 1, raw)) return 0;
    if (skip + UDF_SECTOR_SIZE <= context->device->logical_block_size) {
        UINT32 index;
        for (index = 0; index < UDF_SECTOR_SIZE; index++)
            buffer[index] = raw[skip + index];
        return 1;
    }
    return 0;
}

static UINT64 udf_lba(UDF_CONTEXT *context, const UDF_EXTENT *extent)
{
    if (context == 0 || extent == 0 ||
        extent->partition != context->partition_number) return 0;
    return (UINT64)context->partition_start + extent->block;
}

static int read_extent_bytes(UDF_CONTEXT *context, const UDF_EXTENT *extent,
                             UINT64 offset, void *buffer, UINT64 capacity)
{
    UINT8 sector[UDF_SECTOR_SIZE];
    UINT8 *out = (UINT8 *)buffer;
    UINT64 done = 0;
    UINT64 base_lba;
    if (context == 0 || extent == 0 || buffer == 0 ||
        offset > extent->length) return 0;
    base_lba = udf_lba(context, extent);
    if (base_lba == 0 && extent->block != 0) return 0;
    while (done < capacity && offset + done < extent->length) {
        UINT64 absolute = offset + done;
        UINT32 in_sector = (UINT32)(absolute % UDF_SECTOR_SIZE);
        UINT64 sector_index = absolute / UDF_SECTOR_SIZE;
        UINT32 chunk = UDF_SECTOR_SIZE - in_sector;
        if ((UINT64)chunk > capacity - done)
            chunk = (UINT32)(capacity - done);
        if ((UINT64)chunk > extent->length - absolute)
            chunk = (UINT32)(extent->length - absolute);
        if (!read_udf_sector(context, base_lba + sector_index, sector))
            return 0;
        {
            UINT32 index;
            for (index = 0; index < chunk; index++)
                out[done + index] = sector[in_sector + index];
        }
        done += chunk;
    }
    return 1;
}

static int append_extent(UDF_NODE *node, UINT32 block, UINT32 length,
                         UINT16 partition)
{
    if (node == 0 || length == 0 || node->extent_count >= UDF_MAX_EXTENTS)
        return 0;
    node->extents[node->extent_count].block = block;
    node->extents[node->extent_count].length = length & 0x3FFFFFFFU;
    node->extents[node->extent_count].partition = partition;
    node->extent_count++;
    return 1;
}

static int decode_cs0(char *destination, const UINT8 *source, UINT32 length)
{
    UINT32 out = 0;
    UINT32 index;
    UINT8 compression;
    if (destination == 0 || source == 0 || length == 0) return 0;
    compression = source[0];
    if (compression == 8U) {
        for (index = 1U; index < length && out + 1U < 256U; index++) {
            UINT8 value = source[index];
            destination[out++] = value < 0x80U ? (char)value : '?';
        }
    } else if (compression == 16U) {
        for (index = 1U; index + 1U < length && out + 1U < 256U; index += 2U) {
            UINT16 value = ((UINT16)source[index] << 8) | source[index + 1U];
            if (value < 0x80U) {
                destination[out++] = (char)value;
            } else if (value < 0x800U && out + 2U < 256U) {
                destination[out++] = (char)(0xC0U | (value >> 6));
                destination[out++] = (char)(0x80U | (value & 0x3FU));
            } else if (out + 3U < 256U) {
                destination[out++] = (char)(0xE0U | (value >> 12));
                destination[out++] = (char)(0x80U | ((value >> 6) & 0x3FU));
                destination[out++] = (char)(0x80U | (value & 0x3FU));
            }
        }
    } else {
        return 0;
    }
    destination[out] = '\0';
    return out != 0;
}

static int parse_icb_tag(const UINT8 *data, UDF_NODE *node)
{
    UINT16 flags;
    if (data == 0 || node == 0) return 0;
    node->type = data[11U];
    flags = read_le16(data + 18U);
    return flags & 0x0007U;
}

static int parse_file_entry(UDF_CONTEXT *context, const UDF_EXTENT *icb,
                            UDF_NODE *node)
{
    UINT8 sector[UDF_SECTOR_SIZE];
    UINT16 tag;
    UINT32 ea_length;
    UINT32 ad_length;
    UINT32 ad_offset;
    UINT32 cursor;
    int allocation_type;
    if (context == 0 || icb == 0 || node == 0 ||
        !read_udf_sector(context, udf_lba(context, icb), sector)) return 0;
    tag = read_le16(sector);
    if (tag != UDF_TAG_FILE_ENTRY && tag != UDF_TAG_EXTENDED_FILE_ENTRY)
        return 0;
    if (!descriptor_tag_valid(sector, tag)) return 0;

    node->extent_count = 0;
    node->embedded = 0;
    node->embedded_length = 0;
    node->read_only = (read_le32(sector + 44U) & 0x00000002U) != 0;
    allocation_type = parse_icb_tag(sector + 16U, node);
    if (tag == UDF_TAG_EXTENDED_FILE_ENTRY) {
        node->size = read_le64(sector + 56U);
        ea_length = read_le32(sector + 208U);
        ad_length = read_le32(sector + 212U);
        ad_offset = 216U + ea_length;
    } else {
        node->size = read_le64(sector + 56U);
        ea_length = read_le32(sector + 168U);
        ad_length = read_le32(sector + 172U);
        ad_offset = 176U + ea_length;
    }
    if (ad_offset > UDF_SECTOR_SIZE || ad_length > UDF_SECTOR_SIZE - ad_offset)
        return 0;
    if (allocation_type == 0) {
        UINT32 index;
        node->embedded = 1;
        node->embedded_length = ad_length;
        if (ad_length > sizeof(node->embedded_data)) return 0;
        for (index = 0; index < ad_length; index++)
            node->embedded_data[index] = sector[ad_offset + index];
        return node->size <= ad_length;
    }
    cursor = ad_offset;
    while (cursor < ad_offset + ad_length) {
        UINT32 length;
        UINT32 block;
        UINT16 partition;
        if (allocation_type == 1) {
            if (cursor + 8U > ad_offset + ad_length) return 0;
            length = read_le32(sector + cursor) & 0x3FFFFFFFU;
            block = read_le32(sector + cursor + 4U);
            partition = context->partition_number;
            cursor += 8U;
        } else if (allocation_type == 2) {
            if (cursor + 16U > ad_offset + ad_length) return 0;
            length = read_le32(sector + cursor) & 0x3FFFFFFFU;
            block = read_le32(sector + cursor + 4U);
            partition = read_le16(sector + cursor + 8U);
            cursor += 16U;
        } else if (allocation_type == 3) {
            if (cursor + 20U > ad_offset + ad_length) return 0;
            length = read_le32(sector + cursor) & 0x3FFFFFFFU;
            block = read_le32(sector + cursor + 4U);
            partition = read_le16(sector + cursor + 8U);
            cursor += 20U;
        } else {
            return 0;
        }
        if (length != 0 && !append_extent(node, block, length, partition))
            return 0;
    }
    return 1;
}

static int read_node_bytes(UDF_CONTEXT *context, const UDF_NODE *node,
                           UINT64 offset, void *buffer, UINT64 capacity)
{
    UINT64 done = 0;
    UINT32 index;
    if (context == 0 || node == 0 || buffer == 0 || offset > node->size)
        return 0;
    if (capacity > node->size - offset) capacity = node->size - offset;
    if (node->embedded) {
        UINT8 *out = (UINT8 *)buffer;
        if (offset + capacity > node->embedded_length) return 0;
        for (index = 0; index < capacity; index++)
            out[index] = node->embedded_data[(UINT32)offset + index];
        return 1;
    }
    for (index = 0; index < node->extent_count && done < capacity; index++) {
        UINT64 extent_length = node->extents[index].length;
        if (offset >= extent_length) {
            offset -= extent_length;
            continue;
        }
        {
            UINT64 chunk = extent_length - offset;
            if (chunk > capacity - done) chunk = capacity - done;
            if (!read_extent_bytes(context, &node->extents[index], offset,
                                   (UINT8 *)buffer + done, chunk)) return 0;
            done += chunk;
            offset = 0;
        }
    }
    return done == capacity;
}

static int parse_file_identifier(UDF_CONTEXT *context, const UINT8 *record,
                                 UINT32 available, char *name,
                                 UDF_EXTENT *icb, UINT8 *is_parent)
{
    UINT32 implementation_use_length;
    UINT32 name_length;
    UINT32 name_offset;
    if (context == 0 || record == 0 || name == 0 || icb == 0 ||
        available < 38U || !descriptor_tag_valid(record, UDF_TAG_FILE_IDENTIFIER))
        return 0;
    name_length = record[19U];
    implementation_use_length = read_le16(record + 36U);
    name_offset = 38U + implementation_use_length;
    if (name_offset + name_length > available) return 0;
    icb->length = read_le32(record + 20U) & 0x3FFFFFFFU;
    icb->block = read_le32(record + 24U);
    icb->partition = read_le16(record + 28U);
    *is_parent = (record[18U] & 0x08U) != 0;
    if (*is_parent) {
        copy_string(name, "..", 256U);
        return 1;
    }
    return decode_cs0(name, record + name_offset, name_length);
}

static UINT32 file_identifier_length(const UINT8 *record, UINT32 available)
{
    UINT32 length;
    if (available < 38U || !descriptor_tag_valid(record, UDF_TAG_FILE_IDENTIFIER))
        return 0;
    length = 38U + read_le16(record + 36U) + record[19U];
    return (length + 3U) & ~3U;
}

static int find_in_directory(UDF_CONTEXT *context, const UDF_NODE *directory,
                             const char *name, UDF_NODE *result)
{
    UINT8 buffer[UDF_SECTOR_SIZE];
    UINT64 offset = 0;
    if (context == 0 || directory == 0 || name == 0 || result == 0 ||
        directory->type != UDF_FILE_TYPE_DIRECTORY) return 0;
    while (offset < directory->size) {
        UINT32 chunk = UDF_SECTOR_SIZE;
        UINT32 cursor = 0;
        if ((UINT64)chunk > directory->size - offset)
            chunk = (UINT32)(directory->size - offset);
        if (!read_node_bytes(context, directory, offset, buffer, chunk))
            return 0;
        while (cursor + 38U <= chunk) {
            char entry_name[256];
            UDF_EXTENT icb;
            UINT8 is_parent = 0;
            UINT32 record_length;
            if (!parse_file_identifier(context, buffer + cursor, chunk - cursor,
                                       entry_name, &icb, &is_parent))
                break;
            record_length = file_identifier_length(buffer + cursor,
                                                   chunk - cursor);
            if (record_length == 0) break;
            if (!is_parent && strings_equal_ci(entry_name, name))
                return parse_file_entry(context, &icb, result);
            cursor += record_length;
        }
        offset += chunk;
    }
    return 0;
}

static int next_path_component(const char **path, char *component)
{
    UINT32 length = 0;
    const char *cursor;
    if (path == 0 || *path == 0 || component == 0) return 0;
    cursor = *path;
    while (*cursor == '/') cursor++;
    if (*cursor == '\0') {
        *path = cursor;
        component[0] = '\0';
        return 0;
    }
    while (*cursor != '\0' && *cursor != '/') {
        if (length + 1U < 256U) component[length++] = *cursor;
        cursor++;
    }
    component[length] = '\0';
    *path = cursor;
    return length != 0;
}

static int find_node(UDF_CONTEXT *context, const char *path, UDF_NODE *node)
{
    UDF_NODE current;
    char component[256];
    const char *cursor = path;
    if (context == 0 || path == 0 || node == 0) return 0;
    current = context->root;
    while (next_path_component(&cursor, component)) {
        UDF_NODE next;
        if (!find_in_directory(context, &current, component, &next)) return 0;
        current = next;
    }
    *node = current;
    return 1;
}

static int find_anchor(UDF_CONTEXT *context, UDF_EXTENT *main_vds)
{
    UINT8 sector[UDF_SECTOR_SIZE];
    UINT64 candidates[3];
    UINT32 index;
    candidates[0] = UDF_ANCHOR_SECTOR;
    candidates[1] = context->device->block_count > 256U
        ? context->device->block_count - 256U : 0U;
    candidates[2] = context->device->block_count > 1U
        ? context->device->block_count - 1U : 0U;
    for (index = 0; index < 3U; index++) {
        if (candidates[index] == 0U) continue;
        if (!read_udf_sector(context, candidates[index], sector)) continue;
        if (!descriptor_tag_valid(sector, UDF_TAG_ANCHOR)) continue;
        main_vds->length = read_le32(sector + 16U);
        main_vds->block = read_le32(sector + 20U);
        main_vds->partition = 0;
        return main_vds->length != 0;
    }
    return 0;
}

static int read_volume_descriptors(UDF_CONTEXT *context,
                                   const UDF_EXTENT *main_vds)
{
    UINT8 sector[UDF_SECTOR_SIZE];
    UINT32 count;
    UINT32 index;
    int have_partition = 0;
    int have_lvd = 0;
    if (context == 0 || main_vds == 0) return 0;
    count = main_vds->length / UDF_SECTOR_SIZE;
    for (index = 0; index < count; index++) {
        UINT16 tag;
        if (!read_udf_sector(context, main_vds->block + index, sector))
            return 0;
        tag = read_le16(sector);
        if (tag == UDF_TAG_TERMINATING) break;
        if (!descriptor_tag_valid(sector, tag)) return 0;
        if (tag == UDF_TAG_PARTITION) {
            context->partition_number = read_le16(sector + 22U);
            context->partition_start = read_le32(sector + 188U);
            context->partition_length = read_le32(sector + 192U);
            have_partition = context->partition_length != 0;
        } else if (tag == UDF_TAG_LOGICAL_VOLUME) {
            UINT32 map_length = read_le32(sector + 264U);
            UINT32 maps = read_le32(sector + 268U);
            context->logical_block_size = read_le32(sector + 212U);
            context->file_set_extent.length = read_le32(sector + 248U) &
                0x3FFFFFFFU;
            context->file_set_extent.block = read_le32(sector + 252U);
            context->file_set_extent.partition = read_le16(sector + 256U);
            have_lvd = context->logical_block_size == UDF_SECTOR_SIZE &&
                       context->file_set_extent.length != 0 &&
                       map_length != 0 && maps != 0;
        }
    }
    return have_partition && have_lvd;
}

static int read_file_set(UDF_CONTEXT *context)
{
    UINT8 sector[UDF_SECTOR_SIZE];
    UDF_EXTENT root_icb;
    UINT64 lba;
    if (context == 0) return 0;
    lba = udf_lba(context, &context->file_set_extent);
    if (!read_udf_sector(context, lba, sector) ||
        !descriptor_tag_valid(sector, UDF_TAG_FILE_SET)) return 0;
    root_icb.length = read_le32(sector + 400U) & 0x3FFFFFFFU;
    root_icb.block = read_le32(sector + 404U);
    root_icb.partition = read_le16(sector + 408U);
    return root_icb.length != 0 &&
           parse_file_entry(context, &root_icb, &context->root) &&
           context->root.type == UDF_FILE_TYPE_DIRECTORY;
}

int udf_probe(ASAS_BLOCK_DEVICE *device)
{
    UDF_CONTEXT context;
    UDF_EXTENT main_vds;
    if (device == 0 || device->logical_block_size < 512U ||
        device->logical_block_size > 4096U) return 0;
    context.device = device;
    context.logical_block_size = UDF_SECTOR_SIZE;
    return find_anchor(&context, &main_vds);
}

UDF_CONTEXT *udf_context_create(ASAS_BLOCK_DEVICE *device)
{
    UDF_CONTEXT *context;
    UDF_EXTENT main_vds;
    if (!udf_probe(device)) return 0;
    context = (UDF_CONTEXT *)kmalloc(sizeof(UDF_CONTEXT));
    if (context == 0) return 0;
    context->device = device;
    context->logical_block_size = UDF_SECTOR_SIZE;
    context->partition_number = 0;
    context->partition_start = 0;
    context->partition_length = 0;
    context->file_set_extent.length = 0;
    if (!find_anchor(context, &main_vds) ||
        !read_volume_descriptors(context, &main_vds) ||
        !read_file_set(context)) {
        kfree(context);
        return 0;
    }
    return context;
}

void udf_context_destroy(UDF_CONTEXT *context)
{
    if (context != 0) kfree(context);
}

int udf_context_exists(UDF_CONTEXT *context, const char *path)
{
    UDF_NODE node;
    return find_node(context, path, &node);
}

int udf_context_is_directory(UDF_CONTEXT *context, const char *path)
{
    UDF_NODE node;
    return find_node(context, path, &node) &&
           node.type == UDF_FILE_TYPE_DIRECTORY;
}

UINT64 udf_context_file_size(UDF_CONTEXT *context, const char *path)
{
    UDF_NODE node;
    if (!find_node(context, path, &node) ||
        node.type == UDF_FILE_TYPE_DIRECTORY) return 0;
    return node.size;
}

UINT64 udf_context_read_file(UDF_CONTEXT *context, const char *path,
                             void *buffer, UINT64 capacity)
{
    UDF_NODE node;
    UINT64 bytes;
    if (buffer == 0 || !find_node(context, path, &node) ||
        node.type == UDF_FILE_TYPE_DIRECTORY) return 0;
    bytes = capacity < node.size ? capacity : node.size;
    if (bytes == 0) return 0;
    return read_node_bytes(context, &node, 0, buffer, bytes) ? bytes : 0;
}

UINT64 udf_context_list_directory(UDF_CONTEXT *context, const char *path,
                                  UDF_FILE_INFO *entries, UINT64 capacity)
{
    UDF_NODE directory;
    UINT8 buffer[UDF_SECTOR_SIZE];
    UINT64 offset = 0;
    UINT64 count = 0;
    if (entries == 0 || capacity == 0 ||
        !find_node(context, path, &directory) ||
        directory.type != UDF_FILE_TYPE_DIRECTORY) return 0;
    while (offset < directory.size && count < capacity) {
        UINT32 chunk = UDF_SECTOR_SIZE;
        UINT32 cursor = 0;
        if ((UINT64)chunk > directory.size - offset)
            chunk = (UINT32)(directory.size - offset);
        if (!read_node_bytes(context, &directory, offset, buffer, chunk))
            return count;
        while (cursor + 38U <= chunk && count < capacity) {
            char name[256];
            UDF_EXTENT icb;
            UDF_NODE child;
            UINT8 is_parent = 0;
            UINT32 record_length;
            if (!parse_file_identifier(context, buffer + cursor, chunk - cursor,
                                       name, &icb, &is_parent))
                break;
            record_length = file_identifier_length(buffer + cursor,
                                                   chunk - cursor);
            if (record_length == 0) break;
            if (!is_parent && parse_file_entry(context, &icb, &child)) {
                copy_string(entries[count].name, name,
                            sizeof(entries[count].name));
                entries[count].is_directory =
                    child.type == UDF_FILE_TYPE_DIRECTORY;
                entries[count].size = entries[count].is_directory ? 0 : child.size;
                entries[count].read_only = child.read_only;
                count++;
            }
            cursor += record_length;
        }
        offset += chunk;
    }
    return count;
}

int udf_context_write_file(UDF_CONTEXT *context, const char *path,
                           const void *buffer, UINT64 size)
{
    (void)context; (void)path; (void)buffer; (void)size;
    return 0;
}

int udf_context_delete_file(UDF_CONTEXT *context, const char *path)
{
    (void)context; (void)path;
    return 0;
}

int udf_context_create_directory(UDF_CONTEXT *context, const char *path)
{
    (void)context; (void)path;
    return 0;
}

int udf_context_delete_directory(UDF_CONTEXT *context, const char *path)
{
    (void)context; (void)path;
    return 0;
}

int udf_context_rename(UDF_CONTEXT *context, const char *source,
                       const char *destination)
{
    (void)context; (void)source; (void)destination;
    return 0;
}

int udf_context_sync(UDF_CONTEXT *context)
{
    if (context == 0 || context->device == 0) return 0;
    return block_device_flush(context->device);
}
