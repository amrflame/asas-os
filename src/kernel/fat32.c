/*
 * fat32.c -- FAT32 read/write driver for Asas OS
 *
 * Reads and writes FAT32 volumes through their mounted block device.
 */

#include "fat32.h"
#include "virtio_block.h"
#include "logger.h"
#include "block_device.h"
#include "heap.h"

#pragma intrinsic(__inbyte)
unsigned char __inbyte(unsigned short port);
#pragma intrinsic(__outbyte)
void __outbyte(unsigned short port, unsigned char value);

/* -----------------------------------------------------------------------
 * On-disk structures (packed)
 * --------------------------------------------------------------------- */
#pragma pack(push, 1)

typedef struct {
    UINT8  jump[3];
    UINT8  oem[8];
    UINT16 bytes_per_sector;
    UINT8  sectors_per_cluster;
    UINT16 reserved_sectors;
    UINT8  fat_count;
    UINT16 root_entry_count;      /* 0 for FAT32 */
    UINT16 total_sectors_16;
    UINT8  media;
    UINT16 sectors_per_fat_16;    /* 0 for FAT32 */
    UINT16 sectors_per_track;
    UINT16 number_of_heads;
    UINT32 hidden_sectors;
    UINT32 total_sectors_32;
    /* FAT32 extended BPB (offset 36) */
    UINT32 sectors_per_fat;
    UINT16 ext_flags;
    UINT16 fs_version;
    UINT32 root_cluster;
    UINT16 fs_info_sector;
    UINT16 backup_boot_sector;
    UINT8  reserved[12];
    UINT8  drive_number;
    UINT8  reserved2;
    UINT8  boot_signature;
    UINT32 volume_id;
    UINT8  volume_label[11];
    UINT8  fs_type[8];            /* "FAT32   " */
} FAT32_BPB;

typedef struct {
    UINT8  name[11];
    UINT8  attributes;
    UINT8  nt_reserved;
    UINT8  create_tenths;
    UINT16 create_time;
    UINT16 create_date;
    UINT16 access_date;
    UINT16 first_cluster_hi;
    UINT16 write_time;
    UINT16 write_date;
    UINT16 first_cluster_lo;
    UINT32 file_size;
} FAT32_DIR_ENTRY;

typedef struct {
    UINT8 order;
    UINT16 name1[5];
    UINT8 attributes;
    UINT8 type;
    UINT8 checksum;
    UINT16 name2[6];
    UINT16 first_cluster;
    UINT16 name3[2];
} FAT32_LFN_ENTRY;

#pragma pack(pop)

#define FAT32_ATTR_READ_ONLY  0x01u
#define FAT32_ATTR_DIRECTORY  0x10u
#define FAT32_ATTR_ARCHIVE    0x20u
#define FAT32_ATTR_VOLUME_ID  0x08u
#define FAT32_ATTR_LFN        0x0Fu
#define FAT32_EOC             0x0FFFFFF8u
#define FAT32_EOC_MARK        0x0FFFFFFFu
#define FAT32_BAD_CLUSTER     0x0FFFFFF7u
#define FAT32_RESERVED_START  0x0FFFFFF0u
#define FAT32_FSINFO_UNKNOWN  0xFFFFFFFFu
#define FAT32_FSINFO_LEAD_SIG 0x41615252u
#define FAT32_FSINFO_STR_SIG  0x61417272u
#define FAT32_FSINFO_TRAIL_SIG 0xAA550000u
#define FAT32_MAX_SECTOR_SIZE 4096U

/* -----------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------- */
struct FAT32_CONTEXT {
    ASAS_BLOCK_DEVICE *device;
    FAT32_BPB bpb;
    UINT32 fat_start;
    UINT32 data_start;
    UINT32 free_cluster_count;
    UINT32 next_free_cluster;
    UINT8 fsinfo_valid;
    UINT8 fsinfo_dirty;
    int initialized;
};

static FAT32_CONTEXT legacy_context;
static const char *fat32_last_error = "fat32: no recent error";

static void fat32_set_error(const char *message)
{
    fat32_last_error = message;
}

const char *fat32_context_last_error_string(FAT32_CONTEXT *context)
{
    (void)context;
    return fat32_last_error;
}

#define g_bpb         (context->bpb)
#define g_fat_start   (context->fat_start)
#define g_data_start  (context->data_start)
#define g_initialized (context->initialized)

static int f32_read_sector(FAT32_CONTEXT *context, UINT64 lba, void *buffer)
{
    if (context->device != 0) {
        return block_device_read(context->device, lba, 1, buffer);
    }
    return virtio_block_read_sector(lba, buffer);
}

static int f32_write_sector(FAT32_CONTEXT *context, UINT64 lba, const void *buffer)
{
    if (context->device != 0) {
        return block_device_write(context->device, lba, 1, buffer);
    }
    return virtio_block_write_sector(lba, buffer);
}

static UINT32 cluster_to_lba(FAT32_CONTEXT *context, UINT32 cluster);

static UINT32 fat32_sector_size(FAT32_CONTEXT *context)
{
    if (context->bpb.bytes_per_sector != 0) return context->bpb.bytes_per_sector;
    if (context->device != 0) return context->device->logical_block_size;
    return 512U;
}

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */
static void f32_copy(void *dst, const void *src, UINT32 n)
{
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    UINT32 i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static void f32_clear(void *dst, UINT32 n)
{
    UINT8 *d = (UINT8 *)dst;
    UINT32 i;

    for (i = 0; i < n; i++) d[i] = 0;
}

static UINT32 f32_read_u32(const UINT8 *source)
{
    return (UINT32)source[0] |
           ((UINT32)source[1] << 8) |
           ((UINT32)source[2] << 16) |
           ((UINT32)source[3] << 24);
}

static void f32_write_u32(UINT8 *destination, UINT32 value)
{
    destination[0] = (UINT8)value;
    destination[1] = (UINT8)(value >> 8);
    destination[2] = (UINT8)(value >> 16);
    destination[3] = (UINT8)(value >> 24);
}

static UINT8 cmos_read(UINT8 reg)
{
    __outbyte(0x70, (UINT8)(0x80U | reg));
    return __inbyte(0x71);
}

static UINT8 bcd_to_binary(UINT8 value)
{
    return (UINT8)((value & 0x0FU) + ((value >> 4) * 10U));
}

static void fat32_set_timestamps(FAT32_DIR_ENTRY *entry, int created)
{
    UINT8 status_b = cmos_read(0x0B);
    UINT8 second = cmos_read(0x00);
    UINT8 minute = cmos_read(0x02);
    UINT8 hour = cmos_read(0x04);
    UINT8 day = cmos_read(0x07);
    UINT8 month = cmos_read(0x08);
    UINT16 year = cmos_read(0x09);
    if ((status_b & 0x04U) == 0) {
        second = bcd_to_binary(second);
        minute = bcd_to_binary(minute);
        hour = bcd_to_binary((UINT8)(hour & 0x7FU));
        day = bcd_to_binary(day);
        month = bcd_to_binary(month);
        year = bcd_to_binary((UINT8)year);
    }
    year = (UINT16)(year + 2000U);
    if (year < 1980U) year = 1980U;
    entry->write_time = (UINT16)(((UINT16)hour << 11) |
                                 ((UINT16)minute << 5) | (second / 2U));
    entry->write_date = (UINT16)(((year - 1980U) << 9) |
                                 ((UINT16)month << 5) | day);
    entry->access_date = entry->write_date;
    if (created) {
        entry->create_time = entry->write_time;
        entry->create_date = entry->write_date;
        entry->create_tenths = (UINT8)((second & 1U) * 100U);
    }
}

static int f32_ichar_eq(char a, char b)
{
    if (a >= 'a' && a <= 'z') a = (char)(a - 32);
    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    return a == b;
}

static UINT8 fat32_short_checksum(const UINT8 name[11])
{
    UINT8 sum = 0;
    UINT32 index;
    for (index = 0; index < 11; index++)
        sum = (UINT8)(((sum & 1U) << 7) + (sum >> 1) + name[index]);
    return sum;
}

static void lfn_reset(UINT16 name[256], UINT8 *checksum, UINT8 *valid)
{
    UINT32 index;
    for (index = 0; index < 256; index++) name[index] = 0xFFFFU;
    *checksum = 0;
    *valid = 0;
}

static int lfn_accept(const FAT32_LFN_ENTRY *entry, UINT16 name[256],
                      UINT8 *checksum, UINT8 *valid)
{
    UINT32 ordinal = entry->order & 0x1FU;
    UINT32 base;
    UINT32 index;
    UINT16 chars[13];
    if (ordinal == 0 || ordinal > 20 || entry->type != 0 ||
        entry->first_cluster != 0) return 0;
    if ((entry->order & 0x40U) != 0) {
        lfn_reset(name, checksum, valid);
        *checksum = entry->checksum;
        *valid = 1;
    } else if (!*valid || entry->checksum != *checksum) return 0;
    for (index = 0; index < 5; index++) chars[index] = entry->name1[index];
    for (index = 0; index < 6; index++) chars[5 + index] = entry->name2[index];
    for (index = 0; index < 2; index++) chars[11 + index] = entry->name3[index];
    base = (ordinal - 1U) * 13U;
    for (index = 0; index < 13 && base + index < 255; index++)
        name[base + index] = chars[index];
    return 1;
}

static void utf16_name_to_utf8(const UINT16 name[256], char output[256])
{
    UINT32 input = 0;
    UINT32 out = 0;
    while (input < 255 && name[input] != 0 && name[input] != 0xFFFFU && out + 1 < 256) {
        UINT32 cp = name[input++];
        if (cp >= 0xD800U && cp <= 0xDBFFU && input < 255 &&
            name[input] >= 0xDC00U && name[input] <= 0xDFFFU)
            cp = 0x10000U + ((cp - 0xD800U) << 10) + (name[input++] - 0xDC00U);
        if (cp < 0x80U) output[out++] = (char)cp;
        else if (cp < 0x800U && out + 2 < 256) {
            output[out++] = (char)(0xC0U | (cp >> 6));
            output[out++] = (char)(0x80U | (cp & 0x3FU));
        } else if (cp < 0x10000U && out + 3 < 256) {
            output[out++] = (char)(0xE0U | (cp >> 12));
            output[out++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
            output[out++] = (char)(0x80U | (cp & 0x3FU));
        } else if (cp <= 0x10FFFFU && out + 4 < 256) {
            output[out++] = (char)(0xF0U | (cp >> 18));
            output[out++] = (char)(0x80U | ((cp >> 12) & 0x3FU));
            output[out++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
            output[out++] = (char)(0x80U | (cp & 0x3FU));
        } else output[out++] = '?';
    }
    output[out] = '\0';
}

static int utf8_name_matches(const char *left, const char *right)
{
    UINT32 index = 0;
    while (left[index] && right[index] && right[index] != '/') {
        if ((UINT8)left[index] < 0x80U && (UINT8)right[index] < 0x80U) {
            if (!f32_ichar_eq(left[index], right[index])) return 0;
        } else if (left[index] != right[index]) return 0;
        index++;
    }
    return left[index] == '\0' && (right[index] == '\0' || right[index] == '/');
}

static void format_name(const UINT8 src[11], char dst[13])
{
    UINT32 i, j = 0;
    for (i = 0; i < 8 && src[i] != ' '; i++) dst[j++] = (char)src[i];
    if (src[8] != ' ') {
        dst[j++] = '.';
        for (i = 8; i < 11 && src[i] != ' '; i++) dst[j++] = (char)src[i];
    }
    dst[j] = '\0';
}

static int entry_matches(const FAT32_DIR_ENTRY *e, const char *component)
{
    char name[13];
    UINT32 ni = 0, ci = 0;
    format_name(e->name, name);
    while (name[ni] && component[ci]) {
        if (!f32_ichar_eq(name[ni], component[ci])) return 0;
        ni++; ci++;
    }
    return name[ni] == '\0' && (component[ci] == '\0' || component[ci] == '/');
}

static int component_to_name(const char *component, UINT32 length, UINT8 name[11])
{
    UINT32 component_index;
    UINT32 name_index = 0;
    UINT32 extension_index = 8;
    int in_extension = 0;

    if (length == 0) return 0;
    for (component_index = 0; component_index < 11; component_index++) name[component_index] = ' ';

    for (component_index = 0; component_index < length; component_index++) {
        char value = component[component_index];

        if (value >= 'a' && value <= 'z') value = (char)(value - 'a' + 'A');
        if (value == '.') {
            if (in_extension) return 0;
            in_extension = 1;
            continue;
        }
        if (
            value == '/' ||
            value == '\\' ||
            (!in_extension && name_index >= 8) ||
            (in_extension && extension_index >= 11)
        ) {
            return 0;
        }
        if (in_extension) name[extension_index++] = (UINT8)value;
        else name[name_index++] = (UINT8)value;
    }
    return name_index != 0;
}

static UINT32 fat32_next_cluster(FAT32_CONTEXT *context, UINT32 cluster)
{
    UINT8  sec[FAT32_MAX_SECTOR_SIZE];
    UINT32 bytes_per_sector = fat32_sector_size(context);
    UINT32 fat_offset = cluster * 4;
    UINT32 fat_sector = g_fat_start + fat_offset / bytes_per_sector;
    UINT32 offset     = fat_offset % bytes_per_sector;
    UINT32 val;

    if (!f32_read_sector(context, fat_sector, sec)) return FAT32_EOC;
    val  = (UINT32)sec[offset];
    val |= (UINT32)sec[offset + 1] << 8;
    val |= (UINT32)sec[offset + 2] << 16;
    val |= (UINT32)sec[offset + 3] << 24;
    return val & 0x0FFFFFFFu;
}

static int fat32_set_cluster(FAT32_CONTEXT *context, UINT32 cluster, UINT32 value)
{
    UINT8 sec[FAT32_MAX_SECTOR_SIZE];
    UINT32 bytes_per_sector = fat32_sector_size(context);
    UINT32 fat_index;
    UINT32 fat_offset = cluster * 4;
    UINT32 sector_offset = fat_offset / bytes_per_sector;
    UINT32 offset = fat_offset % bytes_per_sector;

    for (fat_index = 0; fat_index < g_bpb.fat_count; fat_index++) {
        UINT32 fat_sector = g_fat_start + fat_index * g_bpb.sectors_per_fat + sector_offset;

        if (!f32_read_sector(context, fat_sector, sec)) return 0;
        sec[offset] = (UINT8)(value & 0xFF);
        sec[offset + 1] = (UINT8)(value >> 8);
        sec[offset + 2] = (UINT8)(value >> 16);
        sec[offset + 3] = (UINT8)((sec[offset + 3] & 0xF0U) | ((value >> 24) & 0x0FU));
        if (!f32_write_sector(context, fat_sector, sec)) return 0;
    }
    return 1;
}

static int fat32_verify_fat_copies(FAT32_CONTEXT *context)
{
    UINT8 *buffers;
    UINT8 *primary;
    UINT8 *mirror;
    UINT32 fat_index;
    UINT32 sector;
    UINT32 byte;
    const UINT32 chunk_capacity = 8;
    if (g_bpb.fat_count < 2) return 1;
    if (context->device == 0) return 1; /* Modern mounted contexts already verify. */
    buffers = (UINT8 *)kmalloc((UINTN)fat32_sector_size(context) * chunk_capacity * 2U);
    if (buffers == 0) return 0;
    primary = buffers;
    mirror = buffers + fat32_sector_size(context) * chunk_capacity;
    for (fat_index = 1; fat_index < g_bpb.fat_count; fat_index++) {
        for (sector = 0; sector < g_bpb.sectors_per_fat; sector += chunk_capacity) {
            UINT32 chunk = g_bpb.sectors_per_fat - sector;
            UINT32 mirror_lba = g_fat_start + fat_index * g_bpb.sectors_per_fat + sector;
            int equal = 1;
            if (chunk > chunk_capacity) chunk = chunk_capacity;
            if (context->device == 0 ||
                !block_device_read(context->device, g_fat_start + sector, chunk, primary) ||
                !block_device_read(context->device, mirror_lba, chunk, mirror)) {
                kfree(buffers);
                return 0;
            }
            for (byte = 0; byte < chunk * fat32_sector_size(context); byte++) {
                if (primary[byte] != mirror[byte]) { equal = 0; break; }
            }
            if (!equal) {
                if (block_device_has_capability(context->device,
                                                BLOCK_DEVICE_FLAG_READ_ONLY) ||
                    !block_device_write(context->device, mirror_lba, chunk, primary)) {
                    kfree(buffers);
                    return 0;
                }
                logger_write("FAT32", "secondary FAT repaired from primary");
            }
        }
    }
    kfree(buffers);
    return 1;
}

static UINT32 fat32_max_cluster(FAT32_CONTEXT *context)
{
    UINT32 total_sectors = g_bpb.total_sectors_32 != 0
        ? g_bpb.total_sectors_32
        : (UINT32)g_bpb.total_sectors_16;
    UINT32 data_sectors;

    if (context->device != 0 && context->device->block_count != 0 &&
        context->device->block_count <= 0xFFFFFFFFULL &&
        (total_sectors == 0 ||
         total_sectors > (UINT32)context->device->block_count))
        total_sectors = (UINT32)context->device->block_count;
    if (total_sectors <= g_data_start || g_bpb.sectors_per_cluster == 0)
        return 2;

    data_sectors = total_sectors - g_data_start;
    return 2 + data_sectors / g_bpb.sectors_per_cluster;
}

static int fat32_data_cluster_valid(FAT32_CONTEXT *context, UINT32 cluster)
{
    return cluster >= 2 && cluster < fat32_max_cluster(context);
}

static int fat32_chain_advance(FAT32_CONTEXT *context, UINT32 cluster,
                               UINT32 *next)
{
    UINT32 value;
    if (!fat32_data_cluster_valid(context, cluster) || next == 0) return 0;
    value = fat32_next_cluster(context, cluster);
    if (value >= FAT32_EOC) {
        *next = FAT32_EOC;
        return 1;
    }
    if (value == 0 || value == FAT32_BAD_CLUSTER ||
        value >= FAT32_RESERVED_START ||
        !fat32_data_cluster_valid(context, value)) return 0;
    *next = value;
    return 1;
}

static int fat32_chain_valid(FAT32_CONTEXT *context, UINT32 first_cluster)
{
    UINT32 current = first_cluster;
    UINT32 next;
    UINT32 hops = 0;
    UINT32 limit = fat32_max_cluster(context) - 2;
    if (!fat32_data_cluster_valid(context, current)) return 0;
    while (hops++ < limit) {
        if (!fat32_chain_advance(context, current, &next)) return 0;
        if (next >= FAT32_EOC) return 1;
        current = next;
    }
    return 0;
}

static UINT32 fat32_allocate_cluster(FAT32_CONTEXT *context)
{
    UINT32 maximum_cluster = fat32_max_cluster(context);
    UINT32 cluster = context->next_free_cluster;
    UINT32 attempts = 0;

    if (maximum_cluster <= 2) {
        fat32_set_error("fat32: invalid cluster range");
        return 0;
    }
    if (cluster < 2 || cluster >= maximum_cluster) cluster = 2;
    while (attempts < maximum_cluster - 2) {
        if (fat32_next_cluster(context, cluster) == 0) {
            if (fat32_set_cluster(context, cluster, FAT32_EOC_MARK)) {
                UINT8 zero[FAT32_MAX_SECTOR_SIZE];
                UINT32 sector;

                f32_clear(zero, fat32_sector_size(context));
                for (sector = 0; sector < g_bpb.sectors_per_cluster; sector++) {
                    if (!f32_write_sector(context, cluster_to_lba(context, cluster) + sector, zero)) {
                        (void)fat32_set_cluster(context, cluster, 0);
                        fat32_set_error("fat32: new cluster zero write failed");
                        return 0;
                    }
                }
                if (context->free_cluster_count != FAT32_FSINFO_UNKNOWN &&
                    context->free_cluster_count > 0) {
                    context->free_cluster_count--;
                }
                context->next_free_cluster = cluster + 1;
                if (context->next_free_cluster >= maximum_cluster)
                    context->next_free_cluster = 2;
                context->fsinfo_dirty = context->fsinfo_valid;
                return cluster;
            }
            fat32_set_error("fat32: FAT entry write failed");
            return 0;
        }
        cluster++;
        if (cluster >= maximum_cluster) cluster = 2;
        attempts++;
    }
    if (context->free_cluster_count != FAT32_FSINFO_UNKNOWN) {
        context->free_cluster_count = 0;
        context->fsinfo_dirty = context->fsinfo_valid;
    }
    fat32_set_error("fat32: no free cluster found in FAT");
    return 0;
}

static int fat32_free_chain(FAT32_CONTEXT *context, UINT32 cluster)
{
    UINT32 maximum_cluster = fat32_max_cluster(context);
    UINT32 released = 0;
    UINT32 first_released = cluster;
    if (!fat32_chain_valid(context, cluster)) return 0;
    while (cluster >= 2 && cluster < maximum_cluster) {
        UINT32 next;

        if (!fat32_chain_advance(context, cluster, &next)) return 0;
        if (!fat32_set_cluster(context, cluster, 0)) return 0;
        released++;
        if (next >= FAT32_EOC) break;
        cluster = next;
    }
    if (released == 0) return 0;
    if (context->free_cluster_count != FAT32_FSINFO_UNKNOWN) {
        UINT32 capacity = maximum_cluster - 2;
        if (released > capacity - context->free_cluster_count)
            context->free_cluster_count = capacity;
        else context->free_cluster_count += released;
    }
    if (first_released >= 2 && first_released < context->next_free_cluster)
        context->next_free_cluster = first_released;
    context->fsinfo_dirty = context->fsinfo_valid;
    return 1;
}

static UINT32 fat32_allocate_chain(FAT32_CONTEXT *context, UINT32 cluster_count)
{
    UINT32 first = 0;
    UINT32 previous = 0;
    UINT32 index;

    if (cluster_count == 0 ||
        (context->free_cluster_count != FAT32_FSINFO_UNKNOWN &&
         cluster_count > context->free_cluster_count)) return 0;

    for (index = 0; index < cluster_count; index++) {
        UINT32 cluster = fat32_allocate_cluster(context);

        if (cluster == 0) {
            if (first != 0) (void)fat32_free_chain(context, first);
            return 0;
        }
        if (first == 0) first = cluster;
        if (previous != 0 && !fat32_set_cluster(context, previous, cluster)) {
            (void)fat32_free_chain(context, first);
            return 0;
        }
        previous = cluster;
    }
    return first;
}

static UINT32 cluster_to_lba(FAT32_CONTEXT *context, UINT32 cluster)
{
    return g_data_start + (cluster - 2) * (UINT32)g_bpb.sectors_per_cluster;
}

/* Search one directory cluster chain for a named component.
   Fills out_entry and returns 1 on success. */
static int search_dir_chain(
    FAT32_CONTEXT *context,
    UINT32 dir_cluster,
    const char *component,
    FAT32_DIR_ENTRY *out_entry)
{
    UINT8 sec[FAT32_MAX_SECTOR_SIZE];
    UINT32 entries_per_sec = fat32_sector_size(context) / sizeof(FAT32_DIR_ENTRY);
    UINT32 current = dir_cluster;
    UINT32 hops = 0;
    UINT32 s, e;
    UINT16 long_name[256];
    UINT8 lfn_checksum;
    UINT8 lfn_valid;
    lfn_reset(long_name, &lfn_checksum, &lfn_valid);

    while (fat32_data_cluster_valid(context, current) &&
           hops++ < fat32_max_cluster(context) - 2) {
        UINT32 lba      = cluster_to_lba(context, current);
        UINT32 sec_cnt  = g_bpb.sectors_per_cluster;

        for (s = 0; s < sec_cnt; s++) {
            if (!f32_read_sector(context, lba + s, sec)) return 0;
            for (e = 0; e < entries_per_sec; e++) {
                FAT32_DIR_ENTRY entry;
                f32_copy(&entry, sec + e * sizeof(FAT32_DIR_ENTRY), sizeof(FAT32_DIR_ENTRY));
                if (entry.name[0] == 0x00) return 0;
                if ((UINT8)entry.name[0] == 0xE5) continue;
                if ((entry.attributes & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                    if (!lfn_accept((const FAT32_LFN_ENTRY *)(sec + e * 32),
                                    long_name, &lfn_checksum, &lfn_valid))
                        lfn_reset(long_name, &lfn_checksum, &lfn_valid);
                    continue;
                }
                if (entry.attributes & FAT32_ATTR_VOLUME_ID) continue;
                if (lfn_valid && lfn_checksum == fat32_short_checksum(entry.name)) {
                    char utf8[256];
                    utf16_name_to_utf8(long_name, utf8);
                    if (utf8_name_matches(utf8, component)) {
                        if (out_entry) f32_copy(out_entry, &entry, sizeof(FAT32_DIR_ENTRY));
                        return 1;
                    }
                }
                if (entry_matches(&entry, component)) {
                    if (out_entry) f32_copy(out_entry, &entry, sizeof(FAT32_DIR_ENTRY));
                    return 1;
                }
                lfn_reset(long_name, &lfn_checksum, &lfn_valid);
            }
        }
        if (!fat32_chain_advance(context, current, &current)) return 0;
    }
    return 0;
}

/* List all real entries in a cluster chain directory.
   Returns number of entries placed in out[]. */
static UINT64 list_dir_chain(FAT32_CONTEXT *context, UINT32 dir_cluster,
                             FAT32_FILE_INFO *out, UINT64 cap)
{
    UINT8 sec[FAT32_MAX_SECTOR_SIZE];
    UINT32 entries_per_sec = fat32_sector_size(context) / sizeof(FAT32_DIR_ENTRY);
    UINT32 current = dir_cluster;
    UINT32 hops = 0;
    UINT64 count = 0;
    UINT32 s, e;
    UINT16 long_name[256];
    UINT8 lfn_checksum;
    UINT8 lfn_valid;
    lfn_reset(long_name, &lfn_checksum, &lfn_valid);

    while (fat32_data_cluster_valid(context, current) && count < cap &&
           hops++ < fat32_max_cluster(context) - 2) {
        UINT32 lba     = cluster_to_lba(context, current);
        UINT32 sec_cnt = g_bpb.sectors_per_cluster;

        for (s = 0; s < sec_cnt && count < cap; s++) {
            if (!f32_read_sector(context, lba + s, sec)) return count;
            for (e = 0; e < entries_per_sec && count < cap; e++) {
                FAT32_DIR_ENTRY entry;
                f32_copy(&entry, sec + e * sizeof(FAT32_DIR_ENTRY), sizeof(FAT32_DIR_ENTRY));
                if (entry.name[0] == 0x00) return count;
                if ((UINT8)entry.name[0] == 0xE5) continue;
                if ((entry.attributes & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                    if (!lfn_accept((const FAT32_LFN_ENTRY *)(sec + e * 32),
                                    long_name, &lfn_checksum, &lfn_valid))
                        lfn_reset(long_name, &lfn_checksum, &lfn_valid);
                    continue;
                }
                if (entry.attributes & FAT32_ATTR_VOLUME_ID) continue;
                if (entry.name[0] == '.') continue; /* skip . and .. */
                if (lfn_valid && lfn_checksum == fat32_short_checksum(entry.name))
                    utf16_name_to_utf8(long_name, out[count].name);
                else format_name(entry.name, out[count].name);
                out[count].size         = entry.file_size;
                out[count].is_directory = (entry.attributes & FAT32_ATTR_DIRECTORY) ? 1 : 0;
                out[count].read_only    = (entry.attributes & FAT32_ATTR_READ_ONLY) ? 1 : 0;
                count++;
                lfn_reset(long_name, &lfn_checksum, &lfn_valid);
            }
        }
        if (!fat32_chain_advance(context, current, &current)) return count;
    }
    return count;
}

/* Resolve an absolute path to its leaf directory entry.
   Returns 1 on success.  out_dir_cluster receives the cluster of the
   *containing* directory (useful for creating/deleting entries). */
static int fat32_resolve(
    FAT32_CONTEXT *context,
    const char *path,
    FAT32_DIR_ENTRY *out_entry,
    UINT32 *out_dir_cluster)
{
    UINT32 current = g_bpb.root_cluster;
    const char *p = path;
    char component[256];
    UINT32 ci;
    FAT32_DIR_ENTRY found;

    while (*p == '/') p++;

    if (*p == '\0') {
        /* Root directory */
        if (out_dir_cluster) *out_dir_cluster = g_bpb.root_cluster;
        if (out_entry) {
            UINT32 i;
            for (i = 0; i < (UINT32)sizeof(FAT32_DIR_ENTRY); i++)
                ((UINT8 *)out_entry)[i] = 0;
            out_entry->attributes        = FAT32_ATTR_DIRECTORY;
            out_entry->first_cluster_lo  = (UINT16)(g_bpb.root_cluster & 0xFFFF);
            out_entry->first_cluster_hi  = (UINT16)(g_bpb.root_cluster >> 16);
        }
        return 1;
    }

    for (;;) {
        ci = 0;
        while (*p && *p != '/' && ci + 1 < sizeof(component)) component[ci++] = *p++;
        if (*p && *p != '/') return 0;
        component[ci] = '\0';
        while (*p == '/') p++;

        if (!search_dir_chain(context, current, component, &found)) return 0;

        if (*p == '\0') {
            if (out_entry)       f32_copy(out_entry, &found, sizeof(FAT32_DIR_ENTRY));
            if (out_dir_cluster) *out_dir_cluster = current;
            return 1;
        }

        if (!(found.attributes & FAT32_ATTR_DIRECTORY)) return 0;
        current = ((UINT32)found.first_cluster_hi << 16) | found.first_cluster_lo;
    }
}

static int fat32_resolve_parent(FAT32_CONTEXT *context, const char *path,
                                UINT32 *parent_cluster, UINT8 name[11])
{
    char parent_path[96];
    UINT32 last_slash = 0;
    UINT32 length = 0;
    UINT32 index;
    FAT32_DIR_ENTRY parent;

    if (path[0] != '/') return 0;
    while (path[length] != '\0') {
        if (path[length] == '/') last_slash = length;
        length++;
    }
    if (
        length <= 1 ||
        !component_to_name(&path[last_slash + 1], length - last_slash - 1, name)
    ) {
        return 0;
    }

    if (last_slash == 0) {
        *parent_cluster = g_bpb.root_cluster;
        return 1;
    }

    for (index = 0; index < last_slash && index + 1 < sizeof(parent_path); index++) {
        parent_path[index] = path[index];
    }
    if (index != last_slash) return 0;
    parent_path[index] = '\0';

    if (!fat32_resolve(context, parent_path, &parent, 0) ||
        (parent.attributes & FAT32_ATTR_DIRECTORY) == 0) {
        return 0;
    }
    *parent_cluster = ((UINT32)parent.first_cluster_hi << 16) | parent.first_cluster_lo;
    if (*parent_cluster < 2) *parent_cluster = g_bpb.root_cluster;
    return 1;
}

#define FAT32_MAX_LFN_ENTRIES 20U
#define FAT32_MAX_NAME_ENTRIES (FAT32_MAX_LFN_ENTRIES + 1U)

typedef struct {
    UINT32 lba;
    UINT16 offset;
} FAT32_ENTRY_POSITION;

typedef struct {
    FAT32_DIR_ENTRY entry;
    FAT32_ENTRY_POSITION positions[FAT32_MAX_NAME_ENTRIES];
    UINT32 position_count;
} FAT32_LOCATED_ENTRY;

static int fat32_resolve_parent_leaf(FAT32_CONTEXT *context, const char *path,
                                     UINT32 *parent_cluster, char leaf[256])
{
    char parent_path[256];
    UINT32 last_slash = 0;
    UINT32 length = 0;
    UINT32 index;
    FAT32_DIR_ENTRY parent;
    if (path == 0 || path[0] != '/') return 0;
    while (path[length] != '\0' && length < 255) {
        if (path[length] == '/') last_slash = length;
        length++;
    }
    if (path[length] != '\0' || length <= 1 || last_slash + 1 >= length) return 0;
    for (index = last_slash + 1; index < length; index++)
        leaf[index - last_slash - 1] = path[index];
    leaf[length - last_slash - 1] = '\0';
    if (last_slash == 0) {
        *parent_cluster = g_bpb.root_cluster;
        return 1;
    }
    for (index = 0; index < last_slash; index++) parent_path[index] = path[index];
    parent_path[last_slash] = '\0';
    if (!fat32_resolve(context, parent_path, &parent, 0) ||
        (parent.attributes & FAT32_ATTR_DIRECTORY) == 0) return 0;
    *parent_cluster = ((UINT32)parent.first_cluster_hi << 16) |
                      parent.first_cluster_lo;
    if (*parent_cluster < 2) *parent_cluster = g_bpb.root_cluster;
    return 1;
}

static int utf8_name_to_utf16(const char *input, UINT16 output[256],
                              UINT32 *unit_count)
{
    UINT32 in = 0;
    UINT32 out = 0;
    if (input == 0 || input[0] == '\0') return 0;
    while (input[in] != '\0') {
        UINT32 cp;
        UINT8 first = (UINT8)input[in++];
        if (first < 0x80U) cp = first;
        else if ((first & 0xE0U) == 0xC0U &&
                 ((UINT8)input[in] & 0xC0U) == 0x80U) {
            cp = ((UINT32)(first & 0x1FU) << 6) | ((UINT8)input[in++] & 0x3FU);
            if (cp < 0x80U) return 0;
        } else if ((first & 0xF0U) == 0xE0U &&
                   ((UINT8)input[in] & 0xC0U) == 0x80U &&
                   ((UINT8)input[in + 1] & 0xC0U) == 0x80U) {
            cp = ((UINT32)(first & 0x0FU) << 12) |
                 (((UINT8)input[in] & 0x3FU) << 6) |
                 ((UINT8)input[in + 1] & 0x3FU);
            in += 2;
            if (cp < 0x800U || (cp >= 0xD800U && cp <= 0xDFFFU)) return 0;
        } else if ((first & 0xF8U) == 0xF0U &&
                   ((UINT8)input[in] & 0xC0U) == 0x80U &&
                   ((UINT8)input[in + 1] & 0xC0U) == 0x80U &&
                   ((UINT8)input[in + 2] & 0xC0U) == 0x80U) {
            cp = ((UINT32)(first & 7U) << 18) |
                 (((UINT8)input[in] & 0x3FU) << 12) |
                 (((UINT8)input[in + 1] & 0x3FU) << 6) |
                 ((UINT8)input[in + 2] & 0x3FU);
            in += 3;
            if (cp < 0x10000U || cp > 0x10FFFFU) return 0;
        } else return 0;
        if (cp < 0x20U || cp == '"' || cp == '*' || cp == '/' || cp == ':' ||
            cp == '<' || cp == '>' || cp == '?' || cp == '\\' || cp == '|') return 0;
        if (cp < 0x10000U) {
            if (out >= 255) return 0;
            output[out++] = (UINT16)cp;
        } else {
            if (out + 1 >= 255) return 0;
            cp -= 0x10000U;
            output[out++] = (UINT16)(0xD800U | (cp >> 10));
            output[out++] = (UINT16)(0xDC00U | (cp & 0x3FFU));
        }
    }
    if (out == 0 || output[out - 1] == ' ' || output[out - 1] == '.' ||
        (out == 1 && output[0] == '.') ||
        (out == 2 && output[0] == '.' && output[1] == '.')) return 0;
    output[out] = 0;
    *unit_count = out;
    return 1;
}

static int fat32_find_named_entry(FAT32_CONTEXT *context, UINT32 dir_cluster,
                                  const char *name, FAT32_LOCATED_ENTRY *located)
{
    UINT8 sector[FAT32_MAX_SECTOR_SIZE];
    UINT16 long_name[256];
    UINT8 checksum;
    UINT8 valid;
    FAT32_ENTRY_POSITION lfn_positions[FAT32_MAX_LFN_ENTRIES];
    UINT32 lfn_count = 0;
    UINT32 current = dir_cluster;
    UINT32 hops = 0;
    lfn_reset(long_name, &checksum, &valid);
    while (fat32_data_cluster_valid(context, current) &&
           hops++ < fat32_max_cluster(context) - 2) {
        UINT32 lba = cluster_to_lba(context, current);
        UINT32 s;
        for (s = 0; s < g_bpb.sectors_per_cluster; s++) {
            UINT32 e;
            if (!f32_read_sector(context, lba + s, sector)) return 0;
            for (e = 0; e < 16; e++) {
                FAT32_DIR_ENTRY *entry = (FAT32_DIR_ENTRY *)(sector + e * 32U);
                if (entry->name[0] == 0) return 0;
                if ((UINT8)entry->name[0] == 0xE5) {
                    lfn_count = 0;
                    lfn_reset(long_name, &checksum, &valid);
                    continue;
                }
                if ((entry->attributes & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                    if (lfn_count >= FAT32_MAX_LFN_ENTRIES ||
                        !lfn_accept((const FAT32_LFN_ENTRY *)entry, long_name,
                                    &checksum, &valid)) {
                        lfn_count = 0;
                        lfn_reset(long_name, &checksum, &valid);
                    } else {
                        lfn_positions[lfn_count].lba = lba + s;
                        lfn_positions[lfn_count++].offset = (UINT16)(e * 32U);
                    }
                    continue;
                }
                if ((entry->attributes & FAT32_ATTR_VOLUME_ID) == 0) {
                    int match = entry_matches(entry, name);
                    if (valid && checksum == fat32_short_checksum(entry->name)) {
                        char utf8[256];
                        utf16_name_to_utf8(long_name, utf8);
                        if (utf8_name_matches(utf8, name)) match = 1;
                    }
                    if (match) {
                        UINT32 i;
                        located->entry = *entry;
                        located->position_count = lfn_count + 1;
                        for (i = 0; i < lfn_count; i++)
                            located->positions[i] = lfn_positions[i];
                        located->positions[lfn_count].lba = lba + s;
                        located->positions[lfn_count].offset = (UINT16)(e * 32U);
                        return 1;
                    }
                }
                lfn_count = 0;
                lfn_reset(long_name, &checksum, &valid);
            }
        }
        if (!fat32_chain_advance(context, current, &current)) return 0;
    }
    return 0;
}

static int fat32_reserve_entries(FAT32_CONTEXT *context, UINT32 dir_cluster,
                                 UINT32 needed,
                                 FAT32_ENTRY_POSITION positions[21])
{
    UINT8 sector[FAT32_MAX_SECTOR_SIZE];
    UINT32 current = dir_cluster;
    UINT32 last = dir_cluster;
    UINT32 run = 0;
    UINT32 hops = 0;
    while (fat32_data_cluster_valid(context, current) &&
           hops++ < fat32_max_cluster(context) - 2) {
        UINT32 lba = cluster_to_lba(context, current);
        UINT32 s;
        last = current;
        for (s = 0; s < g_bpb.sectors_per_cluster; s++) {
            UINT32 e;
            if (!f32_read_sector(context, lba + s, sector)) return 0;
            for (e = 0; e < 16; e++) {
                FAT32_DIR_ENTRY *entry = (FAT32_DIR_ENTRY *)(sector + e * 32U);
                if (entry->name[0] == 0 || (UINT8)entry->name[0] == 0xE5) {
                    positions[run].lba = lba + s;
                    positions[run++].offset = (UINT16)(e * 32U);
                    if (run == needed) return 1;
                } else run = 0;
            }
        }
        if (!fat32_chain_advance(context, current, &current)) return 0;
    }
    while (run < needed) {
        UINT32 new_cluster = fat32_allocate_cluster(context);
        UINT32 lba;
        UINT32 s;
        if (new_cluster == 0 || !fat32_set_cluster(context, last, new_cluster)) {
            if (new_cluster != 0) (void)fat32_free_chain(context, new_cluster);
            return 0;
        }
        last = new_cluster;
        lba = cluster_to_lba(context, new_cluster);
        f32_clear(sector, sizeof(sector));
        for (s = 0; s < g_bpb.sectors_per_cluster; s++) {
            UINT32 e;
            if (!f32_write_sector(context, lba + s, sector)) return 0;
            for (e = 0; e < 16 && run < needed; e++) {
                positions[run].lba = lba + s;
                positions[run++].offset = (UINT16)(e * 32U);
            }
        }
    }
    return 1;
}

static int fat32_position_write(FAT32_CONTEXT *context,
                                FAT32_ENTRY_POSITION position,
                                const void *entry)
{
    UINT8 sector[FAT32_MAX_SECTOR_SIZE];
    if (!f32_read_sector(context, position.lba, sector)) return 0;
    f32_copy(sector + position.offset, entry, 32);
    return f32_write_sector(context, position.lba, sector);
}

static int fat32_mark_positions_deleted(FAT32_CONTEXT *context,
                                        const FAT32_ENTRY_POSITION *positions,
                                        UINT32 count)
{
    UINT8 sector[FAT32_MAX_SECTOR_SIZE];
    UINT32 index;
    for (index = 0; index < count; index++) {
        if (!f32_read_sector(context, positions[index].lba, sector)) return 0;
        sector[positions[index].offset] = 0xE5;
        if (!f32_write_sector(context, positions[index].lba, sector)) return 0;
    }
    return 1;
}

static int find_directory_entry_slot(
    FAT32_CONTEXT *context,
    UINT32 directory_cluster,
    const UINT8 name[11],
    UINT8 sector[FAT32_MAX_SECTOR_SIZE],
    UINT32 *sector_number,
    FAT32_DIR_ENTRY **entry_result,
    int allow_free)
{
    UINT32 current = directory_cluster;
    UINT32 last_cluster = directory_cluster;
    UINT32 hops = 0;
    UINT32 entries_per_sec = fat32_sector_size(context) / sizeof(FAT32_DIR_ENTRY);
    UINT32 s, e;

    while (fat32_data_cluster_valid(context, current) &&
           hops++ < fat32_max_cluster(context) - 2) {
        last_cluster = current;
        UINT32 lba = cluster_to_lba(context, current);

        for (s = 0; s < g_bpb.sectors_per_cluster; s++) {
            if (!f32_read_sector(context, lba + s, sector)) return 0;
            for (e = 0; e < entries_per_sec; e++) {
                FAT32_DIR_ENTRY *entry = (FAT32_DIR_ENTRY *)&sector[e * sizeof(FAT32_DIR_ENTRY)];

                if (
                    entry->name[0] != 0 &&
                    (UINT8)entry->name[0] != 0xE5 &&
                    (entry->attributes & FAT32_ATTR_LFN) != FAT32_ATTR_LFN
                ) {
                    UINT32 index;
                    int match = 1;

                    for (index = 0; index < 11; index++) {
                        if (entry->name[index] != name[index]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        *sector_number = lba + s;
                        *entry_result = entry;
                        return 1;
                    }
                }
            }
        }
        if (!fat32_chain_advance(context, current, &current)) return 0;
    }

    if (!allow_free) return 0;
    current = directory_cluster;
    hops = 0;
    while (fat32_data_cluster_valid(context, current) &&
           hops++ < fat32_max_cluster(context) - 2) {
        UINT32 lba = cluster_to_lba(context, current);

        for (s = 0; s < g_bpb.sectors_per_cluster; s++) {
            if (!f32_read_sector(context, lba + s, sector)) return 0;
            for (e = 0; e < entries_per_sec; e++) {
                FAT32_DIR_ENTRY *entry = (FAT32_DIR_ENTRY *)&sector[e * sizeof(FAT32_DIR_ENTRY)];

                if (entry->name[0] == 0 || (UINT8)entry->name[0] == 0xE5) {
                    *sector_number = lba + s;
                    *entry_result = entry;
                    return 1;
                }
            }
        }
        if (!fat32_chain_advance(context, current, &current)) return 0;
    }
    if (allow_free && fat32_data_cluster_valid(context, last_cluster)) {
        UINT32 new_cluster = fat32_allocate_cluster(context);
        UINT32 lba;
        if (new_cluster == 0) return 0;
        if (!fat32_set_cluster(context, last_cluster, new_cluster)) {
            (void)fat32_free_chain(context, new_cluster);
            return 0;
        }
        lba = cluster_to_lba(context, new_cluster);
        f32_clear(sector, fat32_sector_size(context));
        *sector_number = lba;
        *entry_result = (FAT32_DIR_ENTRY *)sector;
        return 1;
    }
    return 0;
}

static int fat32_short_name_available(FAT32_CONTEXT *context,
                                      UINT32 parent_cluster,
                                      const UINT8 name[11])
{
    UINT8 sector[FAT32_MAX_SECTOR_SIZE];
    UINT32 lba;
    FAT32_DIR_ENTRY *entry;
    return !find_directory_entry_slot(context, parent_cluster, name, sector,
                                      &lba, &entry, 0);
}

static int fat32_make_short_alias(FAT32_CONTEXT *context, UINT32 parent_cluster,
                                  const char *leaf, UINT8 alias[11],
                                  int *needs_lfn)
{
    UINT8 exact[11];
    char formatted[13];
    UINT32 length = 0;
    UINT32 dot = 0xFFFFFFFFU;
    UINT32 index;
    UINT32 base_count = 0;
    UINT8 base[6];
    UINT8 extension[3];
    UINT32 extension_count = 0;
    UINT32 number;
    while (leaf[length] != '\0') {
        if (leaf[length] == '.') dot = length;
        length++;
    }
    if (component_to_name(leaf, length, exact)) {
        format_name(exact, formatted);
        if (utf8_name_matches(formatted, leaf) &&
            fat32_short_name_available(context, parent_cluster, exact)) {
            f32_copy(alias, exact, 11);
            *needs_lfn = 0;
            return 1;
        }
    }
    for (index = 0; index < length && index != dot && base_count < 6; index++) {
        UINT8 value = (UINT8)leaf[index];
        if (value >= 'a' && value <= 'z') value = (UINT8)(value - 32);
        if ((value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '_') base[base_count++] = value;
    }
    if (base_count == 0) base[base_count++] = '_';
    if (dot != 0xFFFFFFFFU) {
        for (index = dot + 1; index < length && extension_count < 3; index++) {
            UINT8 value = (UINT8)leaf[index];
            if (value >= 'a' && value <= 'z') value = (UINT8)(value - 32);
            if ((value >= 'A' && value <= 'Z') ||
                (value >= '0' && value <= '9') || value == '_')
                extension[extension_count++] = value;
        }
    }
    for (number = 1; number <= 999; number++) {
        UINT8 digits[3];
        UINT32 digit_count = number >= 100 ? 3 : (number >= 10 ? 2 : 1);
        UINT32 keep = 8U - digit_count - 1U;
        UINT32 out = 0;
        UINT32 value = number;
        for (index = 0; index < 11; index++) alias[index] = ' ';
        if (keep > base_count) keep = base_count;
        for (index = 0; index < keep; index++) alias[out++] = base[index];
        alias[out++] = '~';
        for (index = 0; index < digit_count; index++) {
            digits[digit_count - index - 1] = (UINT8)('0' + value % 10U);
            value /= 10U;
        }
        for (index = 0; index < digit_count; index++) alias[out++] = digits[index];
        for (index = 0; index < extension_count; index++) alias[8 + index] = extension[index];
        if (fat32_short_name_available(context, parent_cluster, alias)) {
            *needs_lfn = 1;
            return 1;
        }
    }
    return 0;
}

static void fat32_build_lfn_entry(FAT32_LFN_ENTRY *entry,
                                  const UINT16 name[256], UINT32 units,
                                  UINT32 ordinal, UINT32 total, UINT8 checksum)
{
    UINT16 chars[13];
    UINT32 base = (ordinal - 1U) * 13U;
    UINT32 index;
    f32_clear(entry, sizeof(*entry));
    for (index = 0; index < 13; index++) {
        UINT32 name_index = base + index;
        chars[index] = name_index < units ? name[name_index] :
                       (name_index == units ? 0 : 0xFFFFU);
    }
    entry->order = (UINT8)ordinal;
    if (ordinal == total) entry->order |= 0x40U;
    entry->attributes = FAT32_ATTR_LFN;
    entry->checksum = checksum;
    for (index = 0; index < 5; index++) entry->name1[index] = chars[index];
    for (index = 0; index < 6; index++) entry->name2[index] = chars[5 + index];
    for (index = 0; index < 2; index++) entry->name3[index] = chars[11 + index];
}

static int fat32_write_named_entry(FAT32_CONTEXT *context,
                                   UINT32 parent_cluster, const char *leaf,
                                   const FAT32_DIR_ENTRY *source,
                                   FAT32_LOCATED_ENTRY *created)
{
    UINT16 utf16[256];
    UINT32 units;
    UINT8 alias[11];
    int needs_lfn;
    UINT32 lfn_count;
    UINT32 needed;
    UINT32 index;
    FAT32_DIR_ENTRY short_entry = *source;
    if (!utf8_name_to_utf16(leaf, utf16, &units) ||
        !fat32_make_short_alias(context, parent_cluster, leaf, alias, &needs_lfn)) return 0;
    lfn_count = needs_lfn ? (units + 12U) / 13U : 0;
    if (lfn_count > FAT32_MAX_LFN_ENTRIES) return 0;
    needed = lfn_count + 1U;
    if (!fat32_reserve_entries(context, parent_cluster, needed,
                               created->positions)) return 0;
    f32_copy(short_entry.name, alias, 11);
    if (needs_lfn) {
        UINT8 checksum = fat32_short_checksum(alias);
        for (index = 0; index < lfn_count; index++) {
            FAT32_LFN_ENTRY lfn;
            UINT32 ordinal = lfn_count - index;
            fat32_build_lfn_entry(&lfn, utf16, units, ordinal, lfn_count,
                                  checksum);
            if (!fat32_position_write(context, created->positions[index], &lfn)) {
                (void)fat32_mark_positions_deleted(context, created->positions, index);
                return 0;
            }
        }
    }
    if (!fat32_position_write(context, created->positions[lfn_count],
                              &short_entry)) {
        (void)fat32_mark_positions_deleted(context, created->positions, lfn_count);
        return 0;
    }
    created->entry = short_entry;
    created->position_count = needed;
    return 1;
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */
static int fat32_detect_context(FAT32_CONTEXT *context)
{
    UINT8 sec[FAT32_MAX_SECTOR_SIZE];
    if (!f32_read_sector(context, 0, sec)) return 0;
    /* sectors_per_fat_16 at offset 22 must be 0 for FAT32 */
    if (sec[22] != 0 || sec[23] != 0) return 0;
    /* fs_type "FAT32   " at offset 82 */
    return sec[82]=='F' && sec[83]=='A' && sec[84]=='T' &&
           sec[85]=='3' && sec[86]=='2' ? 1 : 0;
}

static int fat32_initialize_context(FAT32_CONTEXT *context)
{
    UINT8 sec[FAT32_MAX_SECTOR_SIZE];
    UINT32 i;
    UINT32 fsinfo_sector;

    if (!f32_read_sector(context, 0, sec)) return 0;
    for (i = 0; i < sizeof(FAT32_BPB); i++) ((UINT8 *)&g_bpb)[i] = sec[i];

    if ((g_bpb.bytes_per_sector != 512 &&
         g_bpb.bytes_per_sector != 1024 &&
         g_bpb.bytes_per_sector != 2048 &&
         g_bpb.bytes_per_sector != 4096) ||
        (context->device != 0 && context->device->logical_block_size !=
                                g_bpb.bytes_per_sector) ||
        g_bpb.sectors_per_cluster == 0 ||
        g_bpb.sectors_per_fat    == 0  ||
        g_bpb.root_cluster       < 2) return 0;

    g_fat_start  = g_bpb.reserved_sectors;
    g_data_start = g_fat_start + (UINT32)g_bpb.fat_count * g_bpb.sectors_per_fat;
    if (fat32_max_cluster(context) <= 2) return 0;
    if (g_bpb.bytes_per_sector == 4096)
        logger_write("INFO", "FAT32 4096-byte logical sectors verified");
    if (!fat32_verify_fat_copies(context)) return 0;
    context->free_cluster_count = FAT32_FSINFO_UNKNOWN;
    context->next_free_cluster = 2;
    context->fsinfo_valid = 0;
    context->fsinfo_dirty = 0;
    fsinfo_sector = g_bpb.fs_info_sector;
    if (fsinfo_sector != 0 && fsinfo_sector < g_bpb.reserved_sectors &&
        (!f32_read_sector(context, fsinfo_sector, sec) ||
         f32_read_u32(sec) != FAT32_FSINFO_LEAD_SIG ||
         f32_read_u32(sec + 484) != FAT32_FSINFO_STR_SIG ||
         f32_read_u32(sec + 508) != FAT32_FSINFO_TRAIL_SIG) &&
        g_bpb.backup_boot_sector != 0 &&
        (UINT32)g_bpb.backup_boot_sector + fsinfo_sector < g_bpb.reserved_sectors) {
        fsinfo_sector += g_bpb.backup_boot_sector;
        (void)f32_read_sector(context, fsinfo_sector, sec);
    }
    if (fsinfo_sector != 0 && fsinfo_sector < g_bpb.reserved_sectors &&
        f32_read_u32(sec) == FAT32_FSINFO_LEAD_SIG &&
        f32_read_u32(sec + 484) == FAT32_FSINFO_STR_SIG &&
        f32_read_u32(sec + 508) == FAT32_FSINFO_TRAIL_SIG) {
        UINT32 maximum_cluster = fat32_max_cluster(context);
        context->free_cluster_count = f32_read_u32(sec + 488);
        context->next_free_cluster = f32_read_u32(sec + 492);
        if (context->free_cluster_count > maximum_cluster - 2)
            context->free_cluster_count = FAT32_FSINFO_UNKNOWN;
        if (context->next_free_cluster < 2 ||
            context->next_free_cluster >= maximum_cluster)
            context->next_free_cluster = 2;
        context->fsinfo_valid = 1;
    }
    g_initialized = 1;
    logger_write("FAT32", "initialized");
    return 1;
}

UINT64 fat32_context_file_size(FAT32_CONTEXT *context, const char *path)
{
    FAT32_DIR_ENTRY entry;
    if (context == 0 || !g_initialized || !fat32_resolve(context, path, &entry, 0)) return 0;
    if (entry.attributes & FAT32_ATTR_DIRECTORY) return 0;
    return entry.file_size;
}

int fat32_context_exists(FAT32_CONTEXT *context, const char *path)
{
    FAT32_DIR_ENTRY entry;
    return context != 0 && g_initialized && fat32_resolve(context, path, &entry, 0);
}

int fat32_context_is_directory(FAT32_CONTEXT *context, const char *path)
{
    FAT32_DIR_ENTRY entry;
    if (context == 0 || !g_initialized || !fat32_resolve(context, path, &entry, 0)) return 0;
    return (entry.attributes & FAT32_ATTR_DIRECTORY) ? 1 : 0;
}

UINT64 fat32_context_read_file(FAT32_CONTEXT *context, const char *path,
                               void *buffer, UINT64 capacity)
{
    FAT32_DIR_ENTRY entry;
    UINT8  sec[FAT32_MAX_SECTOR_SIZE];
    UINT8  *out = (UINT8 *)buffer;
    UINT32 cluster;
    UINT64 remaining;
    UINT64 total = 0;
    UINT32 s;
    UINT32 hops = 0;

    if (context == 0 || !g_initialized || !fat32_resolve(context, path, &entry, 0)) return 0;
    if (entry.attributes & FAT32_ATTR_DIRECTORY) return 0;

    remaining = entry.file_size < capacity ? (UINT64)entry.file_size : capacity;
    cluster   = ((UINT32)entry.first_cluster_hi << 16) | entry.first_cluster_lo;

    while (fat32_data_cluster_valid(context, cluster) && remaining > 0 &&
           hops++ < fat32_max_cluster(context) - 2) {
        UINT32 lba     = cluster_to_lba(context, cluster);
        UINT32 sec_cnt = g_bpb.sectors_per_cluster;

        for (s = 0; s < sec_cnt && remaining > 0; s++) {
            UINT64 chunk;
            if (!f32_read_sector(context, lba + s, sec)) return total;
            chunk = remaining < fat32_sector_size(context) ? remaining :
                    fat32_sector_size(context);
            f32_copy(out + total, sec, (UINT32)chunk);
            total     += chunk;
            remaining -= chunk;
        }
        if (!fat32_chain_advance(context, cluster, &cluster)) return total;
    }
    return total;
}

UINT64 fat32_context_list_directory(FAT32_CONTEXT *context, const char *path,
                                    FAT32_FILE_INFO *entries, UINT64 capacity)
{
    FAT32_DIR_ENTRY dir_entry;
    UINT32 dir_cluster;

    if (context == 0 || !g_initialized ||
        !fat32_resolve(context, path, &dir_entry, &dir_cluster)) return 0;
    if (!(dir_entry.attributes & FAT32_ATTR_DIRECTORY)) return 0;

    dir_cluster = ((UINT32)dir_entry.first_cluster_hi << 16) | dir_entry.first_cluster_lo;
    if (dir_cluster < 2) dir_cluster = g_bpb.root_cluster;

    return list_dir_chain(context, dir_cluster, entries, capacity);
}

static int write_cluster_chain(FAT32_CONTEXT *context, UINT32 first_cluster,
                               const void *buffer, UINT64 size)
{
    UINT8 sector[FAT32_MAX_SECTOR_SIZE];
    const UINT8 *source = (const UINT8 *)buffer;
    UINT32 cluster = first_cluster;
    UINT64 written = 0;
    UINT32 hops = 0;

    while (fat32_data_cluster_valid(context, cluster) && written < size &&
           hops++ < fat32_max_cluster(context) - 2) {
        UINT32 cluster_sector;

        for (cluster_sector = 0; cluster_sector < g_bpb.sectors_per_cluster && written < size; cluster_sector++) {
            UINT64 remaining = size - written;
            UINT32 copy_size = remaining < fat32_sector_size(context) ?
                               (UINT32)remaining : fat32_sector_size(context);

            f32_clear(sector, sizeof(sector));
            f32_copy(sector, source + written, copy_size);
            if (!f32_write_sector(context,
                                  cluster_to_lba(context, cluster) + cluster_sector,
                                  sector)) {
                return 0;
            }
            written += copy_size;
        }
        if (!fat32_chain_advance(context, cluster, &cluster)) return 0;
    }
    return written == size;
}

int fat32_context_write_file(FAT32_CONTEXT *context, const char *path,
                             const void *buffer, UINT64 size)
{
    UINT32 parent_cluster;
    char leaf[256];
    FAT32_LOCATED_ENTRY located;
    FAT32_DIR_ENTRY target;
    int exists;
    UINT32 old_cluster = 0;
    UINT32 new_cluster = 0;
    UINT32 cluster_size = (UINT32)g_bpb.sectors_per_cluster *
                          fat32_sector_size(context);
    UINT32 cluster_count;

    if (context == 0 || !g_initialized || size > 0xFFFFFFFFULL ||
        !fat32_resolve_parent_leaf(context, path, &parent_cluster, leaf)) {
        return 0;
    }

    cluster_count = (UINT32)((size + cluster_size - 1) / cluster_size);
    exists = fat32_find_named_entry(context, parent_cluster, leaf, &located);
    if (exists) {
        if ((located.entry.attributes &
             (FAT32_ATTR_DIRECTORY | FAT32_ATTR_READ_ONLY)) != 0) {
            return 0;
        }
        old_cluster = ((UINT32)located.entry.first_cluster_hi << 16) |
                      located.entry.first_cluster_lo;
        if (old_cluster >= 2 && !fat32_chain_valid(context, old_cluster)) return 0;
    }

    if (cluster_count != 0) {
        new_cluster = fat32_allocate_chain(context, cluster_count);
        if (new_cluster == 0 || !write_cluster_chain(context, new_cluster, buffer, size)) {
            if (new_cluster != 0) (void)fat32_free_chain(context, new_cluster);
            return 0;
        }
    }

    f32_clear(&target, sizeof(target));
    target.attributes = FAT32_ATTR_ARCHIVE;
    target.first_cluster_lo = (UINT16)(new_cluster & 0xFFFFU);
    target.first_cluster_hi = (UINT16)(new_cluster >> 16);
    target.file_size = (UINT32)size;
    if (exists) {
        f32_copy(target.name, located.entry.name, 11);
        target.create_tenths = located.entry.create_tenths;
        target.create_time = located.entry.create_time;
        target.create_date = located.entry.create_date;
    }
    fat32_set_timestamps(&target, !exists);
    if (exists) {
        FAT32_ENTRY_POSITION short_position =
            located.positions[located.position_count - 1U];
        if (!fat32_position_write(context, short_position, &target)) {
            if (new_cluster != 0) (void)fat32_free_chain(context, new_cluster);
            return 0;
        }
    } else if (!fat32_write_named_entry(context, parent_cluster, leaf, &target,
                                        &located)) {
        if (new_cluster != 0) (void)fat32_free_chain(context, new_cluster);
        return 0;
    }
    return old_cluster == 0 || fat32_free_chain(context, old_cluster);
}

int fat32_context_delete_file(FAT32_CONTEXT *context, const char *path)
{
    UINT32 parent_cluster;
    UINT32 cluster;
    char leaf[256];
    FAT32_LOCATED_ENTRY located;

    if (context == 0 || !g_initialized ||
        !fat32_resolve_parent_leaf(context, path, &parent_cluster, leaf) ||
        !fat32_find_named_entry(context, parent_cluster, leaf, &located)) return 0;
    if ((located.entry.attributes &
         (FAT32_ATTR_DIRECTORY | FAT32_ATTR_READ_ONLY)) != 0) return 0;

    cluster = ((UINT32)located.entry.first_cluster_hi << 16) |
              located.entry.first_cluster_lo;
    if (cluster >= 2 && !fat32_chain_valid(context, cluster)) return 0;
    if (!fat32_mark_positions_deleted(context, located.positions,
                                      located.position_count)) return 0;
    return cluster < 2 || fat32_free_chain(context, cluster);
}

static int directory_is_empty(FAT32_CONTEXT *context, UINT32 cluster)
{
    UINT8 sector[FAT32_MAX_SECTOR_SIZE];
    UINT32 entries_per_sec = fat32_sector_size(context) / sizeof(FAT32_DIR_ENTRY);
    UINT32 current = cluster;
    UINT32 s, e;
    UINT32 hops = 0;

    while (fat32_data_cluster_valid(context, current) &&
           hops++ < fat32_max_cluster(context) - 2) {
        UINT32 lba = cluster_to_lba(context, current);

        for (s = 0; s < g_bpb.sectors_per_cluster; s++) {
            if (!f32_read_sector(context, lba + s, sector)) return 0;
            for (e = 0; e < entries_per_sec; e++) {
                FAT32_DIR_ENTRY *entry = (FAT32_DIR_ENTRY *)&sector[e * sizeof(FAT32_DIR_ENTRY)];

                if (entry->name[0] == 0) return 1;
                if ((UINT8)entry->name[0] == 0xE5) continue;
                if ((entry->attributes & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) continue;
                if (entry->name[0] == '.') continue;
                return 0;
            }
        }
        if (!fat32_chain_advance(context, current, &current)) return 0;
    }
    return current >= FAT32_EOC;
}

int fat32_context_delete_directory(FAT32_CONTEXT *context, const char *path)
{
    UINT32 parent_cluster;
    UINT32 cluster;
    char leaf[256];
    FAT32_LOCATED_ENTRY located;

    if (context == 0 || !g_initialized ||
        !fat32_resolve_parent_leaf(context, path, &parent_cluster, leaf) ||
        !fat32_find_named_entry(context, parent_cluster, leaf, &located)) return 0;
    if ((located.entry.attributes & FAT32_ATTR_DIRECTORY) == 0) return 0;

    cluster = ((UINT32)located.entry.first_cluster_hi << 16) |
              located.entry.first_cluster_lo;
    if (cluster < 2 || !fat32_chain_valid(context, cluster) ||
        !directory_is_empty(context, cluster)) return 0;
    if (!fat32_mark_positions_deleted(context, located.positions,
                                      located.position_count)) return 0;
    return fat32_free_chain(context, cluster);
}

int fat32_context_create_directory(FAT32_CONTEXT *context, const char *path)
{
    UINT8 directory_sector[FAT32_MAX_SECTOR_SIZE];
    UINT32 parent_cluster;
    UINT32 cluster;
    char leaf[256];
    FAT32_LOCATED_ENTRY located;
    FAT32_DIR_ENTRY target;

    if (context == 0 || !g_initialized) {
        fat32_set_error("fat32: context not initialized");
        return 0;
    }
    fat32_set_error("fat32: mkdir started");
    if (!fat32_resolve_parent_leaf(context, path, &parent_cluster, leaf)) {
        fat32_set_error("fat32: parent path or leaf name rejected");
        return 0;
    }
    if (fat32_find_named_entry(context, parent_cluster, leaf, &located)) {
        fat32_set_error("fat32: target already exists");
        return 0;
    }

    cluster = fat32_allocate_cluster(context);
    if (cluster == 0) {
        return 0;
    }

    f32_clear(directory_sector, sizeof(directory_sector));
    {
        FAT32_DIR_ENTRY *dot = (FAT32_DIR_ENTRY *)&directory_sector[0];
        FAT32_DIR_ENTRY *dotdot = (FAT32_DIR_ENTRY *)&directory_sector[32];

        f32_clear(dot, sizeof(FAT32_DIR_ENTRY));
        f32_clear(dotdot, sizeof(FAT32_DIR_ENTRY));
        dot->name[0] = '.';
        dot->attributes = FAT32_ATTR_DIRECTORY;
        dot->first_cluster_lo = (UINT16)(cluster & 0xFFFFU);
        dot->first_cluster_hi = (UINT16)(cluster >> 16);
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';
        dotdot->attributes = FAT32_ATTR_DIRECTORY;
        dotdot->first_cluster_lo = (UINT16)(parent_cluster & 0xFFFFU);
        dotdot->first_cluster_hi = (UINT16)(parent_cluster >> 16);
    }
    if (!f32_write_sector(context, cluster_to_lba(context, cluster), directory_sector)) {
        (void)fat32_free_chain(context, cluster);
        fat32_set_error("fat32: new directory cluster write failed");
        return 0;
    }

    f32_clear(&target, sizeof(target));
    target.attributes = FAT32_ATTR_DIRECTORY;
    target.first_cluster_lo = (UINT16)(cluster & 0xFFFFU);
    target.first_cluster_hi = (UINT16)(cluster >> 16);
    fat32_set_timestamps(&target, 1);
    if (!fat32_write_named_entry(context, parent_cluster, leaf, &target,
                                 &located)) {
        (void)fat32_free_chain(context, cluster);
        fat32_set_error("fat32: parent directory entry write failed");
        return 0;
    }
    fat32_set_error("fat32: ok");
    return 1;
}

int fat32_context_rename(FAT32_CONTEXT *context, const char *source,
                         const char *destination)
{
    char source_name[256];
    char destination_name[256];
    UINT32 source_parent;
    UINT32 destination_parent;
    FAT32_LOCATED_ENTRY source_entry;
    FAT32_LOCATED_ENTRY destination_entry;
    FAT32_DIR_ENTRY saved;
    UINT32 cluster;
    if (context == 0 || !g_initialized ||
        !fat32_resolve_parent_leaf(context, source, &source_parent, source_name) ||
        !fat32_resolve_parent_leaf(context, destination, &destination_parent,
                                   destination_name) ||
        !fat32_find_named_entry(context, source_parent, source_name,
                                &source_entry)) return 0;
    if (fat32_find_named_entry(context, destination_parent, destination_name,
                               &destination_entry)) return 0;
    saved = source_entry.entry;
    fat32_set_timestamps(&saved, 0);
    if (!fat32_write_named_entry(context, destination_parent, destination_name,
                                 &saved, &destination_entry)) return 0;

    if ((saved.attributes & FAT32_ATTR_DIRECTORY) != 0 &&
        source_parent != destination_parent) {
        UINT8 directory_sector[FAT32_MAX_SECTOR_SIZE];
        cluster = ((UINT32)saved.first_cluster_hi << 16) | saved.first_cluster_lo;
        if (!fat32_data_cluster_valid(context, cluster) ||
            !f32_read_sector(context, cluster_to_lba(context, cluster), directory_sector)) return 0;
        ((FAT32_DIR_ENTRY *)(directory_sector + 32))->first_cluster_lo =
            (UINT16)(destination_parent & 0xFFFFU);
        ((FAT32_DIR_ENTRY *)(directory_sector + 32))->first_cluster_hi =
            (UINT16)(destination_parent >> 16);
        if (!f32_write_sector(context, cluster_to_lba(context, cluster),
                              directory_sector)) return 0;
    }
    return fat32_mark_positions_deleted(context, source_entry.positions,
                                        source_entry.position_count);
}

int fat32_detect(void) { return fat32_detect_context(&legacy_context); }
int fat32_initialize(void) { return fat32_initialize_context(&legacy_context); }
UINT64 fat32_file_size(const char *path)
{
    return fat32_context_file_size(&legacy_context, path);
}
int fat32_exists(const char *path)
{
    return fat32_context_exists(&legacy_context, path);
}
int fat32_is_directory(const char *path)
{
    return fat32_context_is_directory(&legacy_context, path);
}
UINT64 fat32_read_file(const char *path, void *buffer, UINT64 capacity)
{
    return fat32_context_read_file(&legacy_context, path, buffer, capacity);
}
UINT64 fat32_list_directory(const char *path, FAT32_FILE_INFO *entries,
                            UINT64 capacity)
{
    return fat32_context_list_directory(&legacy_context, path, entries, capacity);
}
int fat32_write_file(const char *path, const void *buffer, UINT64 size)
{
    return fat32_context_write_file(&legacy_context, path, buffer, size);
}
int fat32_delete_file(const char *path)
{
    return fat32_context_delete_file(&legacy_context, path);
}
int fat32_create_directory(const char *path)
{
    return fat32_context_create_directory(&legacy_context, path);
}
int fat32_delete_directory(const char *path)
{
    return fat32_context_delete_directory(&legacy_context, path);
}

/* ---- Multi-device mount ---- */
static UINT8 fat32_mounted_target = 0xFFU;
static UINT8 fat32_mounted_lun    = 0xFFU;

int fat32_mount_device(UINT8 target, UINT8 lun)
{
    legacy_context.device = 0;
    if (target == fat32_mounted_target && lun == fat32_mounted_lun) {
        return legacy_context.initialized;
    }
    virtio_block_select_device(target, lun);
    if (!fat32_initialize_context(&legacy_context)) {
        return 0;
    }
    fat32_mounted_target = target;
    fat32_mounted_lun    = lun;
    return 1;
}

FAT32_CONTEXT *fat32_context_create(ASAS_BLOCK_DEVICE *device)
{
    FAT32_CONTEXT *context;
    if (device == 0 || (device->logical_block_size != 512 &&
                        device->logical_block_size != 1024 &&
                        device->logical_block_size != 2048 &&
                        device->logical_block_size != 4096)) return 0;
    context = (FAT32_CONTEXT *)kmalloc(sizeof(FAT32_CONTEXT));
    if (context == 0) return 0;
    f32_clear(context, sizeof(FAT32_CONTEXT));
    context->device = device;
    if (!fat32_initialize_context(context)) {
        kfree(context);
        return 0;
    }
    return context;
}

void fat32_context_destroy(FAT32_CONTEXT *context)
{
    if (context == 0 || context == &legacy_context) return;
    kfree(context);
}

int fat32_context_sync(FAT32_CONTEXT *context)
{
    UINT8 sector[FAT32_MAX_SECTOR_SIZE];
    UINT32 backup_sector;
    if (context == 0 || !context->initialized) return 0;
    if (context->fsinfo_valid && context->fsinfo_dirty) {
        if (!f32_read_sector(context, context->bpb.fs_info_sector, sector)) return 0;
        f32_write_u32(sector + 488, context->free_cluster_count);
        f32_write_u32(sector + 492, context->next_free_cluster);
        if (!f32_write_sector(context, context->bpb.fs_info_sector, sector)) return 0;
        backup_sector = (UINT32)context->bpb.backup_boot_sector +
                        context->bpb.fs_info_sector;
        if (context->bpb.backup_boot_sector != 0 &&
            backup_sector < context->bpb.reserved_sectors &&
            !f32_write_sector(context, backup_sector, sector)) return 0;
        context->fsinfo_dirty = 0;
    }
    return context->device == 0 ? 1 : block_device_flush(context->device);
}
