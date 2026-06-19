#include "partition.h"
#include "filesystem.h"
#include "heap.h"

#pragma pack(push, 1)
typedef struct {
    UINT8 status;
    UINT8 first_chs[3];
    UINT8 type;
    UINT8 last_chs[3];
    UINT32 first_lba;
    UINT32 sector_count;
} MBR_PARTITION_ENTRY;

typedef struct {
    UINT8 signature[8];
    UINT32 revision;
    UINT32 header_size;
    UINT32 header_crc32;
    UINT32 reserved;
    UINT64 current_lba;
    UINT64 backup_lba;
    UINT64 first_usable_lba;
    UINT64 last_usable_lba;
    UINT8 disk_guid[16];
    UINT64 entries_lba;
    UINT32 entry_count;
    UINT32 entry_size;
    UINT32 entries_crc32;
} GPT_HEADER;

typedef struct {
    UINT8 type_guid[16];
    UINT8 unique_guid[16];
    UINT64 first_lba;
    UINT64 last_lba;
    UINT64 attributes;
    UINT16 name[36];
} GPT_ENTRY;
#pragma pack(pop)

static ASAS_PARTITION_INFO partitions[BLOCK_DEVICE_MAX_COUNT];
static UINT32 found_partition_count;
#define PARTITION_MAX_BLOCK_SIZE 4096U

typedef struct {
    UINT32 number;
    UINT8 type_guid[16];
    UINT8 unique_guid[16];
    UINT64 start_lba;
    UINT64 block_count;
    UINT16 name[36];
} GPT_CANDIDATE;

typedef struct {
    UINT32 number;
    UINT8 type;
    UINT64 start_lba;
    UINT64 block_count;
} MBR_CANDIDATE;

static void copy_bytes(UINT8 *destination, const UINT8 *source, UINT32 size)
{
    UINT32 index;
    for (index = 0; index < size; index++) destination[index] = source[index];
}

static int bytes_equal(const UINT8 *left, const char *right, UINT32 size)
{
    UINT32 index;
    for (index = 0; index < size; index++) {
        if (left[index] != (UINT8)right[index]) return 0;
    }
    return 1;
}

static int guid_is_zero(const UINT8 guid[16])
{
    UINT32 index;
    for (index = 0; index < 16; index++) if (guid[index] != 0) return 0;
    return 1;
}

static int guid_equal(const UINT8 left[16], const UINT8 right[16])
{
    UINT32 index;
    for (index = 0; index < 16; index++) if (left[index] != right[index]) return 0;
    return 1;
}

static void copy_string(char *destination, const char *source, UINT32 capacity)
{
    UINT32 index = 0;
    while (index + 1 < capacity && source[index] != '\0') {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

static ASAS_PARTITION_TYPE classify_gpt_type(const UINT8 guid[16],
                                              char name[24])
{
    static const UINT8 efi[16] =
        {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
    static const UINT8 ms_reserved[16] =
        {0x16,0xE3,0xC9,0xE3,0x5C,0x0B,0xB8,0x4D,0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE};
    static const UINT8 ms_basic[16] =
        {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
    static const UINT8 recovery[16] =
        {0xA4,0xBB,0x94,0xDE,0xD1,0x06,0x40,0x4D,0xA1,0x6A,0xBF,0xD5,0x01,0x79,0xD6,0xAC};
    static const UINT8 linux_fs[16] =
        {0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4};
    static const UINT8 linux_swap[16] =
        {0x6D,0xFD,0x57,0x06,0xAB,0xA4,0xC4,0x43,0x84,0xE5,0x09,0x33,0xC8,0x4B,0x4F,0x4F};
    if (guid_equal(guid, efi)) { copy_string(name, "EFI System", 24); return PARTITION_TYPE_EFI_SYSTEM; }
    if (guid_equal(guid, ms_reserved)) { copy_string(name, "Microsoft Reserved", 24); return PARTITION_TYPE_MICROSOFT_RESERVED; }
    if (guid_equal(guid, ms_basic)) { copy_string(name, "Microsoft Basic Data", 24); return PARTITION_TYPE_MICROSOFT_BASIC_DATA; }
    if (guid_equal(guid, recovery)) { copy_string(name, "Windows Recovery", 24); return PARTITION_TYPE_WINDOWS_RECOVERY; }
    if (guid_equal(guid, linux_fs)) { copy_string(name, "Linux Filesystem", 24); return PARTITION_TYPE_LINUX_FILESYSTEM; }
    if (guid_equal(guid, linux_swap)) { copy_string(name, "Linux Swap", 24); return PARTITION_TYPE_LINUX_SWAP; }
    copy_string(name, "Unknown GPT", 24);
    return PARTITION_TYPE_UNKNOWN;
}

static char hex_digit(UINT8 value)
{
    return value < 10 ? (char)('0' + value) : (char)('a' + value - 10);
}

static void guid_to_string(const UINT8 guid[16], char output[37])
{
    static const UINT8 order[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    UINT32 index;
    UINT32 out = 0;
    for (index = 0; index < 16; index++) {
        UINT8 value = guid[order[index]];
        if (index == 4 || index == 6 || index == 8 || index == 10) output[out++] = '-';
        output[out++] = hex_digit((UINT8)(value >> 4));
        output[out++] = hex_digit((UINT8)(value & 0x0F));
    }
    output[out] = '\0';
}

static void utf16_to_utf8(const UINT16 input[36], char output[64])
{
    UINT32 in = 0;
    UINT32 out = 0;
    while (in < 36 && input[in] != 0 && out + 1 < 64) {
        UINT32 codepoint = input[in++];
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU && in < 36 &&
            input[in] >= 0xDC00U && input[in] <= 0xDFFFU) {
            codepoint = 0x10000U + ((codepoint - 0xD800U) << 10) +
                        (input[in++] - 0xDC00U);
        }
        if (codepoint < 0x80U && out + 1 < 64) output[out++] = (char)codepoint;
        else if (codepoint < 0x800U && out + 2 < 64) {
            output[out++] = (char)(0xC0U | (codepoint >> 6));
            output[out++] = (char)(0x80U | (codepoint & 0x3FU));
        } else if (codepoint < 0x10000U && out + 3 < 64) {
            output[out++] = (char)(0xE0U | (codepoint >> 12));
            output[out++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            output[out++] = (char)(0x80U | (codepoint & 0x3FU));
        } else if (codepoint <= 0x10FFFFU && out + 4 < 64) {
            output[out++] = (char)(0xF0U | (codepoint >> 18));
            output[out++] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
            output[out++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            output[out++] = (char)(0x80U | (codepoint & 0x3FU));
        }
    }
    output[out] = '\0';
}

static int mbr_type_is_extended(UINT8 type)
{
    return type == 0x05 || type == 0x0F || type == 0x85;
}

static int mbr_candidate_add(MBR_CANDIDATE *candidates, UINT32 *count,
                             UINT32 number, UINT8 type, UINT64 start_lba,
                             UINT64 block_count, UINT64 device_blocks)
{
    UINT32 index;
    UINT64 end_lba;
    if (*count >= PARTITION_MAX_PER_DISK || start_lba == 0 || block_count == 0 ||
        (device_blocks != 0 &&
         (start_lba >= device_blocks || block_count > device_blocks - start_lba))) return 0;
    end_lba = start_lba + block_count - 1;
    for (index = 0; index < *count; index++) {
        UINT64 existing_end = candidates[index].start_lba +
                              candidates[index].block_count - 1;
        if (start_lba <= existing_end && candidates[index].start_lba <= end_lba) return 0;
    }
    candidates[*count].number = number;
    candidates[*count].type = type;
    candidates[*count].start_lba = start_lba;
    candidates[*count].block_count = block_count;
    (*count)++;
    return 1;
}

static UINT32 crc32_update(UINT32 crc, const UINT8 *data, UINT32 size)
{
    UINT32 index;
    for (index = 0; index < size; index++) {
        UINT32 bit;
        crc ^= data[index];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

static void make_partition_name(const ASAS_BLOCK_DEVICE *device,
                                UINT32 number, char output[16])
{
    UINT32 input = 0;
    UINT32 output_index = 0;
    while (device->name[input] && output_index + 1 < 16) {
        output[output_index++] = device->name[input++];
    }
    if (output_index + 2 < 16) output[output_index++] = 'p';
    if (number >= 10 && output_index + 1 < 16) {
        output[output_index++] = (char)('0' + ((number / 10) % 10));
    }
    if (output_index + 1 < 16) output[output_index++] = (char)('0' + (number % 10));
    output[output_index] = '\0';
}

static int record_partition(ASAS_BLOCK_DEVICE *parent,
                            ASAS_PARTITION_SCHEME scheme, UINT32 number,
                            UINT8 type, const UINT8 *type_guid,
                            const UINT8 *unique_guid, const UINT16 *gpt_name,
                            UINT64 start_lba, UINT64 block_count)
{
    ASAS_PARTITION_INFO *info;
    char name[16];
    UINT32 index;
    if (found_partition_count >= BLOCK_DEVICE_MAX_COUNT || block_count == 0 ||
        start_lba == 0 ||
        (parent->block_count != 0 &&
         (start_lba >= parent->block_count ||
          block_count > parent->block_count - start_lba))) return 0;
    for (index = 0; index < found_partition_count; index++) {
        const ASAS_PARTITION_INFO *existing = &partitions[index];
        UINT64 end_lba = start_lba + block_count - 1;
        UINT64 existing_end;
        if (existing->parent != parent) continue;
        existing_end = existing->start_lba + existing->block_count - 1;
        if (start_lba <= existing_end && existing->start_lba <= end_lba) return 0;
    }
    make_partition_name(parent, number, name);
    info = &partitions[found_partition_count];
    info->device = block_device_register_partition(parent, start_lba, block_count,
                                                   name, 0);
    if (info->device == 0) return 0;
    info->parent = parent;
    info->scheme = scheme;
    info->number = number;
    info->type = type;
    if (type_guid != 0) copy_bytes(info->type_guid, type_guid, 16);
    else copy_bytes(info->type_guid, (const UINT8 *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
    if (unique_guid != 0) copy_bytes(info->unique_guid, unique_guid, 16);
    else copy_bytes(info->unique_guid, (const UINT8 *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
    info->label[0] = '\0';
    info->uuid[0] = '\0';
    if (scheme == PARTITION_SCHEME_GPT) {
        info->known_type = classify_gpt_type(info->type_guid, info->type_name);
        guid_to_string(info->unique_guid, info->uuid);
        if (gpt_name != 0) utf16_to_utf8(gpt_name, info->label);
    } else {
        info->known_type = PARTITION_TYPE_UNKNOWN;
        if (type == 0x07) copy_string(info->type_name, "NTFS/exFAT", sizeof(info->type_name));
        else if (type == 0x0B || type == 0x0C) copy_string(info->type_name, "FAT32", sizeof(info->type_name));
        else if (type == 0x82) copy_string(info->type_name, "Linux Swap", sizeof(info->type_name));
        else if (type == 0x83) copy_string(info->type_name, "Linux Filesystem", sizeof(info->type_name));
        else copy_string(info->type_name, "Unknown MBR", sizeof(info->type_name));
    }
    info->start_lba = start_lba;
    info->block_count = block_count;
    found_partition_count++;
    return 1;
}

static int read_gpt_header(ASAS_BLOCK_DEVICE *device, UINT64 header_lba,
                           UINT8 header_sector[PARTITION_MAX_BLOCK_SIZE])
{
    UINT8 header_copy[PARTITION_MAX_BLOCK_SIZE];
    GPT_HEADER *header = (GPT_HEADER *)header_sector;
    UINT32 block_size = device->logical_block_size;
    UINT32 original_crc;
    UINT64 entries_bytes;
    UINT64 entries_sectors;

    if (block_size < 512 || block_size > PARTITION_MAX_BLOCK_SIZE ||
        !block_device_read(device, header_lba, 1, header_sector) ||
        !bytes_equal(header->signature, "EFI PART", 8) ||
        header->header_size < 92 || header->header_size > block_size ||
        header->entry_size < 128 || header->entry_size > block_size ||
        (header->entry_size % 128) != 0 ||
        (block_size % header->entry_size) != 0 ||
        header->entry_count == 0 || header->current_lba != header_lba ||
        header->backup_lba == header_lba ||
        header->first_usable_lba > header->last_usable_lba) return 0;

    if (device->block_count != 0 &&
        (header_lba >= device->block_count ||
         header->backup_lba >= device->block_count ||
         header->last_usable_lba >= device->block_count)) return 0;
    entries_bytes = (UINT64)header->entry_count * header->entry_size;
    if (entries_bytes / header->entry_size != header->entry_count) return 0;
    entries_sectors = (entries_bytes + block_size - 1U) / block_size;
    if (entries_sectors == 0 || header->entries_lba == 0 ||
        (device->block_count != 0 &&
         (header->entries_lba >= device->block_count ||
          entries_sectors > device->block_count - header->entries_lba))) return 0;
    if (header->entries_lba <= header->last_usable_lba &&
        header->entries_lba + entries_sectors - 1 >= header->first_usable_lba) return 0;
    if ((header_lba >= header->first_usable_lba &&
         header_lba <= header->last_usable_lba) ||
        (header->backup_lba >= header->first_usable_lba &&
         header->backup_lba <= header->last_usable_lba)) return 0;

    copy_bytes(header_copy, header_sector, block_size);
    original_crc = ((GPT_HEADER *)header_copy)->header_crc32;
    ((GPT_HEADER *)header_copy)->header_crc32 = 0;
    if ((crc32_update(0xFFFFFFFFU, header_copy, header->header_size) ^ 0xFFFFFFFFU) != original_crc) {
        return 0;
    }
    return 1;
}

static int scan_gpt_at(ASAS_BLOCK_DEVICE *device, UINT64 header_lba)
{
    UINT8 header_sector[PARTITION_MAX_BLOCK_SIZE];
    UINT8 entry_sector[PARTITION_MAX_BLOCK_SIZE];
    GPT_HEADER *header = (GPT_HEADER *)header_sector;
    GPT_CANDIDATE candidates[PARTITION_MAX_PER_DISK];
    UINT32 candidate_count = 0;
    UINT32 index;
    UINT32 entries_crc = 0xFFFFFFFFU;
    UINT32 current_sector = 0xFFFFFFFFU;

    if (!read_gpt_header(device, header_lba, header_sector)) return 0;

    for (index = 0; index < header->entry_count; index++) {
        UINT64 byte_offset = (UINT64)index * header->entry_size;
        UINT32 sector_index =
            (UINT32)(byte_offset / device->logical_block_size);
        UINT32 offset =
            (UINT32)(byte_offset % device->logical_block_size);
        GPT_ENTRY *entry;
        UINT32 candidate_index;

        if (sector_index != current_sector) {
            if (!block_device_read(device, header->entries_lba + sector_index,
                                   1, entry_sector)) return 0;
            current_sector = sector_index;
        }
        if (offset + header->entry_size > device->logical_block_size) return 0;
        entries_crc = crc32_update(entries_crc, entry_sector + offset,
                                   header->entry_size);
        entry = (GPT_ENTRY *)(entry_sector + offset);
        if (guid_is_zero(entry->type_guid)) continue;
        if (candidate_count >= PARTITION_MAX_PER_DISK ||
            entry->first_lba < header->first_usable_lba ||
            entry->last_lba > header->last_usable_lba ||
            entry->last_lba < entry->first_lba) return 0;
        for (candidate_index = 0; candidate_index < candidate_count;
             candidate_index++) {
            UINT64 candidate_end = candidates[candidate_index].start_lba +
                                   candidates[candidate_index].block_count - 1;
            if (entry->first_lba <= candidate_end &&
                candidates[candidate_index].start_lba <= entry->last_lba) return 0;
        }
        candidates[candidate_count].number = index + 1;
        copy_bytes(candidates[candidate_count].type_guid, entry->type_guid, 16);
        copy_bytes(candidates[candidate_count].unique_guid, entry->unique_guid, 16);
        copy_bytes((UINT8 *)candidates[candidate_count].name,
                   (const UINT8 *)entry->name, sizeof(entry->name));
        candidates[candidate_count].start_lba = entry->first_lba;
        candidates[candidate_count].block_count =
            entry->last_lba - entry->first_lba + 1;
        candidate_count++;
    }

    /* Full-array CRC is validated when the supported entry layout fits the
     * current sector reader. Unsupported oversized layouts fail closed above. */
    entries_crc ^= 0xFFFFFFFFU;
    if (entries_crc != header->entries_crc32) return 0;
    for (index = 0; index < candidate_count; index++) {
        if (!record_partition(device, PARTITION_SCHEME_GPT,
                              candidates[index].number, 0,
                              candidates[index].type_guid,
                              candidates[index].unique_guid,
                              candidates[index].name,
                              candidates[index].start_lba,
                              candidates[index].block_count)) return 0;
    }
    return (int)candidate_count;
}

static int scan_gpt(ASAS_BLOCK_DEVICE *device)
{
    int result = scan_gpt_at(device, 1);
    if (result != 0 || device->block_count < 2) return result;
    return scan_gpt_at(device, device->block_count - 1);
}

static int scan_mbr(ASAS_BLOCK_DEVICE *device, const UINT8 *sector)
{
    const MBR_PARTITION_ENTRY *entries =
        (const MBR_PARTITION_ENTRY *)(sector + 446);
    MBR_CANDIDATE candidates[PARTITION_MAX_PER_DISK];
    UINT32 candidate_count = 0;
    UINT64 extended_base = 0;
    UINT64 extended_count = 0;
    UINT32 index;
    for (index = 0; index < 4; index++) {
        UINT64 start_lba;
        UINT64 block_count;
        if (entries[index].type == 0 || entries[index].sector_count == 0 ||
            entries[index].type == 0xEE) continue;
        start_lba = entries[index].first_lba;
        block_count = entries[index].sector_count;
        if (mbr_type_is_extended(entries[index].type)) {
            if (extended_base != 0 || start_lba == 0 || block_count == 0 ||
                (device->block_count != 0 &&
                 (start_lba >= device->block_count ||
                  block_count > device->block_count - start_lba))) return 0;
            extended_base = start_lba;
            extended_count = block_count;
            continue;
        }
        if (!mbr_candidate_add(candidates, &candidate_count, index + 1,
                               entries[index].type, start_lba, block_count,
                               device->block_count)) return 0;
    }
    if (extended_base != 0) {
        UINT8 ebr_sector[PARTITION_MAX_BLOCK_SIZE];
        UINT64 ebr_lba = extended_base;
        UINT64 visited[PARTITION_MAX_PER_DISK];
        UINT32 visited_count = 0;
        UINT32 logical_number = 5;
        UINT64 extended_end = extended_base + extended_count - 1;
        for (index = 0; index < candidate_count; index++) {
            UINT64 primary_end = candidates[index].start_lba +
                                 candidates[index].block_count - 1;
            if (extended_base <= primary_end &&
                candidates[index].start_lba <= extended_end) return 0;
        }
        while (ebr_lba != 0) {
            const MBR_PARTITION_ENTRY *ebr_entries;
            UINT64 logical_start;
            UINT64 next_ebr = 0;
            UINT32 visited_index;
            if (visited_count >= PARTITION_MAX_PER_DISK ||
                ebr_lba < extended_base || ebr_lba > extended_end ||
                !block_device_read(device, ebr_lba, 1, ebr_sector) ||
                ebr_sector[510] != 0x55 || ebr_sector[511] != 0xAA) return 0;
            for (visited_index = 0; visited_index < visited_count; visited_index++) {
                if (visited[visited_index] == ebr_lba) return 0;
            }
            visited[visited_count++] = ebr_lba;
            ebr_entries = (const MBR_PARTITION_ENTRY *)(ebr_sector + 446);
            if (ebr_entries[0].type == 0 || ebr_entries[0].sector_count == 0 ||
                mbr_type_is_extended(ebr_entries[0].type)) return 0;
            logical_start = ebr_lba + ebr_entries[0].first_lba;
            if (logical_start < extended_base ||
                logical_start > extended_end ||
                ebr_entries[0].sector_count > extended_end - logical_start + 1 ||
                !mbr_candidate_add(candidates, &candidate_count, logical_number++,
                                   ebr_entries[0].type, logical_start,
                                   ebr_entries[0].sector_count,
                                   device->block_count)) return 0;
            if (ebr_entries[1].type != 0 || ebr_entries[1].sector_count != 0) {
                if (!mbr_type_is_extended(ebr_entries[1].type) ||
                    ebr_entries[1].first_lba == 0) return 0;
                next_ebr = extended_base + ebr_entries[1].first_lba;
                if (next_ebr < extended_base || next_ebr > extended_end) return 0;
            }
            ebr_lba = next_ebr;
        }
    }
    for (index = 0; index < candidate_count; index++) {
        if (!record_partition(device, PARTITION_SCHEME_MBR,
                              candidates[index].number,
                              candidates[index].type, 0, 0, 0,
                              candidates[index].start_lba,
                              candidates[index].block_count)) return 0;
    }
    return (int)candidate_count;
}

void partition_manager_initialize(void)
{
    found_partition_count = 0;
}

int partition_scan_device(ASAS_BLOCK_DEVICE *device)
{
    UINT8 sector[PARTITION_MAX_BLOCK_SIZE];
    const MBR_PARTITION_ENTRY *entries;
    UINT32 index;
    if (device == 0 || device->parent != 0 ||
        device->logical_block_size < 512 ||
        device->logical_block_size > PARTITION_MAX_BLOCK_SIZE ||
        !block_device_read(device, 0, 1, sector) ||
        sector[510] != 0x55 || sector[511] != 0xAA) return 0;

    entries = (const MBR_PARTITION_ENTRY *)(sector + 446);
    for (index = 0; index < 4; index++) {
        if (entries[index].type == 0xEE) return scan_gpt(device);
    }
    return scan_mbr(device, sector);
}

int partition_scan_all(void)
{
    UINT32 physical_count = block_device_count();
    UINT32 index;
    int found = 0;
    for (index = 0; index < physical_count; index++) {
        ASAS_BLOCK_DEVICE *device = block_device_get(index);
        if (device != 0 && device->parent == 0 &&
            (device->flags & BLOCK_DEVICE_FLAG_OPTICAL) == 0) {
            found += partition_scan_device(device);
        }
    }
    return found;
}

UINT32 partition_count(void) { return found_partition_count; }

const ASAS_PARTITION_INFO *partition_get(UINT32 index)
{
    return index < found_partition_count ? &partitions[index] : 0;
}

const ASAS_PARTITION_INFO *partition_find_by_device(const ASAS_BLOCK_DEVICE *device)
{
    UINT32 index;
    for (index = 0; index < found_partition_count; index++) {
        if (partitions[index].device == device) return &partitions[index];
    }
    return 0;
}

static int mbr_table_valid(const MBR_PARTITION_ENTRY entries[4],
                           UINT64 device_blocks)
{
    MBR_CANDIDATE candidates[4];
    UINT32 count = 0;
    UINT32 index;
    for (index = 0; index < 4; index++) {
        if (entries[index].type == 0 && entries[index].sector_count == 0) continue;
        if (entries[index].type == 0 || entries[index].sector_count == 0 ||
            entries[index].type == 0xEE || mbr_type_is_extended(entries[index].type) ||
            !mbr_candidate_add(candidates, &count, index + 1, entries[index].type,
                               entries[index].first_lba, entries[index].sector_count,
                               device_blocks)) return 0;
    }
    return 1;
}

static int partition_mbr_commit(ASAS_BLOCK_DEVICE *device, UINT32 slot,
                                UINT8 type, UINT64 start_lba,
                                UINT64 block_count, int deleting)
{
    UINT8 sector[PARTITION_MAX_BLOCK_SIZE];
    MBR_PARTITION_ENTRY entries[4];
    MBR_PARTITION_ENTRY *on_disk;
    UINT32 index;
    if (device == 0 || device->parent != 0 || slot >= 4 ||
        device->logical_block_size < 512 ||
        device->logical_block_size > PARTITION_MAX_BLOCK_SIZE ||
        block_device_has_capability(device, BLOCK_DEVICE_FLAG_READ_ONLY) ||
        filesystem_device_is_mounted(device) ||
        !block_device_read(device, 0, 1, sector) ||
        sector[510] != 0x55 || sector[511] != 0xAA) return 0;
    on_disk = (MBR_PARTITION_ENTRY *)(sector + 446);
    for (index = 0; index < 4; index++) entries[index] = on_disk[index];
    for (index = 0; index < 4; index++) if (entries[index].type == 0xEE) return 0;

    if (deleting) {
        if (entries[slot].type == 0) return 0;
        for (index = 0; index < sizeof(MBR_PARTITION_ENTRY); index++)
            ((UINT8 *)&entries[slot])[index] = 0;
    } else {
        if (type == 0 || type == 0xEE || mbr_type_is_extended(type) ||
            start_lba == 0 || block_count == 0 ||
            start_lba > 0xFFFFFFFFULL || block_count > 0xFFFFFFFFULL) return 0;
        entries[slot].status = 0;
        entries[slot].type = type;
        entries[slot].first_lba = (UINT32)start_lba;
        entries[slot].sector_count = (UINT32)block_count;
        for (index = 0; index < 3; index++) {
            entries[slot].first_chs[index] = 0;
            entries[slot].last_chs[index] = 0;
        }
    }
    if (!mbr_table_valid(entries, device->block_count)) return 0;
    for (index = 0; index < 4; index++) on_disk[index] = entries[index];
    return block_device_write(device, 0, 1, sector) && block_device_flush(device);
}

int partition_mbr_create(ASAS_BLOCK_DEVICE *device, UINT32 slot, UINT8 type,
                         UINT64 start_lba, UINT64 block_count)
{
    UINT8 sector[PARTITION_MAX_BLOCK_SIZE];
    MBR_PARTITION_ENTRY *entries;
    if (device == 0 || slot >= 4 || !block_device_read(device, 0, 1, sector)) return 0;
    entries = (MBR_PARTITION_ENTRY *)(sector + 446);
    if (entries[slot].type != 0 || entries[slot].sector_count != 0) return 0;
    return partition_mbr_commit(device, slot, type, start_lba, block_count, 0);
}

int partition_mbr_delete(ASAS_BLOCK_DEVICE *device, UINT32 slot)
{
    return partition_mbr_commit(device, slot, 0, 0, 0, 1);
}

int partition_mbr_resize(ASAS_BLOCK_DEVICE *device, UINT32 slot,
                         UINT64 start_lba, UINT64 block_count)
{
    UINT8 sector[PARTITION_MAX_BLOCK_SIZE];
    MBR_PARTITION_ENTRY *entries;
    if (device == 0 || slot >= 4 || !block_device_read(device, 0, 1, sector)) return 0;
    entries = (MBR_PARTITION_ENTRY *)(sector + 446);
    if (entries[slot].type == 0 || entries[slot].sector_count == 0) return 0;
    return partition_mbr_commit(device, slot, entries[slot].type,
                                start_lba, block_count, 0);
}

static int gpt_layouts_match(const GPT_HEADER *primary,
                             const GPT_HEADER *backup)
{
    return primary->current_lba == 1 &&
           primary->backup_lba == backup->current_lba &&
           backup->backup_lba == primary->current_lba &&
           primary->first_usable_lba == backup->first_usable_lba &&
           primary->last_usable_lba == backup->last_usable_lba &&
           primary->entry_count == backup->entry_count &&
           primary->entry_size == backup->entry_size &&
           guid_equal(primary->disk_guid, backup->disk_guid);
}

static int gpt_entries_valid(const GPT_HEADER *header, const UINT8 *entries)
{
    UINT32 index;
    for (index = 0; index < header->entry_count; index++) {
        const GPT_ENTRY *entry = (const GPT_ENTRY *)(entries +
            (UINT64)index * header->entry_size);
        UINT32 other;
        if (guid_is_zero(entry->type_guid)) continue;
        if (entry->first_lba < header->first_usable_lba ||
            entry->last_lba > header->last_usable_lba ||
            entry->last_lba < entry->first_lba) return 0;
        for (other = 0; other < index; other++) {
            const GPT_ENTRY *existing = (const GPT_ENTRY *)(entries +
                (UINT64)other * header->entry_size);
            if (!guid_is_zero(existing->type_guid) &&
                entry->first_lba <= existing->last_lba &&
                existing->first_lba <= entry->last_lba) return 0;
        }
    }
    return 1;
}

static void gpt_update_header_crc(UINT8 *sector, UINT32 entries_crc)
{
    GPT_HEADER *header = (GPT_HEADER *)sector;
    header->entries_crc32 = entries_crc;
    header->header_crc32 = 0;
    header->header_crc32 =
        crc32_update(0xFFFFFFFFU, sector, header->header_size) ^ 0xFFFFFFFFU;
}

static int partition_gpt_commit(ASAS_BLOCK_DEVICE *device, UINT32 slot,
                                const UINT8 type_guid[16],
                                const UINT8 unique_guid[16],
                                const UINT16 name[36], UINT64 start_lba,
                                UINT64 block_count, int operation)
{
    UINT8 primary_sector[PARTITION_MAX_BLOCK_SIZE];
    UINT8 backup_sector[PARTITION_MAX_BLOCK_SIZE];
    UINT8 original_primary_sector[PARTITION_MAX_BLOCK_SIZE];
    UINT8 original_backup_sector[PARTITION_MAX_BLOCK_SIZE];
    GPT_HEADER *primary = (GPT_HEADER *)primary_sector;
    GPT_HEADER *backup = (GPT_HEADER *)backup_sector;
    UINT8 *entries = 0;
    UINT8 *original_entries = 0;
    UINT64 entries_bytes;
    UINT32 entries_sectors;
    UINT32 crc;
    GPT_ENTRY *entry;
    UINT32 index;
    int result = 0;

    if (device == 0 || device->parent != 0 ||
        device->logical_block_size < 512 ||
        device->logical_block_size > PARTITION_MAX_BLOCK_SIZE ||
        block_device_has_capability(device, BLOCK_DEVICE_FLAG_READ_ONLY) ||
        filesystem_device_is_mounted(device) ||
        !read_gpt_header(device, 1, primary_sector) ||
        !read_gpt_header(device, primary->backup_lba, backup_sector) ||
        !gpt_layouts_match(primary, backup) || slot >= primary->entry_count) return 0;
    entries_bytes = (UINT64)primary->entry_count * primary->entry_size;
    entries_sectors = (UINT32)((entries_bytes +
        device->logical_block_size - 1U) / device->logical_block_size);
    if (entries_bytes == 0 || entries_bytes > 128U * 1024U) return 0;
    entries = (UINT8 *)kmalloc((UINTN)entries_sectors *
                               device->logical_block_size);
    original_entries = (UINT8 *)kmalloc((UINTN)entries_sectors *
                                        device->logical_block_size);
    if (entries == 0 || original_entries == 0) goto cleanup;
    if (!block_device_read(device, primary->entries_lba, entries_sectors, entries) ||
        !block_device_read(device, backup->entries_lba, entries_sectors,
                           original_entries)) goto cleanup;
    for (index = 0; index < entries_bytes; index++) {
        if (entries[index] != original_entries[index]) goto cleanup;
    }
    crc = crc32_update(0xFFFFFFFFU, entries, (UINT32)entries_bytes) ^ 0xFFFFFFFFU;
    if (crc != primary->entries_crc32 || crc != backup->entries_crc32) goto cleanup;
    copy_bytes(original_entries, entries, entries_sectors *
               device->logical_block_size);
    copy_bytes(original_primary_sector, primary_sector,
               device->logical_block_size);
    copy_bytes(original_backup_sector, backup_sector,
               device->logical_block_size);
    entry = (GPT_ENTRY *)(entries + (UINT64)slot * primary->entry_size);

    if (operation == 0) {
        if (!guid_is_zero(entry->type_guid) || type_guid == 0 ||
            unique_guid == 0 || guid_is_zero(type_guid) ||
            guid_is_zero(unique_guid) || block_count == 0 ||
            block_count - 1U > ~0ULL - start_lba) goto cleanup;
        for (index = 0; index < primary->entry_size; index++)
            ((UINT8 *)entry)[index] = 0;
        copy_bytes(entry->type_guid, type_guid, 16);
        copy_bytes(entry->unique_guid, unique_guid, 16);
        if (name != 0) copy_bytes((UINT8 *)entry->name, (const UINT8 *)name,
                                  sizeof(entry->name));
        entry->first_lba = start_lba;
        entry->last_lba = start_lba + block_count - 1;
    } else if (operation == 1) {
        if (guid_is_zero(entry->type_guid)) goto cleanup;
        for (index = 0; index < primary->entry_size; index++)
            ((UINT8 *)entry)[index] = 0;
    } else {
        if (guid_is_zero(entry->type_guid) || block_count == 0 ||
            block_count - 1U > ~0ULL - start_lba) goto cleanup;
        entry->first_lba = start_lba;
        entry->last_lba = start_lba + block_count - 1;
    }
    if (!gpt_entries_valid(primary, entries)) goto cleanup;
    crc = crc32_update(0xFFFFFFFFU, entries, (UINT32)entries_bytes) ^ 0xFFFFFFFFU;
    gpt_update_header_crc(backup_sector, crc);
    gpt_update_header_crc(primary_sector, crc);

    if (!block_device_write(device, backup->entries_lba, entries_sectors, entries) ||
        !block_device_write(device, backup->current_lba, 1, backup_sector) ||
        !block_device_flush(device)) {
        block_device_write(device, backup->entries_lba, entries_sectors,
                           original_entries);
        block_device_write(device, backup->current_lba, 1,
                           original_backup_sector);
        block_device_flush(device);
        goto cleanup;
    }
    if (!block_device_write(device, primary->entries_lba, entries_sectors, entries) ||
        !block_device_write(device, primary->current_lba, 1, primary_sector) ||
        !block_device_flush(device)) {
        block_device_write(device, primary->entries_lba, entries_sectors,
                           original_entries);
        block_device_write(device, primary->current_lba, 1,
                           original_primary_sector);
        block_device_write(device, backup->entries_lba, entries_sectors,
                           original_entries);
        block_device_write(device, backup->current_lba, 1,
                           original_backup_sector);
        block_device_flush(device);
        goto cleanup;
    }
    result = 1;
cleanup:
    if (original_entries != 0) kfree(original_entries);
    if (entries != 0) kfree(entries);
    return result;
}

int partition_gpt_create(ASAS_BLOCK_DEVICE *device, UINT32 slot,
                         const UINT8 type_guid[16],
                         const UINT8 unique_guid[16],
                         const UINT16 name[36], UINT64 start_lba,
                         UINT64 block_count)
{
    return partition_gpt_commit(device, slot, type_guid, unique_guid, name,
                                start_lba, block_count, 0);
}

int partition_gpt_delete(ASAS_BLOCK_DEVICE *device, UINT32 slot)
{
    return partition_gpt_commit(device, slot, 0, 0, 0, 0, 0, 1);
}

int partition_gpt_resize(ASAS_BLOCK_DEVICE *device, UINT32 slot,
                         UINT64 start_lba, UINT64 block_count)
{
    return partition_gpt_commit(device, slot, 0, 0, 0, start_lba,
                                block_count, 2);
}

#define PARTITION_TEST_SECTORS 128U
static UINT8 partition_test_image[PARTITION_TEST_SECTORS * 512U];

static int partition_test_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                               UINT32 count, void *buffer)
{
    UINT32 size = count * 512U;
    (void)device;
    if (lba >= PARTITION_TEST_SECTORS ||
        count > PARTITION_TEST_SECTORS - lba) return 0;
    copy_bytes((UINT8 *)buffer, partition_test_image + lba * 512U, size);
    return 1;
}

static int partition_test_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                                UINT32 count, const void *buffer)
{
    UINT32 size = count * 512U;
    (void)device;
    if (lba >= PARTITION_TEST_SECTORS || count > PARTITION_TEST_SECTORS - lba) return 0;
    copy_bytes(partition_test_image + lba * 512U, (const UINT8 *)buffer, size);
    return 1;
}

static const ASAS_BLOCK_DEVICE_OPS partition_test_ops = {
    partition_test_read, partition_test_write, 0
};

static ASAS_BLOCK_DEVICE *partition_test_disk_with_flags(UINT32 flags)
{
    ASAS_BLOCK_DEVICE description = { 0 };
    description.name[0] = 't';
    description.name[1] = 'e';
    description.name[2] = 's';
    description.name[3] = 't';
    description.name[4] = '0';
    description.logical_block_size = 512;
    description.physical_block_size = 512;
    description.block_count = PARTITION_TEST_SECTORS;
    description.flags = flags;
    description.ops = &partition_test_ops;
    return block_device_register(&description);
}

static ASAS_BLOCK_DEVICE *partition_test_disk(void)
{
    return partition_test_disk_with_flags(BLOCK_DEVICE_FLAG_READ_ONLY);
}

static void partition_test_clear(void)
{
    UINT32 index;
    for (index = 0; index < sizeof(partition_test_image); index++) {
        partition_test_image[index] = 0;
    }
}

static void partition_test_signature(UINT8 *sector)
{
    sector[510] = 0x55;
    sector[511] = 0xAA;
}

static int partition_test_backup_gpt(void)
{
    MBR_PARTITION_ENTRY *mbr;
    GPT_HEADER *header;
    GPT_ENTRY *entry;
    ASAS_BLOCK_DEVICE *disk;
    UINT8 *entry_sector;
    partition_test_clear();
    partition_test_signature(partition_test_image);
    mbr = (MBR_PARTITION_ENTRY *)(partition_test_image + 446);
    mbr[0].type = 0xEE;
    mbr[0].first_lba = 1;
    mbr[0].sector_count = PARTITION_TEST_SECTORS - 1;
    entry_sector = partition_test_image + 126U * 512U;
    entry = (GPT_ENTRY *)entry_sector;
    entry->type_guid[0] = 0xA2;
    entry->unique_guid[0] = 0x5A;
    entry->type_guid[0] = 0xA2;
    entry->type_guid[1] = 0xA0;
    entry->type_guid[2] = 0xD0;
    entry->type_guid[3] = 0xEB;
    entry->type_guid[4] = 0xE5;
    entry->type_guid[5] = 0xB9;
    entry->type_guid[6] = 0x33;
    entry->type_guid[7] = 0x44;
    entry->type_guid[8] = 0x87;
    entry->type_guid[9] = 0xC0;
    entry->type_guid[10] = 0x68;
    entry->type_guid[11] = 0xB6;
    entry->type_guid[12] = 0xB7;
    entry->type_guid[13] = 0x26;
    entry->type_guid[14] = 0x99;
    entry->type_guid[15] = 0xC7;
    entry->name[0] = 'D';
    entry->name[1] = 'a';
    entry->name[2] = 't';
    entry->name[3] = 'a';
    entry->first_lba = 40;
    entry->last_lba = 50;
    header = (GPT_HEADER *)(partition_test_image + 127U * 512U);
    copy_bytes(header->signature, (const UINT8 *)"EFI PART", 8);
    header->revision = 0x00010000U;
    header->header_size = 92;
    header->current_lba = 127;
    header->backup_lba = 1;
    header->first_usable_lba = 34;
    header->last_usable_lba = 125;
    header->entries_lba = 126;
    header->entry_count = 4;
    header->entry_size = 128;
    header->entries_crc32 =
        crc32_update(0xFFFFFFFFU, entry_sector, 512) ^ 0xFFFFFFFFU;
    header->header_crc32 = 0;
    header->header_crc32 =
        crc32_update(0xFFFFFFFFU, (const UINT8 *)header, 92) ^ 0xFFFFFFFFU;
    block_device_initialize();
    partition_manager_initialize();
    disk = partition_test_disk();
    return disk != 0 && partition_scan_device(disk) == 1 &&
           partition_count() == 1 && partitions[0].start_lba == 40 &&
           partitions[0].block_count == 11 &&
           partitions[0].known_type == PARTITION_TYPE_MICROSOFT_BASIC_DATA &&
           bytes_equal((const UINT8 *)partitions[0].label, "Data", 4) &&
           partitions[0].uuid[0] == '0' && partitions[0].uuid[6] == '5';
}

static int partition_test_overlapping_mbr(void)
{
    MBR_PARTITION_ENTRY *mbr;
    ASAS_BLOCK_DEVICE *disk;
    partition_test_clear();
    partition_test_signature(partition_test_image);
    mbr = (MBR_PARTITION_ENTRY *)(partition_test_image + 446);
    mbr[0].type = 0x0C;
    mbr[0].first_lba = 10;
    mbr[0].sector_count = 30;
    mbr[1].type = 0x07;
    mbr[1].first_lba = 20;
    mbr[1].sector_count = 30;
    block_device_initialize();
    partition_manager_initialize();
    disk = partition_test_disk();
    return disk != 0 && partition_scan_device(disk) == 0 && partition_count() == 0;
}

static int partition_test_extended_mbr(void)
{
    MBR_PARTITION_ENTRY *mbr;
    MBR_PARTITION_ENTRY *ebr;
    ASAS_BLOCK_DEVICE *disk;
    partition_test_clear();
    partition_test_signature(partition_test_image);
    mbr = (MBR_PARTITION_ENTRY *)(partition_test_image + 446);
    mbr[0].type = 0x0F;
    mbr[0].first_lba = 10;
    mbr[0].sector_count = 100;
    partition_test_signature(partition_test_image + 10U * 512U);
    ebr = (MBR_PARTITION_ENTRY *)(partition_test_image + 10U * 512U + 446);
    ebr[0].type = 0x0C;
    ebr[0].first_lba = 1;
    ebr[0].sector_count = 10;
    ebr[1].type = 0x0F;
    ebr[1].first_lba = 20;
    ebr[1].sector_count = 80;
    partition_test_signature(partition_test_image + 30U * 512U);
    ebr = (MBR_PARTITION_ENTRY *)(partition_test_image + 30U * 512U + 446);
    ebr[0].type = 0x07;
    ebr[0].first_lba = 1;
    ebr[0].sector_count = 10;
    block_device_initialize();
    partition_manager_initialize();
    disk = partition_test_disk();
    return disk != 0 && partition_scan_device(disk) == 2 &&
           partition_count() == 2 && partitions[0].start_lba == 11 &&
           partitions[1].start_lba == 31;
}

static int partition_test_mount(ASAS_FILESYSTEM_MOUNT *mount)
{
    mount->context = mount;
    return 1;
}

static const ASAS_FILESYSTEM_DRIVER partition_test_fs = {
    "testfs", 0, 0, partition_test_mount, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static int partition_test_mutation(void)
{
    ASAS_BLOCK_DEVICE *disk;
    MBR_PARTITION_ENTRY *entries;
    ASAS_FILESYSTEM_MOUNT *mount;
    partition_test_clear();
    partition_test_signature(partition_test_image);
    block_device_initialize();
    partition_manager_initialize();
    filesystem_initialize();
    disk = partition_test_disk_with_flags(0);
    if (disk == 0 ||
        !partition_mbr_create(disk, 0, 0x0C, 8, 20) ||
        partition_mbr_create(disk, 1, 0x07, 16, 20)) return 0;
    entries = (MBR_PARTITION_ENTRY *)(partition_test_image + 446);
    if (entries[0].type != 0x0C || entries[0].first_lba != 8 ||
        entries[0].sector_count != 20 ||
        !partition_mbr_resize(disk, 0, 12, 24) ||
        entries[0].first_lba != 12 || entries[0].sector_count != 24) return 0;

    mount = filesystem_mount(disk, &partition_test_fs, 0);
    if (mount == 0 || partition_mbr_delete(disk, 0) ||
        !filesystem_unmount(mount) || !partition_mbr_delete(disk, 0) ||
        entries[0].type != 0 || entries[0].sector_count != 0) return 0;

    block_device_initialize();
    disk = partition_test_disk_with_flags(BLOCK_DEVICE_FLAG_READ_ONLY);
    return disk != 0 && !partition_mbr_create(disk, 0, 0x0C, 8, 20);
}

static void partition_test_make_gpt(void)
{
    MBR_PARTITION_ENTRY *mbr;
    GPT_HEADER *primary;
    GPT_HEADER *backup;
    UINT32 entries_crc;
    partition_test_clear();
    partition_test_signature(partition_test_image);
    mbr = (MBR_PARTITION_ENTRY *)(partition_test_image + 446);
    mbr[0].type = 0xEE;
    mbr[0].first_lba = 1;
    mbr[0].sector_count = PARTITION_TEST_SECTORS - 1;
    entries_crc = crc32_update(0xFFFFFFFFU,
                               partition_test_image + 2U * 512U, 512) ^
                  0xFFFFFFFFU;

    primary = (GPT_HEADER *)(partition_test_image + 512U);
    copy_bytes(primary->signature, (const UINT8 *)"EFI PART", 8);
    primary->revision = 0x00010000U;
    primary->header_size = 92;
    primary->current_lba = 1;
    primary->backup_lba = 127;
    primary->first_usable_lba = 34;
    primary->last_usable_lba = 125;
    primary->disk_guid[0] = 0x42;
    primary->entries_lba = 2;
    primary->entry_count = 4;
    primary->entry_size = 128;
    primary->entries_crc32 = entries_crc;
    primary->header_crc32 = 0;
    primary->header_crc32 =
        crc32_update(0xFFFFFFFFU, (const UINT8 *)primary, 92) ^ 0xFFFFFFFFU;

    backup = (GPT_HEADER *)(partition_test_image + 127U * 512U);
    copy_bytes((UINT8 *)backup, (const UINT8 *)primary, sizeof(GPT_HEADER));
    backup->current_lba = 127;
    backup->backup_lba = 1;
    backup->entries_lba = 126;
    backup->header_crc32 = 0;
    backup->header_crc32 =
        crc32_update(0xFFFFFFFFU, (const UINT8 *)backup, 92) ^ 0xFFFFFFFFU;
}

static int partition_test_gpt_mutation(void)
{
    static const UINT8 type_guid[16] =
        {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,
         0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
    static const UINT8 unique_a[16] =
        {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    static const UINT8 unique_b[16] =
        {16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1};
    UINT16 name[36] = { 'D','a','t','a' };
    ASAS_BLOCK_DEVICE *disk;
    GPT_ENTRY *primary_entries;
    GPT_ENTRY *backup_entries;
    UINT8 primary_header[512];
    UINT8 backup_header[512];
    UINT32 index;
    partition_test_make_gpt();
    block_device_initialize();
    partition_manager_initialize();
    filesystem_initialize();
    disk = partition_test_disk_with_flags(0);
    if (disk == 0 ||
        !partition_gpt_create(disk, 0, type_guid, unique_a, name, 40, 11) ||
        partition_gpt_create(disk, 1, type_guid, unique_b, name, 45, 10)) return 0;
    primary_entries = (GPT_ENTRY *)(partition_test_image + 2U * 512U);
    backup_entries = (GPT_ENTRY *)(partition_test_image + 126U * 512U);
    if (primary_entries[0].first_lba != 40 ||
        primary_entries[0].last_lba != 50) return 0;
    for (index = 0; index < 512; index++) {
        if (partition_test_image[2U * 512U + index] !=
            partition_test_image[126U * 512U + index]) return 0;
    }
    if (!partition_gpt_resize(disk, 0, 60, 10) ||
        primary_entries[0].first_lba != 60 ||
        primary_entries[0].last_lba != 69 ||
        backup_entries[0].first_lba != 60 ||
        backup_entries[0].last_lba != 69 ||
        !read_gpt_header(disk, 1, primary_header) ||
        !read_gpt_header(disk, 127, backup_header) ||
        !partition_gpt_delete(disk, 0) ||
        !guid_is_zero(primary_entries[0].type_guid) ||
        !guid_is_zero(backup_entries[0].type_guid)) return 0;

    partition_test_make_gpt();
    block_device_initialize();
    disk = partition_test_disk_with_flags(BLOCK_DEVICE_FLAG_READ_ONLY);
    return disk != 0 &&
           !partition_gpt_create(disk, 0, type_guid, unique_a, name, 40, 11);
}

static int partition_test_gpt_rare_layouts(void)
{
    MBR_PARTITION_ENTRY *mbr;
    GPT_HEADER *header;
    ASAS_BLOCK_DEVICE *disk;
    UINT32 entry_size;
    partition_test_clear();
    partition_test_signature(partition_test_image);
    mbr = (MBR_PARTITION_ENTRY *)(partition_test_image + 446);
    mbr[0].type = 0xEE;
    mbr[0].first_lba = 1;
    mbr[0].sector_count = PARTITION_TEST_SECTORS - 1;
    header = (GPT_HEADER *)(partition_test_image + 512U);
    copy_bytes(header->signature, (const UINT8 *)"EFI PART", 8);
    header->revision = 0x00010000U;
    header->header_size = 92;
    header->current_lba = 1;
    header->backup_lba = 127;
    header->first_usable_lba = 34;
    header->last_usable_lba = 125;
    header->entries_lba = 2;
    header->entry_count = 4;
    header->entry_size = 192;
    header->header_crc32 = 0;
    header->header_crc32 = crc32_update(0xFFFFFFFFU, (const UINT8 *)header, 92) ^ 0xFFFFFFFFU;
    block_device_initialize();
    partition_manager_initialize();
    disk = partition_test_disk();
    if (disk == 0 || partition_scan_device(disk) != 0) return 0;

    for (entry_size = 129; entry_size < 512; entry_size += 37) {
        header->entry_size = entry_size;
        header->header_crc32 = 0;
        header->header_crc32 = crc32_update(0xFFFFFFFFU, (const UINT8 *)header, 92) ^ 0xFFFFFFFFU;
        partition_manager_initialize();
        if (partition_scan_device(disk) != 0 || partition_count() != 0) return 0;
    }
    return 1;
}

int partition_self_test(void)
{
    int result = partition_test_backup_gpt() &&
                 partition_test_overlapping_mbr() &&
                 partition_test_extended_mbr() &&
                 partition_test_mutation() &&
                 partition_test_gpt_mutation() &&
                 partition_test_gpt_rare_layouts();
    block_device_initialize();
    partition_manager_initialize();
    return result;
}
