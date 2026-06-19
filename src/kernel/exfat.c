#include "exfat.h"
#include "heap.h"

#define EXFAT_MAX_SECTOR_SIZE 4096U
#define EXFAT_ENTRY_FILE 0x85U
#define EXFAT_ENTRY_STREAM 0xC0U
#define EXFAT_ENTRY_NAME 0xC1U
#define EXFAT_ENTRY_UPCASE 0x82U
#define EXFAT_ENTRY_BITMAP 0x81U
#define EXFAT_ATTR_DIRECTORY 0x10U
#define EXFAT_STREAM_NO_FAT_CHAIN 0x02U
#define EXFAT_EOC 0xFFFFFFF8U
#define EXFAT_BAD_CLUSTER 0xFFFFFFF7U
#define EXFAT_MAX_TRANSACTION_SECTORS 256U

typedef struct {
    UINT32 first_cluster;
    UINT64 valid_data_length;
    UINT64 data_length;
    UINT16 attributes;
    UINT8 no_fat_chain;
    UINT8 name_length;
    UINT16 name[255];
    UINT32 parent_first_cluster;
    UINT64 entry_offset;
    UINT32 entry_count;
    UINT8 parent_no_fat_chain;
    UINT64 parent_data_length;
} EXFAT_NODE;

typedef struct {
    UINT64 lba;
    UINT8 *before;
} EXFAT_JOURNAL_ENTRY;

struct EXFAT_CONTEXT {
    ASAS_BLOCK_DEVICE *device;
    UINT64 volume_length;
    UINT32 fat_offset;
    UINT32 fat_length;
    UINT32 cluster_heap_offset;
    UINT32 cluster_count;
    UINT32 root_cluster;
    UINT32 sector_size;
    UINT32 sectors_per_cluster;
    UINT32 cluster_size;
    UINT16 *upcase;
    UINT32 bitmap_first_cluster;
    UINT64 bitmap_length;
    EXFAT_JOURNAL_ENTRY journal[EXFAT_MAX_TRANSACTION_SECTORS];
    UINT32 journal_count;
    UINT8 transaction_active;
    UINT8 volume_dirty;
};

static UINT16 read_u16(const UINT8 *p)
{
    return (UINT16)p[0] | ((UINT16)p[1] << 8);
}

static UINT32 read_u32(const UINT8 *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static UINT64 read_u64(const UINT8 *p)
{
    return (UINT64)read_u32(p) | ((UINT64)read_u32(p + 4) << 32);
}

static void write_u16(UINT8 *p, UINT16 value)
{
    p[0] = (UINT8)value;
    p[1] = (UINT8)(value >> 8);
}

static void write_u32(UINT8 *p, UINT32 value)
{
    p[0] = (UINT8)value;
    p[1] = (UINT8)(value >> 8);
    p[2] = (UINT8)(value >> 16);
    p[3] = (UINT8)(value >> 24);
}

static void write_u64(UINT8 *p, UINT64 value)
{
    write_u32(p, (UINT32)value);
    write_u32(p + 4, (UINT32)(value >> 32));
}

static void clear_bytes(void *buffer, UINT64 size)
{
    UINT8 *bytes = (UINT8 *)buffer;
    while (size-- != 0) *bytes++ = 0;
}

static UINT32 boot_checksum_step(UINT32 checksum, UINT8 value)
{
    return ((checksum << 31) | (checksum >> 1)) + value;
}

static UINT32 stream_checksum_step(UINT32 checksum, UINT8 value)
{
    return ((checksum << 31) | (checksum >> 1)) + value;
}

static UINT16 entry_set_checksum(const UINT8 *entries, UINT32 count)
{
    UINT16 checksum = 0;
    UINT32 byte_count = count * 32U;
    UINT32 index;
    for (index = 0; index < byte_count; index++) {
        if (index == 2U || index == 3U) continue;
        checksum = (UINT16)(((checksum << 15) | (checksum >> 1)) + entries[index]);
    }
    return checksum;
}

static int valid_cluster(const EXFAT_CONTEXT *context, UINT32 cluster)
{
    return cluster >= 2U && cluster - 2U < context->cluster_count;
}

static UINT64 cluster_lba(const EXFAT_CONTEXT *context, UINT32 cluster)
{
    return (UINT64)context->cluster_heap_offset +
           (UINT64)(cluster - 2U) * context->sectors_per_cluster;
}

static int read_sector(EXFAT_CONTEXT *context, UINT64 lba, void *buffer)
{
    return lba < context->volume_length &&
           block_device_read(context->device, lba, 1, buffer);
}

static int journal_sector(EXFAT_CONTEXT *context, UINT64 lba)
{
    UINT32 index;
    UINT8 *before;
    for (index = 0; index < context->journal_count; index++)
        if (context->journal[index].lba == lba) return 1;
    if (!context->transaction_active ||
        context->journal_count >= EXFAT_MAX_TRANSACTION_SECTORS) return 0;
    before = (UINT8 *)kmalloc(context->sector_size);
    if (before == 0 || !read_sector(context, lba, before)) {
        if (before != 0) kfree(before);
        return 0;
    }
    context->journal[context->journal_count].lba = lba;
    context->journal[context->journal_count].before = before;
    context->journal_count++;
    return 1;
}

static int write_sector(EXFAT_CONTEXT *context, UINT64 lba, const void *buffer)
{
    return lba < context->volume_length && journal_sector(context, lba) &&
           block_device_write(context->device, lba, 1, buffer);
}

static int set_volume_dirty(EXFAT_CONTEXT *context, int dirty)
{
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    UINT32 copy;
    for (copy = 0; copy < 2U; copy++) {
        UINT64 lba = copy == 0 ? 0U : 12U;
        UINT16 flags;
        if (!read_sector(context, lba, sector)) return 0;
        flags = read_u16(sector + 106U);
        if (dirty) flags |= 0x0002U; else flags &= 0xFFFDU;
        write_u16(sector + 106U, flags);
        if (!write_sector(context, lba, sector)) return 0;
    }
    context->volume_dirty = dirty ? 1U : 0U;
    return block_device_flush(context->device);
}

static int transaction_rollback(EXFAT_CONTEXT *context);
static int bitmap_bit(EXFAT_CONTEXT *context, UINT32 cluster, int *allocated);

static int update_percent_in_use(EXFAT_CONTEXT *context)
{
    UINT64 allocated = 0;
    UINT32 cluster;
    UINT8 percent;
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    UINT32 copy;
    for (cluster = 2U; cluster - 2U < context->cluster_count; cluster++) {
        int used;
        if (!bitmap_bit(context, cluster, &used)) return 0;
        if (used) allocated++;
    }
    percent = (UINT8)((allocated * 100U + context->cluster_count - 1U) /
                      context->cluster_count);
    for (copy = 0; copy < 2U; copy++) {
        UINT64 lba = copy == 0 ? 0U : 12U;
        if (!read_sector(context, lba, sector)) return 0;
        sector[112] = percent;
        if (!write_sector(context, lba, sector)) return 0;
    }
    return 1;
}

static int transaction_begin(EXFAT_CONTEXT *context)
{
    if (context->transaction_active) return 0;
    context->transaction_active = 1;
    context->journal_count = 0;
    if (!set_volume_dirty(context, 1)) {
        (void)transaction_rollback(context);
        return 0;
    }
    return 1;
}

static void transaction_release(EXFAT_CONTEXT *context)
{
    while (context->journal_count != 0) {
        context->journal_count--;
        kfree(context->journal[context->journal_count].before);
        context->journal[context->journal_count].before = 0;
    }
    context->transaction_active = 0;
}

static int transaction_rollback(EXFAT_CONTEXT *context)
{
    int result = 1;
    while (context->journal_count != 0) {
        EXFAT_JOURNAL_ENTRY *entry = &context->journal[--context->journal_count];
        if (!block_device_write(context->device, entry->lba, 1, entry->before))
            result = 0;
        kfree(entry->before);
        entry->before = 0;
    }
    if (!block_device_flush(context->device)) result = 0;
    context->transaction_active = 0;
    context->volume_dirty = 0;
    return result;
}

static int transaction_commit(EXFAT_CONTEXT *context)
{
    if (!block_device_flush(context->device) || !update_percent_in_use(context) ||
        !set_volume_dirty(context, 0) ||
        !block_device_flush(context->device)) {
        (void)transaction_rollback(context);
        return 0;
    }
    transaction_release(context);
    return 1;
}

static int read_fat(EXFAT_CONTEXT *context, UINT32 cluster, UINT32 *next)
{
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    UINT64 byte_offset = (UINT64)cluster * 4U;
    UINT64 sector_index = byte_offset / context->sector_size;
    UINT32 offset = (UINT32)(byte_offset % context->sector_size);
    if (!valid_cluster(context, cluster) || sector_index >= context->fat_length ||
        !read_sector(context, context->fat_offset + sector_index, sector)) return 0;
    *next = read_u32(sector + offset);
    if (*next == EXFAT_BAD_CLUSTER || (*next < EXFAT_EOC && !valid_cluster(context, *next)))
        return 0;
    return 1;
}

static int write_fat(EXFAT_CONTEXT *context, UINT32 cluster, UINT32 value)
{
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    UINT64 byte_offset = (UINT64)cluster * 4U;
    UINT64 sector_index = byte_offset / context->sector_size;
    UINT32 offset = (UINT32)(byte_offset % context->sector_size);
    if (!valid_cluster(context, cluster) || sector_index >= context->fat_length ||
        !read_sector(context, context->fat_offset + sector_index, sector)) return 0;
    write_u32(sector + offset, value);
    return write_sector(context, context->fat_offset + sector_index, sector);
}

static int stream_cluster(EXFAT_CONTEXT *context, UINT32 first_cluster,
                          UINT8 no_fat_chain, UINT32 index, UINT32 *cluster)
{
    UINT32 current = first_cluster;
    UINT32 step;
    if (!valid_cluster(context, current) || index >= context->cluster_count) return 0;
    if (no_fat_chain) {
        if (current - 2U > context->cluster_count - 1U - index) return 0;
        *cluster = current + index;
        return 1;
    }
    for (step = 0; step < index; step++) {
        UINT32 next;
        if (!read_fat(context, current, &next) || next >= EXFAT_EOC) return 0;
        current = next;
    }
    *cluster = current;
    return 1;
}

static UINT64 read_stream(EXFAT_CONTEXT *context, UINT32 first_cluster,
                          UINT8 no_fat_chain, UINT64 data_length,
                          UINT64 offset, void *buffer, UINT64 capacity)
{
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    UINT8 *destination = (UINT8 *)buffer;
    UINT64 total = 0;
    if (offset >= data_length || capacity == 0) return 0;
    if (capacity > data_length - offset) capacity = data_length - offset;
    while (total < capacity) {
        UINT64 position = offset + total;
        UINT32 cluster_index = (UINT32)(position / context->cluster_size);
        UINT32 inside_cluster = (UINT32)(position % context->cluster_size);
        UINT32 cluster;
        UINT32 sector_in_cluster = inside_cluster / context->sector_size;
        UINT32 sector_offset = inside_cluster % context->sector_size;
        UINT32 copy = context->sector_size - sector_offset;
        if (copy > capacity - total) copy = (UINT32)(capacity - total);
        if (!stream_cluster(context, first_cluster, no_fat_chain,
                            cluster_index, &cluster) ||
            !read_sector(context, cluster_lba(context, cluster) +
                         sector_in_cluster, sector)) break;
        {
            UINT32 index;
            for (index = 0; index < copy; index++)
                destination[total + index] = sector[sector_offset + index];
        }
        total += copy;
    }
    return total;
}

static int stream_byte_location(EXFAT_CONTEXT *context, UINT32 first_cluster,
                                UINT8 no_fat_chain, UINT64 offset,
                                UINT64 *lba, UINT32 *inside_sector)
{
    UINT32 cluster_index = (UINT32)(offset / context->cluster_size);
    UINT32 inside_cluster = (UINT32)(offset % context->cluster_size);
    UINT32 cluster;
    if (!stream_cluster(context, first_cluster, no_fat_chain,
                        cluster_index, &cluster)) return 0;
    *lba = cluster_lba(context, cluster) + inside_cluster / context->sector_size;
    *inside_sector = inside_cluster % context->sector_size;
    return 1;
}

static int write_stream_metadata(EXFAT_CONTEXT *context, UINT32 first_cluster,
                                 UINT8 no_fat_chain, UINT64 offset,
                                 const void *buffer, UINT64 size)
{
    const UINT8 *source = (const UINT8 *)buffer;
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    UINT64 written = 0;
    while (written < size) {
        UINT64 lba;
        UINT32 inside;
        UINT32 copy;
        UINT32 index;
        if (!stream_byte_location(context, first_cluster, no_fat_chain,
                                  offset + written, &lba, &inside) ||
            !read_sector(context, lba, sector)) return 0;
        copy = context->sector_size - inside;
        if (copy > size - written) copy = (UINT32)(size - written);
        for (index = 0; index < copy; index++)
            sector[inside + index] = source[written + index];
        if (!write_sector(context, lba, sector)) return 0;
        written += copy;
    }
    return 1;
}

static int write_new_clusters(EXFAT_CONTEXT *context, const UINT32 *clusters,
                              UINT32 cluster_count, const void *buffer,
                              UINT64 size)
{
    const UINT8 *source = (const UINT8 *)buffer;
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    UINT64 written = 0;
    UINT32 cluster_index;
    for (cluster_index = 0; cluster_index < cluster_count; cluster_index++) {
        UINT32 sector_index;
        for (sector_index = 0; sector_index < context->sectors_per_cluster;
             sector_index++) {
            UINT32 copy = context->sector_size;
            UINT32 index;
            clear_bytes(sector, context->sector_size);
            if (written < size) {
                if (copy > size - written) copy = (UINT32)(size - written);
                for (index = 0; index < copy; index++)
                    sector[index] = source[written + index];
                written += copy;
            }
            if (!block_device_write(context->device,
                    cluster_lba(context, clusters[cluster_index]) + sector_index,
                    1, sector)) return 0;
        }
    }
    return block_device_flush(context->device);
}

static int bitmap_bit(EXFAT_CONTEXT *context, UINT32 cluster, int *allocated)
{
    UINT8 value;
    UINT64 bit = cluster - 2U;
    if (!valid_cluster(context, cluster) ||
        read_stream(context, context->bitmap_first_cluster, 0,
                    context->bitmap_length, bit >> 3, &value, 1) != 1) return 0;
    *allocated = (value & (1U << (bit & 7U))) != 0;
    return 1;
}

static int set_bitmap_bit(EXFAT_CONTEXT *context, UINT32 cluster, int allocated)
{
    UINT8 value;
    UINT64 bit = cluster - 2U;
    if (!valid_cluster(context, cluster) ||
        read_stream(context, context->bitmap_first_cluster, 0,
                    context->bitmap_length, bit >> 3, &value, 1) != 1) return 0;
    if (allocated) value |= (UINT8)(1U << (bit & 7U));
    else value &= (UINT8)~(1U << (bit & 7U));
    return write_stream_metadata(context, context->bitmap_first_cluster, 0,
                                 bit >> 3, &value, 1);
}

static int allocate_clusters(EXFAT_CONTEXT *context, UINT32 count,
                             UINT32 *clusters)
{
    UINT32 cluster;
    UINT32 found = 0;
    if (count == 0) return 1;
    for (cluster = 2U; cluster - 2U < context->cluster_count && found < count;
         cluster++) {
        int allocated;
        if (!bitmap_bit(context, cluster, &allocated)) return 0;
        if (!allocated) clusters[found++] = cluster;
    }
    if (found != count) return 0;
    for (cluster = 0; cluster < count; cluster++)
        if (!set_bitmap_bit(context, clusters[cluster], 1) ||
            !write_fat(context, clusters[cluster],
                       cluster + 1U < count ? clusters[cluster + 1U] : 0xFFFFFFFFU))
            return 0;
    return 1;
}

static int collect_clusters(EXFAT_CONTEXT *context, const EXFAT_NODE *node,
                            UINT32 **clusters_out, UINT32 *count_out)
{
    UINT32 count = node->data_length == 0 ? 1U :
        (UINT32)((node->data_length + context->cluster_size - 1U) /
                 context->cluster_size);
    UINT32 *clusters;
    UINT32 index;
    if (node->data_length == 0) {
        UINT32 current = node->first_cluster;
        count = 0;
        while (count < context->cluster_count && valid_cluster(context, current)) {
            UINT32 next;
            count++;
            if (!read_fat(context, current, &next) || next >= EXFAT_EOC) break;
            current = next;
        }
    }
    clusters = (UINT32 *)kmalloc((UINTN)count * sizeof(UINT32));
    if (clusters == 0) return 0;
    for (index = 0; index < count; index++)
        if (!stream_cluster(context, node->first_cluster, node->no_fat_chain,
                            index, &clusters[index])) {
            kfree(clusters);
            return 0;
        }
    *clusters_out = clusters;
    *count_out = count;
    return 1;
}

static int free_clusters(EXFAT_CONTEXT *context, const UINT32 *clusters,
                         UINT32 count)
{
    UINT32 index;
    for (index = 0; index < count; index++)
        if (!set_bitmap_bit(context, clusters[index], 0) ||
            !write_fat(context, clusters[index], 0)) return 0;
    return 1;
}

static UINT16 upcase_character(const EXFAT_CONTEXT *context, UINT16 character)
{
    if (context->upcase != 0) return context->upcase[character];
    if (character >= 'a' && character <= 'z') return (UINT16)(character - 32U);
    return character;
}

static int utf8_component(const char **path, UINT16 *name, UINT32 *length)
{
    const UINT8 *source = (const UINT8 *)*path;
    UINT32 count = 0;
    while (*source == '/') source++;
    while (*source != 0 && *source != '/') {
        UINT32 code;
        if (*source < 0x80U) code = *source++;
        else if ((*source & 0xE0U) == 0xC0U && (source[1] & 0xC0U) == 0x80U) {
            code = ((UINT32)(source[0] & 0x1FU) << 6) | (source[1] & 0x3FU);
            if (code < 0x80U) return 0;
            source += 2;
        } else if ((*source & 0xF0U) == 0xE0U &&
                   (source[1] & 0xC0U) == 0x80U &&
                   (source[2] & 0xC0U) == 0x80U) {
            code = ((UINT32)(source[0] & 0x0FU) << 12) |
                   ((UINT32)(source[1] & 0x3FU) << 6) | (source[2] & 0x3FU);
            if (code < 0x800U || (code >= 0xD800U && code <= 0xDFFFU)) return 0;
            source += 3;
        } else return 0;
        if (code > 0xFFFFU || count >= 255U) return 0;
        name[count++] = (UINT16)code;
    }
    *path = (const char *)source;
    *length = count;
    return count != 0;
}

static int names_equal(EXFAT_CONTEXT *context, const UINT16 *left,
                       UINT32 left_length, const UINT16 *right,
                       UINT32 right_length)
{
    UINT32 index;
    if (left_length != right_length) return 0;
    for (index = 0; index < left_length; index++)
        if (upcase_character(context, left[index]) !=
            upcase_character(context, right[index])) return 0;
    return 1;
}

static UINT32 utf16_to_utf8(const UINT16 *source, UINT32 length,
                            char *destination, UINT32 capacity)
{
    UINT32 input = 0;
    UINT32 output = 0;
    while (input < length) {
        UINT32 code = source[input++];
        if (code >= 0xD800U && code <= 0xDBFFU && input < length &&
            source[input] >= 0xDC00U && source[input] <= 0xDFFFU) {
            code = 0x10000U + ((code - 0xD800U) << 10) +
                   (source[input++] - 0xDC00U);
        } else if (code >= 0xD800U && code <= 0xDFFFU) return 0;
        if (code < 0x80U) {
            if (output + 1U >= capacity) return 0;
            destination[output++] = (char)code;
        } else if (code < 0x800U) {
            if (output + 2U >= capacity) return 0;
            destination[output++] = (char)(0xC0U | (code >> 6));
            destination[output++] = (char)(0x80U | (code & 0x3FU));
        } else if (code < 0x10000U) {
            if (output + 3U >= capacity) return 0;
            destination[output++] = (char)(0xE0U | (code >> 12));
            destination[output++] = (char)(0x80U | ((code >> 6) & 0x3FU));
            destination[output++] = (char)(0x80U | (code & 0x3FU));
        } else {
            if (output + 4U >= capacity) return 0;
            destination[output++] = (char)(0xF0U | (code >> 18));
            destination[output++] = (char)(0x80U | ((code >> 12) & 0x3FU));
            destination[output++] = (char)(0x80U | ((code >> 6) & 0x3FU));
            destination[output++] = (char)(0x80U | (code & 0x3FU));
        }
    }
    destination[output] = '\0';
    return output;
}

typedef int (*EXFAT_VISITOR)(EXFAT_CONTEXT *, const EXFAT_NODE *, void *);

static int visit_directory(EXFAT_CONTEXT *context, const EXFAT_NODE *directory,
                           EXFAT_VISITOR visitor, void *opaque)
{
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    UINT8 set[32U * 19U];
    UINT32 set_expected = 0;
    UINT32 set_stored = 0;
    UINT32 cluster_index = 0;
    UINT64 processed = 0;
    UINT64 limit = directory->data_length;
    int root = directory->data_length == 0;
    while (cluster_index < context->cluster_count) {
        UINT32 cluster;
        UINT32 sector_index;
        if (!stream_cluster(context, directory->first_cluster,
                            directory->no_fat_chain, cluster_index, &cluster)) break;
        for (sector_index = 0; sector_index < context->sectors_per_cluster;
             sector_index++) {
            UINT32 offset;
            if (!read_sector(context, cluster_lba(context, cluster) + sector_index,
                             sector)) return 0;
            for (offset = 0; offset < context->sector_size; offset += 32U) {
                UINT8 type = sector[offset];
                if (!root && processed >= limit) return 1;
                processed += 32U;
                if (type == 0) return 1;
                if (set_expected == 0) {
                    if (type != EXFAT_ENTRY_FILE) continue;
                    set_expected = (UINT32)sector[offset + 1U] + 1U;
                    if (set_expected < 3U || set_expected > 19U) {
                        set_expected = 0;
                        continue;
                    }
                    {
                        UINT32 index;
                        for (index = 0; index < 32U; index++) set[index] = sector[offset + index];
                    }
                    set_stored = 1;
                    continue;
                }
                {
                    UINT32 index;
                    for (index = 0; index < 32U; index++)
                        set[set_stored * 32U + index] = sector[offset + index];
                    set_stored++;
                    if (set_stored == set_expected) {
                        EXFAT_NODE node;
                        UINT32 entry_index;
                        UINT32 name_offset = 0;
                        UINT8 *stream = set + 32U;
                        int valid = stream[0] == EXFAT_ENTRY_STREAM &&
                            entry_set_checksum(set, set_expected) == read_u16(set + 2U);
                        clear_bytes(&node, sizeof(node));
                        if (valid) {
                            node.attributes = read_u16(set + 4U);
                            node.no_fat_chain = stream[1] & EXFAT_STREAM_NO_FAT_CHAIN;
                            node.name_length = stream[3];
                            node.valid_data_length = read_u64(stream + 8U);
                            node.first_cluster = read_u32(stream + 20U);
                            node.data_length = read_u64(stream + 24U);
                            node.parent_first_cluster = directory->first_cluster;
                            node.parent_no_fat_chain = directory->no_fat_chain;
                            node.parent_data_length = directory->data_length;
                            node.entry_offset = processed - (UINT64)set_expected * 32U;
                            node.entry_count = set_expected;
                            if (node.name_length == 0 || node.name_length > 255U ||
                                node.valid_data_length > node.data_length ||
                                (node.data_length != 0 && !valid_cluster(context, node.first_cluster)))
                                valid = 0;
                        }
                        for (entry_index = 2U; valid && entry_index < set_expected;
                             entry_index++) {
                            UINT8 *name_entry = set + entry_index * 32U;
                            UINT32 character;
                            if (name_entry[0] != EXFAT_ENTRY_NAME) continue;
                            for (character = 0; character < 15U &&
                                 name_offset < node.name_length; character++)
                                node.name[name_offset++] =
                                    read_u16(name_entry + 2U + character * 2U);
                        }
                        if (valid && name_offset == node.name_length &&
                            !visitor(context, &node, opaque)) return 1;
                        set_expected = 0;
                        set_stored = 0;
                    }
                }
            }
        }
        cluster_index++;
        if (!directory->no_fat_chain) {
            UINT32 next;
            if (!read_fat(context, cluster, &next) || next >= EXFAT_EOC) return 1;
        } else if (root) return 0;
    }
    return root ? 0 : processed >= limit;
}

typedef struct { const UINT16 *name; UINT32 length; EXFAT_NODE *result; int found; } FIND_STATE;

static int find_visitor(EXFAT_CONTEXT *context, const EXFAT_NODE *node, void *opaque)
{
    FIND_STATE *state = (FIND_STATE *)opaque;
    if (names_equal(context, state->name, state->length,
                    node->name, node->name_length)) {
        *state->result = *node;
        state->found = 1;
        return 0;
    }
    return 1;
}

static int resolve_path(EXFAT_CONTEXT *context, const char *path, EXFAT_NODE *node)
{
    EXFAT_NODE current;
    clear_bytes(&current, sizeof(current));
    current.first_cluster = context->root_cluster;
    current.attributes = EXFAT_ATTR_DIRECTORY;
    while (*path == '/') path++;
    if (*path == 0) { *node = current; return 1; }
    while (*path != 0) {
        UINT16 component[255];
        UINT32 length;
        FIND_STATE state;
        if ((current.attributes & EXFAT_ATTR_DIRECTORY) == 0 ||
            !utf8_component(&path, component, &length)) return 0;
        state.name = component;
        state.length = length;
        state.result = &current;
        state.found = 0;
        if (!visit_directory(context, &current, find_visitor, &state) ||
            !state.found) return 0;
        while (*path == '/') path++;
    }
    *node = current;
    return 1;
}

static UINT16 name_hash(EXFAT_CONTEXT *context, const UINT16 *name, UINT32 length)
{
    UINT16 hash = 0;
    UINT32 index;
    for (index = 0; index < length; index++) {
        UINT16 value = upcase_character(context, name[index]);
        hash = (UINT16)(((hash << 15) | (hash >> 1)) + (UINT8)value);
        hash = (UINT16)(((hash << 15) | (hash >> 1)) + (UINT8)(value >> 8));
    }
    return hash;
}

static UINT64 directory_length(EXFAT_CONTEXT *context, const EXFAT_NODE *directory)
{
    UINT32 cluster = directory->first_cluster;
    UINT32 count = 0;
    if (directory->data_length != 0) return directory->data_length;
    while (count < context->cluster_count && valid_cluster(context, cluster)) {
        UINT32 next;
        count++;
        if (!read_fat(context, cluster, &next) || next >= EXFAT_EOC) break;
        cluster = next;
    }
    return (UINT64)count * context->cluster_size;
}

static int read_directory_bytes(EXFAT_CONTEXT *context,
                                const EXFAT_NODE *directory, UINT64 offset,
                                void *buffer, UINT64 size)
{
    return read_stream(context, directory->first_cluster,
                       directory->no_fat_chain,
                       directory_length(context, directory), offset,
                       buffer, size) == size;
}

static int write_directory_bytes(EXFAT_CONTEXT *context,
                                 const EXFAT_NODE *directory, UINT64 offset,
                                 const void *buffer, UINT64 size)
{
    return offset + size <= directory_length(context, directory) &&
           write_stream_metadata(context, directory->first_cluster,
                                 directory->no_fat_chain, offset,
                                 buffer, size);
}

static int read_node_set(EXFAT_CONTEXT *context, const EXFAT_NODE *node,
                         UINT8 *set)
{
    EXFAT_NODE parent;
    clear_bytes(&parent, sizeof(parent));
    parent.first_cluster = node->parent_first_cluster;
    parent.no_fat_chain = node->parent_no_fat_chain;
    parent.data_length = node->parent_data_length;
    return node->entry_count >= 3U && node->entry_count <= 19U &&
           read_directory_bytes(context, &parent, node->entry_offset,
                                set, node->entry_count * 32U);
}

static int write_node_set(EXFAT_CONTEXT *context, const EXFAT_NODE *node,
                          const UINT8 *set)
{
    EXFAT_NODE parent;
    clear_bytes(&parent, sizeof(parent));
    parent.first_cluster = node->parent_first_cluster;
    parent.no_fat_chain = node->parent_no_fat_chain;
    parent.data_length = node->parent_data_length;
    return write_directory_bytes(context, &parent, node->entry_offset,
                                 set, node->entry_count * 32U);
}

static int update_node_stream(EXFAT_CONTEXT *context, const EXFAT_NODE *node,
                              UINT32 first_cluster, UINT64 data_length,
                              UINT64 valid_length, UINT8 no_fat_chain)
{
    UINT8 set[32U * 19U];
    UINT8 *stream;
    UINT16 checksum;
    if (!read_node_set(context, node, set)) return 0;
    stream = set + 32U;
    if (stream[0] != EXFAT_ENTRY_STREAM) return 0;
    stream[1] = no_fat_chain ? EXFAT_STREAM_NO_FAT_CHAIN : 0;
    write_u64(stream + 8U, valid_length);
    write_u32(stream + 20U, first_cluster);
    write_u64(stream + 24U, data_length);
    checksum = entry_set_checksum(set, node->entry_count);
    write_u16(set + 2U, checksum);
    return write_node_set(context, node, set);
}

static int split_parent_path(const char *path, char *parent, UINT32 capacity,
                             UINT16 *leaf, UINT32 *leaf_length)
{
    UINT32 length = 0;
    UINT32 slash = 0;
    UINT32 index;
    const char *component;
    while (path[length] != '\0') {
        if (path[length] == '/') slash = length;
        length++;
    }
    if (length == 0 || length >= 512U || slash + 1U >= length) return 0;
    if (slash == 0) {
        if (capacity < 2U) return 0;
        parent[0] = '/'; parent[1] = '\0';
    } else {
        if (slash + 1U > capacity) return 0;
        for (index = 0; index < slash; index++) parent[index] = path[index];
        parent[slash] = '\0';
    }
    component = path + slash + 1U;
    if (!utf8_component(&component, leaf, leaf_length) || *component != '\0') return 0;
    if ((*leaf_length == 1U && leaf[0] == '.') ||
        (*leaf_length == 2U && leaf[0] == '.' && leaf[1] == '.')) return 0;
    for (index = 0; index < *leaf_length; index++) {
        UINT16 character = leaf[index];
        if (character < 0x20U || character == '"' || character == '*' ||
            character == '/' || character == ':' || character == '<' ||
            character == '>' || character == '?' || character == '\\' ||
            character == '|') return 0;
    }
    return 1;
}

static UINT32 build_entry_set(EXFAT_CONTEXT *context, UINT8 *set,
                              const UINT16 *name, UINT32 name_length,
                              UINT16 attributes, UINT32 first_cluster,
                              UINT64 data_length, UINT64 valid_length,
                              UINT8 no_fat_chain)
{
    UINT32 name_entries = (name_length + 14U) / 15U;
    UINT32 count = 2U + name_entries;
    UINT32 index;
    UINT16 checksum;
    if (name_length == 0 || name_length > 255U || count > 19U) return 0;
    clear_bytes(set, count * 32U);
    set[0] = EXFAT_ENTRY_FILE;
    set[1] = (UINT8)(count - 1U);
    write_u16(set + 4U, attributes);
    set[32U] = EXFAT_ENTRY_STREAM;
    set[33U] = no_fat_chain ? EXFAT_STREAM_NO_FAT_CHAIN : 0;
    set[35U] = (UINT8)name_length;
    write_u16(set + 36U, name_hash(context, name, name_length));
    write_u64(set + 40U, valid_length);
    write_u32(set + 52U, first_cluster);
    write_u64(set + 56U, data_length);
    for (index = 0; index < name_entries; index++) {
        UINT8 *entry = set + (2U + index) * 32U;
        UINT32 character;
        entry[0] = EXFAT_ENTRY_NAME;
        for (character = 0; character < 15U; character++) {
            UINT32 source = index * 15U + character;
            write_u16(entry + 2U + character * 2U,
                      source < name_length ? name[source] : 0);
        }
    }
    checksum = entry_set_checksum(set, count);
    write_u16(set + 2U, checksum);
    return count;
}

static int extend_directory(EXFAT_CONTEXT *context, EXFAT_NODE *directory)
{
    UINT32 *old_clusters = 0;
    UINT32 old_count = 0;
    UINT32 new_cluster;
    UINT8 zero[EXFAT_MAX_SECTOR_SIZE];
    UINT32 sector;
    if (!collect_clusters(context, directory, &old_clusters, &old_count) ||
        old_count == 0 || !allocate_clusters(context, 1, &new_cluster)) {
        if (old_clusters != 0) kfree(old_clusters);
        return 0;
    }
    clear_bytes(zero, context->sector_size);
    for (sector = 0; sector < context->sectors_per_cluster; sector++)
        if (!block_device_write(context->device,
                cluster_lba(context, new_cluster) + sector, 1, zero)) {
            kfree(old_clusters);
            return 0;
        }
    for (sector = 0; sector + 1U < old_count; sector++)
        if (!write_fat(context, old_clusters[sector], old_clusters[sector + 1U])) {
            kfree(old_clusters); return 0;
        }
    if (!write_fat(context, old_clusters[old_count - 1U], new_cluster) ||
        !write_fat(context, new_cluster, 0xFFFFFFFFU)) {
        kfree(old_clusters); return 0;
    }
    directory->no_fat_chain = 0;
    directory->data_length = (UINT64)(old_count + 1U) * context->cluster_size;
    if (directory->entry_count != 0 &&
        !update_node_stream(context, directory, directory->first_cluster,
                            directory->data_length, directory->data_length, 0)) {
        kfree(old_clusters); return 0;
    }
    kfree(old_clusters);
    return 1;
}

static int find_free_entries(EXFAT_CONTEXT *context, EXFAT_NODE *directory,
                             UINT32 needed, UINT64 *offset_out)
{
    UINT64 length;
    UINT64 offset;
    UINT32 consecutive = 0;
    UINT64 start = 0;
    UINT8 entry[32];
    for (;;) {
        length = directory_length(context, directory);
        for (offset = 0; offset + 32U <= length; offset += 32U) {
            if (!read_directory_bytes(context, directory, offset, entry, 32U)) return 0;
            if (entry[0] == 0 || (entry[0] & 0x80U) == 0) {
                if (consecutive == 0) start = offset;
                consecutive++;
                if (consecutive == needed) { *offset_out = start; return 1; }
            } else consecutive = 0;
        }
        if (!extend_directory(context, directory)) return 0;
    }
}

static int publish_entry(EXFAT_CONTEXT *context, EXFAT_NODE *parent,
                         const UINT16 *name, UINT32 name_length,
                         UINT16 attributes, UINT32 first_cluster,
                         UINT64 data_length, UINT64 valid_length,
                         UINT8 no_fat_chain, EXFAT_NODE *published)
{
    UINT8 set[32U * 19U];
    UINT32 count = build_entry_set(context, set, name, name_length, attributes,
                                   first_cluster, data_length, valid_length,
                                   no_fat_chain);
    UINT64 offset;
    if (count == 0 || !find_free_entries(context, parent, count, &offset) ||
        !write_directory_bytes(context, parent, offset, set, count * 32U)) return 0;
    if (published != 0) {
        clear_bytes(published, sizeof(*published));
        published->first_cluster = first_cluster;
        published->data_length = data_length;
        published->valid_data_length = valid_length;
        published->attributes = attributes;
        published->no_fat_chain = no_fat_chain;
        published->parent_first_cluster = parent->first_cluster;
        published->parent_no_fat_chain = parent->no_fat_chain;
        published->parent_data_length = parent->data_length;
        published->entry_offset = offset;
        published->entry_count = count;
    }
    return 1;
}

static int retire_entry(EXFAT_CONTEXT *context, const EXFAT_NODE *node)
{
    UINT8 set[32U * 19U];
    UINT32 index;
    if (!read_node_set(context, node, set)) return 0;
    for (index = 0; index < node->entry_count; index++)
        set[index * 32U] &= 0x7FU;
    return write_node_set(context, node, set);
}

static int validate_boot_region(ASAS_BLOCK_DEVICE *device, UINT64 base,
                                UINT8 *boot)
{
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    UINT32 checksum = 0;
    UINT32 sector_index;
    UINT32 byte_index;
    UINT32 size = device->logical_block_size;
    if (!block_device_read(device, base, 1, boot) ||
        boot[3] != 'E' || boot[4] != 'X' || boot[5] != 'F' ||
        boot[6] != 'A' || boot[7] != 'T' || boot[8] != ' ' ||
        boot[9] != ' ' || boot[10] != ' ' ||
        boot[510] != 0x55U || boot[511] != 0xAAU) return 0;
    for (sector_index = 0; sector_index < 11U; sector_index++) {
        const UINT8 *bytes = sector;
        if (sector_index == 0) bytes = boot;
        else if (!block_device_read(device, base + sector_index, 1, sector)) return 0;
        for (byte_index = 0; byte_index < size; byte_index++) {
            if (sector_index == 0 &&
                (byte_index == 106U || byte_index == 107U || byte_index == 112U))
                continue;
            checksum = boot_checksum_step(checksum, bytes[byte_index]);
        }
    }
    if (!block_device_read(device, base + 11U, 1, sector)) return 0;
    for (byte_index = 0; byte_index + 4U <= size; byte_index += 4U)
        if (read_u32(sector + byte_index) != checksum) return 0;
    return 1;
}

int exfat_probe(ASAS_BLOCK_DEVICE *device)
{
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    if (device == 0 || device->logical_block_size < 512U ||
        device->logical_block_size > EXFAT_MAX_SECTOR_SIZE ||
        !block_device_read(device, 0, 1, sector)) return 0;
    return sector[3] == 'E' && sector[4] == 'X' && sector[5] == 'F' &&
           sector[6] == 'A' && sector[7] == 'T' &&
           sector[8] == ' ' && sector[9] == ' ' && sector[10] == ' ';
}

static int load_upcase(EXFAT_CONTEXT *context)
{
    EXFAT_NODE root;
    UINT8 sector[EXFAT_MAX_SECTOR_SIZE];
    UINT32 cluster = context->root_cluster;
    UINT32 seen = 0;
    int found_upcase = 0;
    clear_bytes(&root, sizeof(root));
    while (seen++ < context->cluster_count) {
        UINT32 sector_index;
        for (sector_index = 0; sector_index < context->sectors_per_cluster;
             sector_index++) {
            UINT32 offset;
            if (!read_sector(context, cluster_lba(context, cluster) + sector_index,
                             sector)) return 0;
            for (offset = 0; offset < context->sector_size; offset += 32U) {
                if (sector[offset] == 0)
                    return found_upcase && context->bitmap_first_cluster != 0;
                if (sector[offset] == EXFAT_ENTRY_BITMAP) {
                    UINT32 first = read_u32(sector + offset + 20U);
                    UINT64 length = read_u64(sector + offset + 24U);
                    if ((sector[offset + 1U] & 1U) != 0 ||
                        length < (context->cluster_count + 7U) / 8U ||
                        !valid_cluster(context, first)) return 0;
                    context->bitmap_first_cluster = first;
                    context->bitmap_length = length;
                }
                if (sector[offset] == EXFAT_ENTRY_UPCASE) {
                    UINT32 expected = read_u32(sector + offset + 4U);
                    UINT32 first = read_u32(sector + offset + 20U);
                    UINT64 length = read_u64(sector + offset + 24U);
                    UINT8 *raw;
                    UINT64 position;
                    UINT32 checksum = 0;
                    UINT32 code = 0;
                    if (length == 0 || length > 131072U || !valid_cluster(context, first))
                        return 0;
                    raw = (UINT8 *)kmalloc((UINTN)length);
                    context->upcase = (UINT16 *)kmalloc(65536U * sizeof(UINT16));
                    if (raw == 0 || context->upcase == 0) {
                        if (raw != 0) kfree(raw);
                        if (context->upcase != 0) { kfree(context->upcase); context->upcase = 0; }
                        return 0;
                    }
                    if (read_stream(context, first, 0, length, 0, raw, length) != length) {
                        kfree(raw); kfree(context->upcase); context->upcase = 0; return 0;
                    }
                    for (position = 0; position < length; position++)
                        checksum = stream_checksum_step(checksum, raw[position]);
                    if (checksum != expected) {
                        kfree(raw); kfree(context->upcase); context->upcase = 0; return 0;
                    }
                    for (position = 0; position + 1U < length && code < 65536U;
                         position += 2U) {
                        UINT16 value = read_u16(raw + position);
                        if (value == 0xFFFFU && position + 3U < length) {
                            UINT32 skip = read_u16(raw + position + 2U);
                            position += 2U;
                            while (skip-- != 0 && code < 65536U)
                                context->upcase[code] = (UINT16)code, code++;
                        } else context->upcase[code++] = value;
                    }
                    kfree(raw);
                    if (code != 65536U) { kfree(context->upcase); context->upcase = 0; return 0; }
                    found_upcase = 1;
                }
            }
        }
        {
            UINT32 next;
            if (!read_fat(context, cluster, &next) || next >= EXFAT_EOC)
                return found_upcase && context->bitmap_first_cluster != 0;
            cluster = next;
        }
    }
    return 0;
}

EXFAT_CONTEXT *exfat_context_create(ASAS_BLOCK_DEVICE *device)
{
    UINT8 boot[EXFAT_MAX_SECTOR_SIZE];
    EXFAT_CONTEXT *context;
    UINT32 sector_size;
    UINT32 sectors_per_cluster;
    if (!exfat_probe(device)) return 0;
    if (!validate_boot_region(device, 0, boot) &&
        !validate_boot_region(device, 12, boot)) return 0;
    if (boot[108] < 9U || boot[108] > 12U || boot[109] > 25U ||
        boot[110] == 0 || boot[110] > 2U) return 0;
    sector_size = 1U << boot[108];
    sectors_per_cluster = 1U << boot[109];
    if (sector_size != device->logical_block_size ||
        sectors_per_cluster > 0xFFFFFFFFU / sector_size) return 0;
    context = (EXFAT_CONTEXT *)kmalloc(sizeof(*context));
    if (context == 0) return 0;
    clear_bytes(context, sizeof(*context));
    context->device = device;
    context->volume_length = read_u64(boot + 72U);
    context->fat_offset = read_u32(boot + 80U);
    context->fat_length = read_u32(boot + 84U);
    if (boot[110] == 2U && (read_u16(boot + 106U) & 1U) != 0)
        context->fat_offset += context->fat_length;
    context->cluster_heap_offset = read_u32(boot + 88U);
    context->cluster_count = read_u32(boot + 92U);
    context->root_cluster = read_u32(boot + 96U);
    context->sector_size = sector_size;
    context->sectors_per_cluster = sectors_per_cluster;
    context->cluster_size = sector_size * sectors_per_cluster;
    if (context->volume_length < 24U || context->fat_offset < 24U ||
        context->fat_length == 0 || context->cluster_count == 0 ||
        !valid_cluster(context, context->root_cluster) ||
        context->fat_offset + context->fat_length > context->volume_length ||
        context->cluster_heap_offset > context->volume_length ||
        (UINT64)context->cluster_count * sectors_per_cluster >
            context->volume_length - context->cluster_heap_offset ||
        (device->block_count != 0 && context->volume_length > device->block_count)) {
        kfree(context);
        return 0;
    }
    if (!load_upcase(context)) {
        exfat_context_destroy(context);
        return 0;
    }
    return context;
}

void exfat_context_destroy(EXFAT_CONTEXT *context)
{
    if (context == 0) return;
    if (context->upcase != 0) kfree(context->upcase);
    kfree(context);
}

int exfat_context_exists(EXFAT_CONTEXT *context, const char *path)
{
    EXFAT_NODE node;
    return context != 0 && path != 0 && resolve_path(context, path, &node);
}

int exfat_context_is_directory(EXFAT_CONTEXT *context, const char *path)
{
    EXFAT_NODE node;
    return context != 0 && path != 0 && resolve_path(context, path, &node) &&
           (node.attributes & EXFAT_ATTR_DIRECTORY) != 0;
}

UINT64 exfat_context_file_size(EXFAT_CONTEXT *context, const char *path)
{
    EXFAT_NODE node;
    if (context == 0 || path == 0 || !resolve_path(context, path, &node) ||
        (node.attributes & EXFAT_ATTR_DIRECTORY) != 0) return 0;
    return node.data_length;
}

UINT64 exfat_context_read_file(EXFAT_CONTEXT *context, const char *path,
                               void *buffer, UINT64 capacity)
{
    EXFAT_NODE node;
    UINT64 valid;
    UINT64 total;
    UINT8 *bytes = (UINT8 *)buffer;
    if (context == 0 || path == 0 || buffer == 0 ||
        !resolve_path(context, path, &node) ||
        (node.attributes & EXFAT_ATTR_DIRECTORY) != 0) return 0;
    if (capacity > node.data_length) capacity = node.data_length;
    valid = capacity < node.valid_data_length ? capacity : node.valid_data_length;
    total = read_stream(context, node.first_cluster, node.no_fat_chain,
                        node.valid_data_length, 0, buffer, valid);
    if (total != valid) return total;
    while (total < capacity) bytes[total++] = 0;
    return total;
}

typedef struct { EXFAT_FILE_INFO *entries; UINT64 capacity; UINT64 count; } LIST_STATE;

static int list_visitor(EXFAT_CONTEXT *context, const EXFAT_NODE *node, void *opaque)
{
    LIST_STATE *state = (LIST_STATE *)opaque;
    EXFAT_FILE_INFO *entry;
    (void)context;
    if (state->count >= state->capacity) return 0;
    entry = &state->entries[state->count];
    if (utf16_to_utf8(node->name, node->name_length,
                      entry->name, sizeof(entry->name)) == 0) return 1;
    entry->size = node->data_length;
    entry->is_directory = (node->attributes & EXFAT_ATTR_DIRECTORY) != 0;
    entry->read_only = 1;
    state->count++;
    return state->count < state->capacity;
}

UINT64 exfat_context_list_directory(EXFAT_CONTEXT *context, const char *path,
                                    EXFAT_FILE_INFO *entries, UINT64 capacity)
{
    EXFAT_NODE node;
    LIST_STATE state;
    if (context == 0 || path == 0 || entries == 0 || capacity == 0 ||
        !resolve_path(context, path, &node) ||
        (node.attributes & EXFAT_ATTR_DIRECTORY) == 0) return 0;
    state.entries = entries;
    state.capacity = capacity;
    state.count = 0;
    if (!visit_directory(context, &node, list_visitor, &state)) return 0;
    return state.count;
}

static int remount_verify(EXFAT_CONTEXT *context, const char *path,
                          int exists, int directory, UINT64 size)
{
    EXFAT_CONTEXT *fresh = exfat_context_create(context->device);
    int result;
    if (fresh == 0) return 0;
    result = exfat_context_exists(fresh, path) == exists;
    if (result && exists) {
        result = exfat_context_is_directory(fresh, path) == directory;
        if (result && !directory)
            result = exfat_context_file_size(fresh, path) == size;
    }
    exfat_context_destroy(fresh);
    return result;
}

static int create_entry(EXFAT_CONTEXT *context, const char *path,
                        UINT16 attributes, const void *buffer, UINT64 size)
{
    char parent_path[512];
    UINT16 leaf[255];
    UINT32 leaf_length;
    EXFAT_NODE parent;
    UINT32 cluster_count = size == 0 ? 0U :
        (UINT32)((size + context->cluster_size - 1U) / context->cluster_size);
    UINT32 *clusters = 0;
    UINT32 first_cluster = 0;
    int directory = (attributes & EXFAT_ATTR_DIRECTORY) != 0;
    int result = 0;
    if (exfat_context_exists(context, path) ||
        !split_parent_path(path, parent_path, sizeof(parent_path),
                           leaf, &leaf_length) ||
        !resolve_path(context, parent_path, &parent) ||
        (parent.attributes & EXFAT_ATTR_DIRECTORY) == 0) return 0;
    if (directory) cluster_count = 1U, size = context->cluster_size;
    if (cluster_count != 0) {
        clusters = (UINT32 *)kmalloc((UINTN)cluster_count * sizeof(UINT32));
        if (clusters == 0) return 0;
    }
    if (!transaction_begin(context)) goto done;
    if (cluster_count != 0) {
        if (!allocate_clusters(context, cluster_count, clusters) ||
            !write_new_clusters(context, clusters, cluster_count,
                                directory ? 0 : buffer, directory ? 0 : size))
            goto rollback;
        first_cluster = clusters[0];
    }
    if (!publish_entry(context, &parent, leaf, leaf_length, attributes,
                       first_cluster, size, directory ? size : size, 0, 0))
        goto rollback;
    if (!transaction_commit(context) ||
        !remount_verify(context, path, 1, directory, directory ? 0 : size))
        goto done;
    result = 1;
    goto done;
rollback:
    (void)transaction_rollback(context);
done:
    if (clusters != 0) kfree(clusters);
    return result;
}

int exfat_context_write_file(EXFAT_CONTEXT *context, const char *path,
                             const void *buffer, UINT64 size)
{
    EXFAT_NODE node;
    UINT32 *old_clusters = 0;
    UINT32 old_count = 0;
    UINT32 *new_clusters = 0;
    UINT32 new_count;
    UINT32 first_cluster = 0;
    int result = 0;
    if (context == 0 || path == 0 || (size != 0 && buffer == 0)) return 0;
    if (!resolve_path(context, path, &node))
        return create_entry(context, path, 0x20U, buffer, size);
    if ((node.attributes & EXFAT_ATTR_DIRECTORY) != 0 ||
        !collect_clusters(context, &node, &old_clusters, &old_count)) return 0;
    new_count = size == 0 ? 0U :
        (UINT32)((size + context->cluster_size - 1U) / context->cluster_size);
    if (new_count != 0) {
        new_clusters = (UINT32 *)kmalloc((UINTN)new_count * sizeof(UINT32));
        if (new_clusters == 0) goto done;
    }
    if (!transaction_begin(context)) goto done;
    if (new_count != 0) {
        if (!allocate_clusters(context, new_count, new_clusters) ||
            !write_new_clusters(context, new_clusters, new_count, buffer, size))
            goto rollback;
        first_cluster = new_clusters[0];
    }
    if (!update_node_stream(context, &node, first_cluster, size, size, 0) ||
        !free_clusters(context, old_clusters, old_count)) goto rollback;
    if (!transaction_commit(context) || !remount_verify(context, path, 1, 0, size))
        goto done;
    result = 1;
    goto done;
rollback:
    (void)transaction_rollback(context);
done:
    if (old_clusters != 0) kfree(old_clusters);
    if (new_clusters != 0) kfree(new_clusters);
    return result;
}

typedef struct { UINT32 count; } COUNT_STATE;

static int count_visitor(EXFAT_CONTEXT *context, const EXFAT_NODE *node,
                         void *opaque)
{
    COUNT_STATE *state = (COUNT_STATE *)opaque;
    (void)context; (void)node;
    state->count++;
    return 0;
}

static int directory_empty(EXFAT_CONTEXT *context, const EXFAT_NODE *directory)
{
    COUNT_STATE state;
    state.count = 0;
    return visit_directory(context, directory, count_visitor, &state) &&
           state.count == 0;
}

static int delete_entry(EXFAT_CONTEXT *context, const char *path, int directory)
{
    EXFAT_NODE node;
    UINT32 *clusters = 0;
    UINT32 count = 0;
    int result = 0;
    if (!resolve_path(context, path, &node) ||
        (((node.attributes & EXFAT_ATTR_DIRECTORY) != 0) != directory) ||
        (directory && !directory_empty(context, &node)) ||
        !collect_clusters(context, &node, &clusters, &count) ||
        !transaction_begin(context)) goto done;
    if (!retire_entry(context, &node) || !free_clusters(context, clusters, count))
        goto rollback;
    if (!transaction_commit(context) || !remount_verify(context, path, 0, 0, 0))
        goto done;
    result = 1;
    goto done;
rollback:
    (void)transaction_rollback(context);
done:
    if (clusters != 0) kfree(clusters);
    return result;
}

int exfat_context_delete_file(EXFAT_CONTEXT *context, const char *path)
{
    return context != 0 && path != 0 && delete_entry(context, path, 0);
}

int exfat_context_create_directory(EXFAT_CONTEXT *context, const char *path)
{
    return context != 0 && path != 0 &&
           create_entry(context, path, EXFAT_ATTR_DIRECTORY, 0, 0);
}

int exfat_context_delete_directory(EXFAT_CONTEXT *context, const char *path)
{
    return context != 0 && path != 0 && delete_entry(context, path, 1);
}

static int path_is_descendant(const char *parent, const char *candidate)
{
    UINT32 index = 0;
    while (parent[index] != '\0' && candidate[index] == parent[index]) index++;
    return parent[index] == '\0' && candidate[index] == '/';
}

int exfat_context_rename(EXFAT_CONTEXT *context, const char *source,
                         const char *destination)
{
    EXFAT_NODE node;
    EXFAT_NODE parent;
    EXFAT_NODE refreshed;
    char parent_path[512];
    UINT16 leaf[255];
    UINT32 leaf_length;
    int directory;
    int result = 0;
    if (context == 0 || source == 0 || destination == 0 ||
        !resolve_path(context, source, &node) ||
        exfat_context_exists(context, destination) ||
        !split_parent_path(destination, parent_path, sizeof(parent_path),
                           leaf, &leaf_length) ||
        !resolve_path(context, parent_path, &parent) ||
        (parent.attributes & EXFAT_ATTR_DIRECTORY) == 0) return 0;
    directory = (node.attributes & EXFAT_ATTR_DIRECTORY) != 0;
    if (directory && path_is_descendant(source, destination)) return 0;
    if (!transaction_begin(context)) return 0;
    if (!publish_entry(context, &parent, leaf, leaf_length, node.attributes,
                       node.first_cluster, node.data_length,
                       node.valid_data_length, node.no_fat_chain, 0) ||
        !resolve_path(context, source, &refreshed) ||
        !retire_entry(context, &refreshed)) goto rollback;
    if (!transaction_commit(context) ||
        !remount_verify(context, source, 0, 0, 0) ||
        !remount_verify(context, destination, 1, directory,
                        directory ? 0 : node.data_length)) return 0;
    result = 1;
    return result;
rollback:
    (void)transaction_rollback(context);
    return 0;
}

int exfat_context_sync(EXFAT_CONTEXT *context)
{
    return context != 0 && block_device_flush(context->device);
}

typedef struct { UINT8 *bytes; UINT32 sectors; } EXFAT_TEST_DISK;

static int test_disk_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                          UINT32 count, void *buffer)
{
    EXFAT_TEST_DISK *disk = (EXFAT_TEST_DISK *)device->driver_context;
    UINT8 *destination = (UINT8 *)buffer;
    UINT64 bytes = (UINT64)count * 512U;
    UINT64 index;
    if (lba >= disk->sectors || count > disk->sectors - lba) return 0;
    for (index = 0; index < bytes; index++)
        destination[index] = disk->bytes[lba * 512U + index];
    return 1;
}

static int test_disk_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                           UINT32 count, const void *buffer)
{
    EXFAT_TEST_DISK *disk = (EXFAT_TEST_DISK *)device->driver_context;
    const UINT8 *source = (const UINT8 *)buffer;
    UINT64 bytes = (UINT64)count * 512U;
    UINT64 index;
    if (lba >= disk->sectors || count > disk->sectors - lba) return 0;
    for (index = 0; index < bytes; index++)
        disk->bytes[lba * 512U + index] = source[index];
    return 1;
}

static int test_disk_flush(ASAS_BLOCK_DEVICE *device)
{
    (void)device;
    return 1;
}

static const ASAS_BLOCK_DEVICE_OPS test_disk_ops = {
    test_disk_read, test_disk_write, test_disk_flush
};

static void build_test_boot_region(UINT8 *image, UINT32 base)
{
    UINT8 *boot = image + (UINT64)base * 512U;
    UINT32 checksum = 0;
    UINT32 sector;
    UINT32 byte;
    clear_bytes(boot, 12U * 512U);
    boot[0] = 0xEBU; boot[1] = 0x76U; boot[2] = 0x90U;
    boot[3] = 'E'; boot[4] = 'X'; boot[5] = 'F'; boot[6] = 'A';
    boot[7] = 'T'; boot[8] = ' '; boot[9] = ' '; boot[10] = ' ';
    write_u64(boot + 72U, 128U);
    write_u32(boot + 80U, 24U);
    write_u32(boot + 84U, 1U);
    write_u32(boot + 88U, 25U);
    write_u32(boot + 92U, 100U);
    write_u32(boot + 96U, 2U);
    boot[108] = 9U;
    boot[109] = 0U;
    boot[110] = 1U;
    boot[510] = 0x55U; boot[511] = 0xAAU;
    for (sector = 0; sector < 11U; sector++)
        for (byte = 0; byte < 512U; byte++) {
            if (sector == 0 && (byte == 106U || byte == 107U || byte == 112U))
                continue;
            checksum = boot_checksum_step(checksum,
                                           boot[sector * 512U + byte]);
        }
    for (byte = 0; byte < 512U; byte += 4U)
        write_u32(boot + 11U * 512U + byte, checksum);
}

static int mutation_self_test(void)
{
    static const char payload[] = "exFAT mutation payload";
    EXFAT_TEST_DISK disk;
    ASAS_BLOCK_DEVICE device;
    EXFAT_CONTEXT *context;
    EXFAT_CONTEXT *fresh;
    UINT8 *image = (UINT8 *)kmalloc(128U * 512U);
    UINT8 *fat;
    UINT8 *root;
    UINT8 *bitmap;
    UINT8 *upcase;
    UINT8 verify[32];
    UINT32 checksum = 0;
    UINT32 index;
    int result = 0;
    if (image == 0) return 0;
    clear_bytes(image, 128U * 512U);
    build_test_boot_region(image, 0);
    build_test_boot_region(image, 12);
    fat = image + 24U * 512U;
    root = image + 25U * 512U;
    bitmap = image + 26U * 512U;
    upcase = image + 27U * 512U;
    write_u32(fat + 2U * 4U, 0xFFFFFFFFU);
    write_u32(fat + 3U * 4U, 0xFFFFFFFFU);
    write_u32(fat + 4U * 4U, 0xFFFFFFFFU);
    bitmap[0] = 0x07U;
    write_u16(upcase, 0xFFFFU);
    write_u16(upcase + 2U, 0xFFFFU);
    write_u16(upcase + 4U, 0xFFFEU);
    for (index = 0; index < 6U; index++)
        checksum = stream_checksum_step(checksum, upcase[index]);
    root[0] = EXFAT_ENTRY_BITMAP;
    write_u32(root + 20U, 3U);
    write_u64(root + 24U, 13U);
    root[32U] = EXFAT_ENTRY_UPCASE;
    write_u32(root + 36U, checksum);
    write_u32(root + 52U, 4U);
    write_u64(root + 56U, 6U);
    clear_bytes(&device, sizeof(device));
    disk.bytes = image;
    disk.sectors = 128U;
    device.logical_block_size = 512U;
    device.physical_block_size = 512U;
    device.block_count = 128U;
    device.ops = &test_disk_ops;
    device.driver_context = &disk;
    context = exfat_context_create(&device);
    if (context == 0 ||
        !exfat_context_write_file(context, "/HELLO.TXT", payload,
                                  sizeof(payload) - 1U) ||
        exfat_context_read_file(context, "/HELLO.TXT", verify,
                                sizeof(verify)) != sizeof(payload) - 1U ||
        !exfat_context_create_directory(context, "/WORK") ||
        !exfat_context_rename(context, "/HELLO.TXT", "/WORK/MOVED.TXT") ||
        !exfat_context_delete_file(context, "/WORK/MOVED.TXT") ||
        !exfat_context_delete_directory(context, "/WORK")) goto done;
    fresh = exfat_context_create(&device);
    if (fresh == 0) goto done;
    result = !exfat_context_exists(fresh, "/HELLO.TXT") &&
             !exfat_context_exists(fresh, "/WORK");
    exfat_context_destroy(fresh);
done:
    if (context != 0) exfat_context_destroy(context);
    kfree(image);
    return result;
}

int exfat_self_test(void)
{
    UINT8 set[96];
    UINT8 built[96];
    UINT16 unicode[3] = { 'A', 0x03A9U, 0x4E2DU };
    EXFAT_CONTEXT context;
    char utf8[16];
    UINT16 checksum;
    clear_bytes(set, sizeof(set));
    set[0] = EXFAT_ENTRY_FILE;
    set[1] = 2;
    set[32] = EXFAT_ENTRY_STREAM;
    set[64] = EXFAT_ENTRY_NAME;
    checksum = entry_set_checksum(set, 3);
    set[2] = (UINT8)checksum;
    set[3] = (UINT8)(checksum >> 8);
    clear_bytes(&context, sizeof(context));
    return entry_set_checksum(set, 3) == checksum &&
           build_entry_set(&context, built, unicode, 3, 0x20U,
                           17U, 4096U, 1024U, 0) == 3U &&
           entry_set_checksum(built, 3) == read_u16(built + 2U) &&
           read_u32(built + 52U) == 17U && read_u64(built + 56U) == 4096U &&
           utf16_to_utf8(unicode, 3, utf8, sizeof(utf8)) == 6U &&
           (UINT8)utf8[0] == 'A' && (UINT8)utf8[1] == 0xCEU &&
           (UINT8)utf8[2] == 0xA9U && (UINT8)utf8[3] == 0xE4U &&
           mutation_self_test();
}
