#include "iso9660.h"
#include "heap.h"

#define ISO_SECTOR_SIZE 2048U
#define ISO_PVD_SECTOR 16U
#define ISO_RECORD_DIRECTORY 0x02U
#define ISO_RECORD_MULTI_EXTENT 0x80U
#define ISO_MAX_EXTENTS 16U

typedef struct {
    UINT32 extent;
    UINT32 size;
} ISO9660_EXTENT;

typedef struct {
    ISO9660_EXTENT extents[ISO_MAX_EXTENTS];
    UINT32 extent_count;
    UINT32 extent;
    UINT32 size;
    UINT32 rr_mode;
    UINT8 rr_has_mode;
    UINT8 rr_has_tf;
    UINT8 rr_is_symlink;
    char rr_symlink[256];
    char rr_timestamp[18];
    UINT8 flags;
} ISO9660_RECORD;

struct ISO9660_CONTEXT {
    ASAS_BLOCK_DEVICE *device;
    ISO9660_RECORD root;
    UINT8 joliet;
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

static UINT32 string_length(const char *text)
{
    UINT32 length = 0;
    if (text == 0) return 0;
    while (text[length] != '\0') length++;
    return length;
}

static void copy_name(char *destination, const UINT8 *source, UINT32 length)
{
    UINT32 in_index = 0;
    UINT32 out_index = 0;
    while (in_index < length && out_index + 1U < 256U) {
        char value = (char)source[in_index++];
        if (value == ';') break;
        destination[out_index++] = value;
    }
    if (out_index != 0 && destination[out_index - 1U] == '.')
        out_index--;
    destination[out_index] = '\0';
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

static void copy_joliet_name(char *destination, const UINT8 *source,
                             UINT32 length)
{
    UINT32 in_index = 0;
    UINT32 out_index = 0;
    while (in_index + 1U < length && out_index + 1U < 256U) {
        UINT16 value = ((UINT16)source[in_index] << 8) |
                       (UINT16)source[in_index + 1U];
        in_index += 2U;
        if (value == ';') break;
        if (value < 0x80U) {
            destination[out_index++] = (char)value;
        } else if (value < 0x800U && out_index + 2U < 256U) {
            destination[out_index++] = (char)(0xC0U | (value >> 6));
            destination[out_index++] = (char)(0x80U | (value & 0x3FU));
        } else if (out_index + 3U < 256U) {
            destination[out_index++] = (char)(0xE0U | (value >> 12));
            destination[out_index++] = (char)(0x80U | ((value >> 6) & 0x3FU));
            destination[out_index++] = (char)(0x80U | (value & 0x3FU));
        }
    }
    if (out_index != 0 && destination[out_index - 1U] == '.')
        out_index--;
    destination[out_index] = '\0';
}

static void record_name_to_utf8(ISO9660_CONTEXT *context, char *destination,
                                const UINT8 *source, UINT32 length)
{
    if (context != 0 && context->joliet)
        copy_joliet_name(destination, source, length);
    else
        copy_name(destination, source, length);
}

static int rock_ridge_name(const UINT8 *record, char *destination)
{
    UINT32 record_length;
    UINT32 name_length;
    UINT32 offset;
    UINT32 out_index = 0;

    if (record == 0 || destination == 0 || record[0] < 34U) return 0;
    record_length = record[0];
    name_length = record[32];
    offset = 33U + name_length;
    if ((name_length & 1U) == 0) offset++;

    while (offset + 4U <= record_length) {
        UINT8 entry_length = record[offset + 2U];
        UINT8 version = record[offset + 3U];
        if (entry_length < 4U || offset + entry_length > record_length)
            break;
        if (record[offset] == 'N' && record[offset + 1U] == 'M' &&
            version == 1U && entry_length >= 5U) {
            UINT32 data_offset = offset + 5U;
            UINT32 data_end = offset + entry_length;
            while (data_offset < data_end && out_index + 1U < 256U) {
                destination[out_index++] = (char)record[data_offset++];
            }
            destination[out_index] = '\0';
            return out_index != 0;
        }
        offset += entry_length;
    }
    return 0;
}

static void rock_ridge_parse_px(const UINT8 *entry, ISO9660_RECORD *record)
{
    if (entry[2] < 12U || record == 0) return;
    record->rr_mode = read_le32(entry + 4U);
    record->rr_has_mode = 1;
}

static void rock_ridge_parse_tf(const UINT8 *entry, ISO9660_RECORD *record)
{
    UINT8 flags;
    UINT32 offset = 5U;
    UINT32 index;
    if (entry[2] < 12U || record == 0) return;
    flags = entry[4U];
    if ((flags & 0x80U) != 0) return; /* Long form is not surfaced yet. */
    if ((flags & 0x01U) == 0) return; /* Creation time absent. */
    if (offset + 7U > entry[2]) return;
    for (index = 0; index < 7U && index + 1U < sizeof(record->rr_timestamp);
         index++) {
        record->rr_timestamp[index] = (char)entry[offset + index];
    }
    record->rr_timestamp[index] = '\0';
    record->rr_has_tf = 1;
}

static void rock_ridge_parse_sl(const UINT8 *entry, ISO9660_RECORD *record)
{
    UINT32 offset = 5U;
    UINT32 out_index = 0;
    if (record == 0 || entry[2] < 7U) return;
    while (offset + 2U <= entry[2] && out_index + 1U < sizeof(record->rr_symlink)) {
        UINT8 flags = entry[offset];
        UINT8 length = entry[offset + 1U];
        offset += 2U;
        if (offset + length > entry[2]) break;
        if (out_index != 0 && out_index + 1U < sizeof(record->rr_symlink))
            record->rr_symlink[out_index++] = '/';
        if ((flags & 0x02U) != 0) {
            record->rr_symlink[out_index++] = '.';
        } else if ((flags & 0x04U) != 0) {
            if (out_index + 2U < sizeof(record->rr_symlink)) {
                record->rr_symlink[out_index++] = '.';
                record->rr_symlink[out_index++] = '.';
            }
        } else if ((flags & 0x08U) != 0) {
            record->rr_symlink[out_index++] = '/';
        } else {
            UINT32 index;
            for (index = 0; index < length &&
                 out_index + 1U < sizeof(record->rr_symlink); index++) {
                record->rr_symlink[out_index++] = (char)entry[offset + index];
            }
        }
        offset += length;
    }
    record->rr_symlink[out_index] = '\0';
    record->rr_is_symlink = record->rr_symlink[0] != '\0';
}

static void rock_ridge_metadata(const UINT8 *record_bytes,
                                ISO9660_RECORD *record)
{
    UINT32 record_length;
    UINT32 name_length;
    UINT32 offset;
    if (record_bytes == 0 || record == 0 || record_bytes[0] < 34U) return;
    record_length = record_bytes[0];
    name_length = record_bytes[32];
    offset = 33U + name_length;
    if ((name_length & 1U) == 0) offset++;

    while (offset + 4U <= record_length) {
        const UINT8 *entry = record_bytes + offset;
        UINT8 entry_length = entry[2U];
        UINT8 version = entry[3U];
        if (entry_length < 4U || offset + entry_length > record_length)
            break;
        if (version == 1U && entry[0] == 'P' && entry[1] == 'X')
            rock_ridge_parse_px(entry, record);
        else if (version == 1U && entry[0] == 'S' && entry[1] == 'L')
            rock_ridge_parse_sl(entry, record);
        else if (version == 1U && entry[0] == 'T' && entry[1] == 'F')
            rock_ridge_parse_tf(entry, record);
        offset += entry_length;
    }
}

static void directory_record_name(ISO9660_CONTEXT *context, char *destination,
                                  const UINT8 *record)
{
    if (!rock_ridge_name(record, destination)) {
        record_name_to_utf8(context, destination, record + 33U, record[32]);
    }
}

static int read_bytes(ISO9660_CONTEXT *context, UINT64 byte_offset,
                      void *buffer, UINT32 size)
{
    UINT8 block[4096];
    UINT8 *output = (UINT8 *)buffer;
    UINT32 copied = 0;
    UINT32 block_size;

    if (context == 0 || context->device == 0 || buffer == 0) return 0;
    block_size = context->device->logical_block_size;
    if (block_size < 512U || block_size > sizeof(block)) return 0;

    while (copied < size) {
        UINT64 block_lba = byte_offset / block_size;
        UINT32 block_offset = (UINT32)(byte_offset % block_size);
        UINT32 chunk = block_size - block_offset;
        if (chunk > size - copied) chunk = size - copied;
        if (!block_device_read(context->device, block_lba, 1, block))
            return 0;
        {
            UINT32 index;
            for (index = 0; index < chunk; index++)
                output[copied + index] = block[block_offset + index];
        }
        copied += chunk;
        byte_offset += chunk;
    }
    return 1;
}

static int read_iso_sector(ISO9660_CONTEXT *context, UINT32 sector,
                           UINT8 buffer[ISO_SECTOR_SIZE])
{
    return read_bytes(context, (UINT64)sector * ISO_SECTOR_SIZE,
                      buffer, ISO_SECTOR_SIZE);
}

static int parse_record(const UINT8 *record, ISO9660_RECORD *output)
{
    if (record == 0 || output == 0 || record[0] < 34U) return 0;
    output->extent = read_le32(record + 2);
    output->size = read_le32(record + 10);
    output->flags = record[25];
    output->rr_mode = 0;
    output->rr_has_mode = 0;
    output->rr_has_tf = 0;
    output->rr_is_symlink = 0;
    output->rr_symlink[0] = '\0';
    output->rr_timestamp[0] = '\0';
    output->extent_count = 1;
    output->extents[0].extent = output->extent;
    output->extents[0].size = output->size;
    rock_ridge_metadata(record, output);
    return 1;
}

static int append_extent(ISO9660_RECORD *output, const ISO9660_RECORD *part)
{
    if (output == 0 || part == 0 ||
        output->extent_count >= ISO_MAX_EXTENTS) return 0;
    output->extents[output->extent_count].extent = part->extent;
    output->extents[output->extent_count].size = part->size;
    output->extent_count++;
    output->size += part->size;
    output->flags = part->flags;
    return 1;
}

int iso9660_probe(ASAS_BLOCK_DEVICE *device)
{
    ISO9660_CONTEXT context;
    UINT8 sector[ISO_SECTOR_SIZE];
    if (device == 0 || device->logical_block_size < 512U ||
        device->logical_block_size > 4096U) return 0;
    context.device = device;
    if (!read_iso_sector(&context, ISO_PVD_SECTOR, sector)) return 0;
    return sector[0] == 1U && sector[1] == 'C' && sector[2] == 'D' &&
           sector[3] == '0' && sector[4] == '0' && sector[5] == '1' &&
           sector[6] == 1U && read_le16(sector + 128) == ISO_SECTOR_SIZE;
}

ISO9660_CONTEXT *iso9660_context_create(ASAS_BLOCK_DEVICE *device)
{
    ISO9660_CONTEXT *context;
    UINT8 sector[ISO_SECTOR_SIZE];
    UINT32 descriptor;
    if (!iso9660_probe(device)) return 0;
    context = (ISO9660_CONTEXT *)kmalloc(sizeof(ISO9660_CONTEXT));
    if (context == 0) return 0;
    context->device = device;
    context->joliet = 0;
    if (!read_iso_sector(context, ISO_PVD_SECTOR, sector) ||
        !parse_record(sector + 156, &context->root)) {
        kfree(context);
        return 0;
    }
    for (descriptor = ISO_PVD_SECTOR + 1U; descriptor < 64U; descriptor++) {
        if (!read_iso_sector(context, descriptor, sector)) break;
        if (sector[1] != 'C' || sector[2] != 'D' || sector[3] != '0' ||
            sector[4] != '0' || sector[5] != '1') break;
        if (sector[0] == 255U) break;
        if (sector[0] == 2U &&
            sector[88] == 0x25U && sector[89] == 0x2FU &&
            (sector[90] == 0x40U || sector[90] == 0x43U ||
             sector[90] == 0x45U) &&
            read_le16(sector + 128) == ISO_SECTOR_SIZE &&
            parse_record(sector + 156, &context->root)) {
            context->joliet = 1;
            break;
        }
    }
    return context;
}

void iso9660_context_destroy(ISO9660_CONTEXT *context)
{
    if (context != 0) kfree(context);
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

static int find_in_directory(ISO9660_CONTEXT *context,
                             const ISO9660_RECORD *directory,
                             const char *name, ISO9660_RECORD *result)
{
    UINT32 offset = 0;
    UINT8 sector[ISO_SECTOR_SIZE];
    char collecting_name[256];
    UINT8 collecting = 0;
    if (context == 0 || directory == 0 || name == 0 || result == 0 ||
        (directory->flags & ISO_RECORD_DIRECTORY) == 0) return 0;
    collecting_name[0] = '\0';

    while (offset < directory->size) {
        UINT32 sector_offset = offset % ISO_SECTOR_SIZE;
        UINT32 sector_index = directory->extent + offset / ISO_SECTOR_SIZE;
        if (!read_iso_sector(context, sector_index, sector)) return 0;
        while (sector_offset < ISO_SECTOR_SIZE && offset < directory->size) {
            UINT8 record_length = sector[sector_offset];
            UINT8 name_length;
            char record_name[256];
            if (record_length == 0) {
                offset += ISO_SECTOR_SIZE - sector_offset;
                break;
            }
            if (sector_offset + record_length > ISO_SECTOR_SIZE ||
                record_length < 34U) return 0;
            name_length = sector[sector_offset + 32U];
            if (name_length != 1U ||
                (sector[sector_offset + 33U] != 0U &&
                 sector[sector_offset + 33U] != 1U)) {
                directory_record_name(context, record_name,
                                      sector + sector_offset);
                if (collecting) {
                    if (strings_equal_ci(record_name, collecting_name)) {
                        ISO9660_RECORD part;
                        if (!parse_record(sector + sector_offset, &part) ||
                            !append_extent(result, &part)) return 0;
                        if ((part.flags & ISO_RECORD_MULTI_EXTENT) == 0)
                            return 1;
                    } else {
                        return 0;
                    }
                } else if (strings_equal_ci(record_name, name)) {
                    if (!parse_record(sector + sector_offset, result))
                        return 0;
                    if ((result->flags & ISO_RECORD_MULTI_EXTENT) == 0)
                        return 1;
                    copy_string(collecting_name, record_name,
                                sizeof(collecting_name));
                    collecting = 1;
                }
            }
            offset += record_length;
            sector_offset += record_length;
        }
    }
    return collecting && (result->flags & ISO_RECORD_MULTI_EXTENT) == 0;
}

static int find_record(ISO9660_CONTEXT *context, const char *path,
                       ISO9660_RECORD *record)
{
    ISO9660_RECORD current;
    char component[256];
    const char *cursor = path;
    if (context == 0 || path == 0 || record == 0) return 0;
    current = context->root;
    while (next_path_component(&cursor, component)) {
        ISO9660_RECORD child;
        if (!find_in_directory(context, &current, component, &child))
            return 0;
        current = child;
    }
    *record = current;
    return 1;
}

int iso9660_context_exists(ISO9660_CONTEXT *context, const char *path)
{
    ISO9660_RECORD record;
    return find_record(context, path, &record);
}

int iso9660_context_is_directory(ISO9660_CONTEXT *context, const char *path)
{
    ISO9660_RECORD record;
    return find_record(context, path, &record) &&
           (record.flags & ISO_RECORD_DIRECTORY) != 0;
}

int iso9660_context_is_read_only(ISO9660_CONTEXT *context, const char *path)
{
    ISO9660_RECORD record;
    if (!find_record(context, path, &record)) return 1;
    return !record.rr_has_mode || (record.rr_mode & 0222U) == 0;
}

UINT64 iso9660_context_file_size(ISO9660_CONTEXT *context, const char *path)
{
    ISO9660_RECORD record;
    if (!find_record(context, path, &record) ||
        (record.flags & ISO_RECORD_DIRECTORY) != 0) return 0;
    if (record.rr_is_symlink) return string_length(record.rr_symlink);
    return record.size;
}

UINT64 iso9660_context_read_file(ISO9660_CONTEXT *context, const char *path,
                                 void *buffer, UINT64 capacity)
{
    ISO9660_RECORD record;
    UINT64 bytes;
    UINT64 remaining;
    UINT64 written = 0;
    UINT32 index;
    if (buffer == 0 || !find_record(context, path, &record) ||
        (record.flags & ISO_RECORD_DIRECTORY) != 0) return 0;
    if (record.rr_is_symlink) {
        UINT32 length = string_length(record.rr_symlink);
        UINT32 copy = capacity < length ? (UINT32)capacity : length;
        for (index = 0; index < copy; index++)
            ((UINT8 *)buffer)[index] = (UINT8)record.rr_symlink[index];
        return copy;
    }
    bytes = capacity < record.size ? capacity : record.size;
    if (bytes == 0) return 0;
    if (bytes > 0xFFFFFFFFULL) bytes = 0xFFFFFFFFULL;
    remaining = bytes;
    for (index = 0; index < record.extent_count && remaining != 0; index++) {
        UINT32 chunk = record.extents[index].size;
        if ((UINT64)chunk > remaining) chunk = (UINT32)remaining;
        if (!read_bytes(context,
                        (UINT64)record.extents[index].extent * ISO_SECTOR_SIZE,
                        (UINT8 *)buffer + written, chunk))
            return 0;
        written += chunk;
        remaining -= chunk;
    }
    return written == bytes ? bytes : 0;
}

UINT64 iso9660_context_list_directory(ISO9660_CONTEXT *context, const char *path,
                                      ISO9660_FILE_INFO *entries,
                                      UINT64 capacity)
{
    ISO9660_RECORD directory;
    UINT64 count = 0;
    UINT32 offset = 0;
    UINT8 sector[ISO_SECTOR_SIZE];
    char collecting_name[256];
    UINT8 collecting = 0;
    if (entries == 0 || capacity == 0 ||
        !find_record(context, path, &directory) ||
        (directory.flags & ISO_RECORD_DIRECTORY) == 0) return 0;
    collecting_name[0] = '\0';

    while (offset < directory.size && count < capacity) {
        UINT32 sector_offset = offset % ISO_SECTOR_SIZE;
        UINT32 sector_index = directory.extent + offset / ISO_SECTOR_SIZE;
        if (!read_iso_sector(context, sector_index, sector)) return count;
        while (sector_offset < ISO_SECTOR_SIZE && offset < directory.size &&
               count < capacity) {
            UINT8 record_length = sector[sector_offset];
            UINT8 name_length;
            ISO9660_RECORD record;
            if (record_length == 0) {
                offset += ISO_SECTOR_SIZE - sector_offset;
                break;
            }
            if (sector_offset + record_length > ISO_SECTOR_SIZE ||
                record_length < 34U) return count;
            name_length = sector[sector_offset + 32U];
            if (name_length != 1U ||
                (sector[sector_offset + 33U] != 0U &&
                 sector[sector_offset + 33U] != 1U)) {
                if (parse_record(sector + sector_offset, &record)) {
                    char record_name[256];
                    directory_record_name(context, record_name,
                                          sector + sector_offset);
                    if (collecting &&
                        strings_equal_ci(record_name, collecting_name)) {
                        collecting = (record.flags & ISO_RECORD_MULTI_EXTENT)
                            != 0;
                    } else {
                        copy_string(entries[count].name, record_name,
                                    sizeof(entries[count].name));
                        entries[count].size = record.size;
                        entries[count].is_directory =
                            (record.flags & ISO_RECORD_DIRECTORY) != 0;
                        if (record.rr_is_symlink) entries[count].size =
                            string_length(record.rr_symlink);
                        entries[count].read_only = !record.rr_has_mode ||
                            (record.rr_mode & 0222U) == 0;
                        count++;
                        if ((record.flags & ISO_RECORD_MULTI_EXTENT) != 0) {
                            copy_string(collecting_name, record_name,
                                        sizeof(collecting_name));
                            collecting = 1;
                        } else {
                            collecting = 0;
                            collecting_name[0] = '\0';
                        }
                    }
                }
            }
            offset += record_length;
            sector_offset += record_length;
        }
    }
    return count;
}

int iso9660_context_write_file(ISO9660_CONTEXT *context, const char *path,
                               const void *buffer, UINT64 size)
{
    (void)context; (void)path; (void)buffer; (void)size;
    return 0;
}

int iso9660_context_delete_file(ISO9660_CONTEXT *context, const char *path)
{
    (void)context; (void)path;
    return 0;
}

int iso9660_context_create_directory(ISO9660_CONTEXT *context, const char *path)
{
    (void)context; (void)path;
    return 0;
}

int iso9660_context_delete_directory(ISO9660_CONTEXT *context, const char *path)
{
    (void)context; (void)path;
    return 0;
}

int iso9660_context_rename(ISO9660_CONTEXT *context, const char *source,
                           const char *destination)
{
    (void)context; (void)source; (void)destination;
    return 0;
}

int iso9660_context_sync(ISO9660_CONTEXT *context)
{
    if (context == 0 || context->device == 0) return 0;
    return block_device_flush(context->device);
}
