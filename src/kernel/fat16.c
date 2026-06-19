#include "fat16.h"
#include "virtio_block.h"
#include "logger.h"

#pragma pack(push, 1)
typedef struct {
    UINT8 jump[3];
    UINT8 oem[8];
    UINT16 bytes_per_sector;
    UINT8 sectors_per_cluster;
    UINT16 reserved_sectors;
    UINT8 fat_count;
    UINT16 root_entry_count;
    UINT16 total_sectors_16;
    UINT8 media;
    UINT16 sectors_per_fat;
} FAT16_BPB;

typedef struct {
    UINT8 name[11];
    UINT8 attributes;
    UINT8 reserved[14];
    UINT16 first_cluster;
    UINT32 size;
} FAT16_DIRECTORY_ENTRY;
#pragma pack(pop)

static FAT16_BPB bpb;
static UINT32 root_start_sector;
static UINT32 root_sector_count;
static UINT32 data_start_sector;

static int resolve_path(const char *path, FAT16_DIRECTORY_ENTRY *result);

static void clear_bytes(void *buffer, UINT32 size)
{
    UINT8 *bytes = (UINT8 *)buffer;
    UINT32 index;

    for (index = 0; index < size; index++) {
        bytes[index] = 0;
    }
}

static void copy_entry(FAT16_DIRECTORY_ENTRY *destination, const FAT16_DIRECTORY_ENTRY *source)
{
    UINT32 index;
    UINT8 *destination_bytes = (UINT8 *)destination;
    const UINT8 *source_bytes = (const UINT8 *)source;

    for (index = 0; index < sizeof(FAT16_DIRECTORY_ENTRY); index++) {
        destination_bytes[index] = source_bytes[index];
    }
}

static int name_matches(const UINT8 left[11], const char right[11])
{
    UINT32 index;

    for (index = 0; index < 11; index++) {
        if (left[index] != (UINT8)right[index]) {
            return 0;
        }
    }
    return 1;
}

static void format_name(const UINT8 source[11], char destination[13])
{
    UINT32 source_index;
    UINT32 destination_index = 0;

    for (source_index = 0; source_index < 8 && source[source_index] != ' '; source_index++) {
        destination[destination_index++] = (char)source[source_index];
    }

    if (source[8] != ' ') {
        destination[destination_index++] = '.';
        for (source_index = 8; source_index < 11 && source[source_index] != ' '; source_index++) {
            destination[destination_index++] = (char)source[source_index];
        }
    }

    destination[destination_index] = '\0';
}

static UINT16 fat16_next_cluster(UINT16 cluster)
{
    UINT8 sector[512];
    UINT32 fat_offset = (UINT32)cluster * 2;
    UINT32 fat_sector = bpb.reserved_sectors + fat_offset / 512;
    UINT32 offset = fat_offset % 512;

    if (!virtio_block_read_sector(fat_sector, sector)) {
        logger_write_hex("ERROR", "FAT sector read failed", fat_sector);
        return 0xFFFF;
    }
    {
        return (UINT16)(sector[offset] | ((UINT16)sector[offset + 1] << 8));
    }
}

static int fat16_set_cluster(UINT16 cluster, UINT16 value)
{
    UINT8 sector[512];
    UINT32 fat_index;
    UINT32 fat_offset = (UINT32)cluster * 2;
    UINT32 sector_offset = fat_offset / 512;
    UINT32 offset = fat_offset % 512;

    for (fat_index = 0; fat_index < bpb.fat_count; fat_index++) {
        UINT32 fat_sector = bpb.reserved_sectors + fat_index * bpb.sectors_per_fat + sector_offset;

        if (!virtio_block_read_sector(fat_sector, sector)) {
            return 0;
        }
        sector[offset] = (UINT8)(value & 0xFF);
        sector[offset + 1] = (UINT8)(value >> 8);
        if (!virtio_block_write_sector(fat_sector, sector)) {
            return 0;
        }
    }
    return 1;
}

static UINT16 fat16_allocate_cluster(void)
{
    UINT32 cluster;
    UINT32 maximum_cluster = bpb.sectors_per_fat * 256U;

    for (cluster = 2; cluster < maximum_cluster && cluster < 0xFFF0; cluster++) {
        if (fat16_next_cluster((UINT16)cluster) == 0) {
            if (fat16_set_cluster((UINT16)cluster, 0xFFFF)) {
                return (UINT16)cluster;
            }
            return 0;
        }
    }
    return 0;
}

static int fat16_free_chain(UINT16 cluster)
{
    while (cluster >= 2 && cluster < 0xFFF8) {
        UINT16 next = fat16_next_cluster(cluster);

        if (!fat16_set_cluster(cluster, 0)) {
            return 0;
        }
        cluster = next;
    }
    return 1;
}

static UINT16 fat16_allocate_chain(UINT32 cluster_count)
{
    UINT16 first = 0;
    UINT16 previous = 0;
    UINT32 index;

    for (index = 0; index < cluster_count; index++) {
        UINT16 cluster = fat16_allocate_cluster();

        if (cluster == 0) {
            if (first != 0) {
                (void)fat16_free_chain(first);
            }
            return 0;
        }
        if (first == 0) {
            first = cluster;
        }
        if (previous != 0 && !fat16_set_cluster(previous, cluster)) {
            (void)fat16_free_chain(first);
            return 0;
        }
        previous = cluster;
    }
    return first;
}

static int write_cluster_chain(UINT16 first_cluster, const void *buffer, UINT64 size)
{
    UINT8 sector[512];
    const UINT8 *source = (const UINT8 *)buffer;
    UINT16 cluster = first_cluster;
    UINT64 written = 0;

    while (cluster >= 2 && cluster < 0xFFF8 && written < size) {
        UINT32 cluster_sector;

        for (cluster_sector = 0; cluster_sector < bpb.sectors_per_cluster && written < size; cluster_sector++) {
            UINT32 index;
            UINT64 remaining = size - written;
            UINT32 copy_size = remaining < 512 ? (UINT32)remaining : 512;

            clear_bytes(sector, sizeof(sector));
            for (index = 0; index < copy_size; index++) {
                sector[index] = source[written + index];
            }
            if (!virtio_block_write_sector(
                data_start_sector + ((UINT32)cluster - 2) * bpb.sectors_per_cluster + cluster_sector,
                sector
            )) {
                return 0;
            }
            written += copy_size;
        }
        cluster = fat16_next_cluster(cluster);
    }
    return written == size;
}

static int find_root_entry_slot(
    const char name[11],
    UINT8 sector[512],
    UINT32 *sector_number,
    FAT16_DIRECTORY_ENTRY **entry_result,
    int allow_free
)
{
    UINT32 root_sector;

    for (root_sector = 0; root_sector < root_sector_count; root_sector++) {
        UINT32 entry_index;

        if (!virtio_block_read_sector(root_start_sector + root_sector, sector)) {
            return 0;
        }
        for (entry_index = 0; entry_index < 16; entry_index++) {
            FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&sector[entry_index * 32];

            if (entry->name[0] != 0 && entry->name[0] != 0xE5 && name_matches(entry->name, name)) {
                *sector_number = root_start_sector + root_sector;
                *entry_result = entry;
                return 1;
            }
        }
    }
    if (allow_free) {
        for (root_sector = 0; root_sector < root_sector_count; root_sector++) {
            UINT32 entry_index;

            if (!virtio_block_read_sector(root_start_sector + root_sector, sector)) {
                return 0;
            }
            for (entry_index = 0; entry_index < 16; entry_index++) {
                FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&sector[entry_index * 32];

                if (entry->name[0] == 0 || entry->name[0] == 0xE5) {
                    *sector_number = root_start_sector + root_sector;
                    *entry_result = entry;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void set_entry_name(FAT16_DIRECTORY_ENTRY *entry, const char name[11])
{
    UINT32 index;

    for (index = 0; index < 11; index++) {
        entry->name[index] = (UINT8)name[index];
    }
}

static int component_to_name(const char *component, UINT32 length, char name[11])
{
    UINT32 component_index;
    UINT32 name_index = 0;
    UINT32 extension_index = 8;
    int in_extension = 0;

    if (length == 0) {
        return 0;
    }

    for (component_index = 0; component_index < 11; component_index++) {
        name[component_index] = ' ';
    }

    for (component_index = 0; component_index < length; component_index++) {
        char value = component[component_index];

        if (value >= 'a' && value <= 'z') {
            value = (char)(value - 'a' + 'A');
        }
        if (value == '.') {
            if (in_extension) {
                return 0;
            }
            in_extension = 1;
            continue;
        }
        if ((!in_extension && name_index >= 8) || (in_extension && extension_index >= 11)) {
            return 0;
        }
        if (in_extension) {
            name[extension_index++] = value;
        } else {
            name[name_index++] = value;
        }
    }

    return name_index != 0;
}

static int resolve_parent(const char *path, UINT16 *parent_cluster, char name[11])
{
    char parent_path[64];
    UINT32 last_slash = 0;
    UINT32 length = 0;
    UINT32 index;
    FAT16_DIRECTORY_ENTRY parent;

    if (path[0] != '/') {
        return 0;
    }
    while (path[length] != '\0') {
        if (path[length] == '/') {
            last_slash = length;
        }
        length++;
    }
    if (length <= 1 || !component_to_name(&path[last_slash + 1], length - last_slash - 1, name)) {
        return 0;
    }
    if (last_slash == 0) {
        *parent_cluster = 0;
        return 1;
    }

    for (index = 0; index < last_slash && index + 1 < sizeof(parent_path); index++) {
        parent_path[index] = path[index];
    }
    if (index != last_slash) {
        return 0;
    }
    parent_path[index] = '\0';
    if (!resolve_path(parent_path, &parent) || (parent.attributes & 0x10) == 0) {
        return 0;
    }
    *parent_cluster = parent.first_cluster;
    return 1;
}

static int find_directory_entry_slot(
    UINT16 directory_cluster,
    const char name[11],
    UINT8 sector[512],
    UINT32 *sector_number,
    FAT16_DIRECTORY_ENTRY **entry_result,
    int allow_free
)
{
    if (directory_cluster == 0) {
        return find_root_entry_slot(name, sector, sector_number, entry_result, allow_free);
    }

    {
        UINT16 cluster = directory_cluster;

        while (cluster < 0xFFF8) {
            UINT32 cluster_sector;

            for (cluster_sector = 0; cluster_sector < bpb.sectors_per_cluster; cluster_sector++) {
                UINT32 entry_index;
                UINT32 current_sector =
                    data_start_sector + ((UINT32)cluster - 2) * bpb.sectors_per_cluster + cluster_sector;

                if (!virtio_block_read_sector(current_sector, sector)) {
                    return 0;
                }
                for (entry_index = 0; entry_index < 16; entry_index++) {
                    FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&sector[entry_index * 32];

                    if (entry->name[0] != 0 && entry->name[0] != 0xE5 && name_matches(entry->name, name)) {
                        *sector_number = current_sector;
                        *entry_result = entry;
                        return 1;
                    }
                }
            }
            cluster = fat16_next_cluster(cluster);
        }
    }

    if (allow_free) {
        UINT16 cluster = directory_cluster;

        while (cluster < 0xFFF8) {
            UINT32 cluster_sector;

            for (cluster_sector = 0; cluster_sector < bpb.sectors_per_cluster; cluster_sector++) {
                UINT32 entry_index;
                UINT32 current_sector =
                    data_start_sector + ((UINT32)cluster - 2) * bpb.sectors_per_cluster + cluster_sector;

                if (!virtio_block_read_sector(current_sector, sector)) {
                    return 0;
                }
                for (entry_index = 0; entry_index < 16; entry_index++) {
                    FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&sector[entry_index * 32];

                    if (entry->name[0] == 0 || entry->name[0] == 0xE5) {
                        *sector_number = current_sector;
                        *entry_result = entry;
                        return 1;
                    }
                }
            }
            cluster = fat16_next_cluster(cluster);
        }
    }
    return 0;
}

static int find_in_directory(UINT16 directory_cluster, const char name[11], FAT16_DIRECTORY_ENTRY *result)
{
    UINT8 sector[512];

    if (directory_cluster == 0) {
        UINT32 root_sector;

        for (root_sector = 0; root_sector < root_sector_count; root_sector++) {
            UINT32 entry_index;

            if (!virtio_block_read_sector(root_start_sector + root_sector, sector)) {
                return 0;
            }
            for (entry_index = 0; entry_index < 16; entry_index++) {
                FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&sector[entry_index * 32];

                if (entry->name[0] == 0) {
                    return 0;
                }
                if (entry->name[0] != 0xE5 && entry->attributes != 0x0F && name_matches(entry->name, name)) {
                    copy_entry(result, entry);
                    return 1;
                }
            }
        }
        return 0;
    }

    while (directory_cluster < 0xFFF8) {
        UINT32 cluster_sector;

        for (cluster_sector = 0; cluster_sector < bpb.sectors_per_cluster; cluster_sector++) {
            UINT32 entry_index;

            if (!virtio_block_read_sector(
                data_start_sector + ((UINT32)directory_cluster - 2) * bpb.sectors_per_cluster + cluster_sector,
                sector
            )) {
                return 0;
            }
            for (entry_index = 0; entry_index < 16; entry_index++) {
                FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&sector[entry_index * 32];

                if (entry->name[0] == 0) {
                    return 0;
                }
                if (entry->name[0] != 0xE5 && entry->attributes != 0x0F && name_matches(entry->name, name)) {
                    copy_entry(result, entry);
                    return 1;
                }
            }
        }
        directory_cluster = fat16_next_cluster(directory_cluster);
    }
    return 0;
}

static int resolve_path(const char *path, FAT16_DIRECTORY_ENTRY *result)
{
    UINT16 directory_cluster = 0;
    const char *component;

    if (path[0] != '/') {
        return 0;
    }
    path++;
    if (*path == '\0') {
        return 0;
    }

    while (*path != '\0') {
        char name[11];
        UINT32 length = 0;
        FAT16_DIRECTORY_ENTRY entry;

        component = path;
        while (path[length] != '\0' && path[length] != '/') {
            length++;
        }
        if (!component_to_name(component, length, name) || !find_in_directory(directory_cluster, name, &entry)) {
            return 0;
        }
        path += length;
        if (*path == '/') {
            if ((entry.attributes & 0x10) == 0) {
                return 0;
            }
            directory_cluster = entry.first_cluster;
            path++;
            if (*path == '\0') {
                *result = entry;
                return 1;
            }
        } else {
            *result = entry;
            return 1;
        }
    }
    return 0;
}

static UINT64 read_entry(const FAT16_DIRECTORY_ENTRY *entry, void *buffer, UINT64 capacity)
{
    UINT8 sector[512];
    UINT8 *output = (UINT8 *)buffer;
    UINT16 cluster = entry->first_cluster;
    UINT32 file_size = entry->size;
    UINT64 copied = 0;

    while (cluster < 0xFFF8 && copied < file_size && copied < capacity) {
        UINT32 cluster_sector;

        for (
            cluster_sector = 0;
            cluster_sector < bpb.sectors_per_cluster && copied < file_size && copied < capacity;
            cluster_sector++
        ) {
            UINT64 remaining = file_size - copied;
            UINT64 room = capacity - copied;
            UINT64 copy_size = remaining < 512 ? remaining : 512;
            UINT32 byte_index;

            if (copy_size > room) {
                copy_size = room;
            }
            if (!virtio_block_read_sector(
                data_start_sector + ((UINT32)cluster - 2) * bpb.sectors_per_cluster + cluster_sector,
                sector
            )) {
                return 0;
            }
            for (byte_index = 0; byte_index < copy_size; byte_index++) {
                output[copied + byte_index] = sector[byte_index];
            }
            copied += copy_size;
        }
        cluster = fat16_next_cluster(cluster);
    }
    return copied;
}

int fat16_initialize(void)
{
    UINT8 sector[512];
    FAT16_BPB *disk_bpb;

    if (!virtio_block_read_sector(0, sector)) {
        return 0;
    }

    disk_bpb = (FAT16_BPB *)sector;
    if (disk_bpb->bytes_per_sector != 512 || disk_bpb->sectors_per_cluster == 0) {
        return 0;
    }

    bpb = *disk_bpb;
    root_start_sector = bpb.reserved_sectors + bpb.fat_count * bpb.sectors_per_fat;
    root_sector_count = ((UINT32)bpb.root_entry_count * 32 + 511) / 512;
    data_start_sector = root_start_sector + root_sector_count;
    return 1;
}

UINT64 fat16_root_file_size(const char name[11])
{
    UINT8 sector[512];
    UINT32 root_sector;

    for (root_sector = 0; root_sector < root_sector_count; root_sector++) {
        UINT32 entry_index;

        if (!virtio_block_read_sector(root_start_sector + root_sector, sector)) {
            return 0;
        }

        for (entry_index = 0; entry_index < 16; entry_index++) {
            FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&sector[entry_index * 32];

            if (entry->name[0] == 0) {
                return 0;
            }

            if ((entry->attributes & 0x10) == 0 && name_matches(entry->name, name)) {
                return entry->size;
            }
        }
    }

    return 0;
}

UINT64 fat16_read_root_file(const char name[11], void *buffer, UINT64 capacity)
{
    UINT8 sector[512];
    UINT8 *output = (UINT8 *)buffer;
    UINT32 root_sector;

    for (root_sector = 0; root_sector < root_sector_count; root_sector++) {
        UINT32 entry_index;

        if (!virtio_block_read_sector(root_start_sector + root_sector, sector)) {
            return 0;
        }

        for (entry_index = 0; entry_index < 16; entry_index++) {
            FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&sector[entry_index * 32];

            if (entry->name[0] == 0) {
                return 0;
            }

            if (name_matches(entry->name, name)) {
                UINT16 cluster = entry->first_cluster;
                UINT32 file_size = entry->size;
                UINT64 copied = 0;

                while (cluster < 0xFFF8 && copied < file_size && copied < capacity) {
                    UINT32 cluster_sector;

                    for (
                        cluster_sector = 0;
                        cluster_sector < bpb.sectors_per_cluster && copied < file_size && copied < capacity;
                        cluster_sector++
                    ) {
                        UINT64 remaining = file_size - copied;
                        UINT64 room = capacity - copied;
                        UINT64 copy_size = remaining < 512 ? remaining : 512;
                        UINT32 byte_index;

                        if (copy_size > room) {
                            copy_size = room;
                        }

                        if (!virtio_block_read_sector(
                            data_start_sector +
                                ((UINT32)cluster - 2) * bpb.sectors_per_cluster +
                                cluster_sector,
                            sector
                        )) {
                            logger_write_hex(
                                "ERROR",
                                "FAT data sector read failed",
                                data_start_sector +
                                    ((UINT32)cluster - 2) * bpb.sectors_per_cluster +
                                    cluster_sector
                            );
                            return 0;
                        }

                        for (byte_index = 0; byte_index < copy_size; byte_index++) {
                            output[copied + byte_index] = sector[byte_index];
                        }
                        copied += copy_size;
                    }

                    cluster = fat16_next_cluster(cluster);
                }

                return copied;
            }
        }
    }

    return 0;
}

UINT64 fat16_list_root(FAT16_FILE_INFO *entries, UINT64 capacity)
{
    UINT8 sector[512];
    UINT32 root_sector;
    UINT64 count = 0;

    for (root_sector = 0; root_sector < root_sector_count; root_sector++) {
        UINT32 entry_index;

        if (!virtio_block_read_sector(root_start_sector + root_sector, sector)) {
            return count;
        }

        for (entry_index = 0; entry_index < 16; entry_index++) {
            FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&sector[entry_index * 32];

            if (entry->name[0] == 0) {
                return count;
            }
            if (entry->name[0] == 0xE5 || entry->attributes == 0x0F || (entry->attributes & 0x08) != 0) {
                continue;
            }
            if (count >= capacity) {
                return count;
            }

            format_name(entry->name, entries[count].name);
            entries[count].size = entry->size;
            entries[count].is_directory = (entry->attributes & 0x10) != 0;
            entries[count].read_only = (entry->attributes & 0x01) != 0;
            count++;
        }
    }

    return count;
}

UINT64 fat16_file_size(const char *path)
{
    FAT16_DIRECTORY_ENTRY entry;

    if (!resolve_path(path, &entry) || (entry.attributes & 0x10) != 0) {
        return 0;
    }
    return entry.size;
}

int fat16_is_directory(const char *path)
{
    FAT16_DIRECTORY_ENTRY entry;

    if (path[0] == '/' && path[1] == '\0') {
        return 1;
    }
    return resolve_path(path, &entry) && (entry.attributes & 0x10) != 0;
}

UINT64 fat16_read_file(const char *path, void *buffer, UINT64 capacity)
{
    FAT16_DIRECTORY_ENTRY entry;

    if (!resolve_path(path, &entry) || (entry.attributes & 0x10) != 0) {
        return 0;
    }
    return read_entry(&entry, buffer, capacity);
}

UINT64 fat16_list_directory(const char *path, FAT16_FILE_INFO *entries, UINT64 capacity)
{
    UINT8 sector[512];
    UINT16 cluster;
    UINT64 count = 0;
    FAT16_DIRECTORY_ENTRY directory;

    if (path[0] == '/' && path[1] == '\0') {
        return fat16_list_root(entries, capacity);
    }
    if (!resolve_path(path, &directory) || (directory.attributes & 0x10) == 0) {
        return 0;
    }

    cluster = directory.first_cluster;
    while (cluster < 0xFFF8) {
        UINT32 cluster_sector;

        for (cluster_sector = 0; cluster_sector < bpb.sectors_per_cluster; cluster_sector++) {
            UINT32 entry_index;

            if (!virtio_block_read_sector(
                data_start_sector + ((UINT32)cluster - 2) * bpb.sectors_per_cluster + cluster_sector,
                sector
            )) {
                return count;
            }
            for (entry_index = 0; entry_index < 16; entry_index++) {
                FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&sector[entry_index * 32];

                if (entry->name[0] == 0) {
                    return count;
                }
                if (
                    entry->name[0] == 0xE5 ||
                    entry->attributes == 0x0F ||
                    (entry->attributes & 0x08) != 0 ||
                    entry->name[0] == '.'
                ) {
                    continue;
                }
                if (count >= capacity) {
                    return count;
                }
                format_name(entry->name, entries[count].name);
                entries[count].size = entry->size;
                entries[count].is_directory = (entry->attributes & 0x10) != 0;
                entries[count].read_only = (entry->attributes & 0x01) != 0;
                count++;
            }
        }
        cluster = fat16_next_cluster(cluster);
    }
    return count;
}

int fat16_write_root_file(const char *path, const void *buffer, UINT64 size)
{
    UINT8 root_sector_data[512];
    FAT16_DIRECTORY_ENTRY *target;
    UINT32 target_sector;
    UINT16 old_cluster = 0;
    UINT16 new_cluster = 0;
    UINT32 cluster_size = (UINT32)bpb.sectors_per_cluster * 512U;
    UINT32 cluster_count = (UINT32)((size + cluster_size - 1) / cluster_size);
    UINT16 parent_cluster;
    char name[11];

    if (size > 0xFFFFFFFFULL || !resolve_parent(path, &parent_cluster, name)) {
        return 0;
    }

    if (!find_directory_entry_slot(parent_cluster, name, root_sector_data, &target_sector, &target, 1)) {
        return 0;
    }
    if (target->name[0] != 0 && target->name[0] != 0xE5) {
        if ((target->attributes & 0x11) != 0) {
            return 0;
        }
        old_cluster = target->first_cluster;
    }

    if (cluster_count != 0) {
        new_cluster = fat16_allocate_chain(cluster_count);
        if (new_cluster == 0 || !write_cluster_chain(new_cluster, buffer, size)) {
            if (new_cluster != 0) {
                (void)fat16_free_chain(new_cluster);
            }
            return 0;
        }
    }

    clear_bytes(target, sizeof(FAT16_DIRECTORY_ENTRY));
    set_entry_name(target, name);
    target->attributes = 0x20;
    target->first_cluster = new_cluster;
    target->size = (UINT32)size;
    if (!virtio_block_write_sector(target_sector, root_sector_data)) {
        if (new_cluster != 0) {
            (void)fat16_free_chain(new_cluster);
        }
        return 0;
    }
    return old_cluster == 0 || fat16_free_chain(old_cluster);
}

int fat16_delete_root_file(const char *path)
{
    UINT8 sector[512];
    FAT16_DIRECTORY_ENTRY *entry;
    UINT32 entry_sector;
    UINT16 parent_cluster;
    char name[11];
    if (!resolve_parent(path, &parent_cluster, name)) {
        return 0;
    }

    if (!find_directory_entry_slot(parent_cluster, name, sector, &entry_sector, &entry, 0)) {
        return 0;
    }
    if ((entry->attributes & 0x11) == 0) {
        UINT16 cluster = entry->first_cluster;

        entry->name[0] = 0xE5;
        if (!virtio_block_write_sector(entry_sector, sector)) {
            return 0;
        }
        return cluster == 0 || fat16_free_chain(cluster);
    }
    return 0;
}

int fat16_create_root_directory(const char *path)
{
    UINT8 root_sector_data[512];
    UINT8 directory_sector[512];
    FAT16_DIRECTORY_ENTRY *target;
    UINT32 target_sector;
    UINT16 cluster;
    UINT16 parent_cluster;
    char name[11];

    if (!resolve_parent(path, &parent_cluster, name)) {
        return 0;
    }
    if (!find_directory_entry_slot(parent_cluster, name, root_sector_data, &target_sector, &target, 1)) {
        return 0;
    }
    if (target->name[0] != 0 && target->name[0] != 0xE5) {
        return 0;
    }
    cluster = fat16_allocate_cluster();
    if (cluster == 0) {
        return 0;
    }

    clear_bytes(directory_sector, sizeof(directory_sector));
    {
        FAT16_DIRECTORY_ENTRY *dot = (FAT16_DIRECTORY_ENTRY *)&directory_sector[0];
        FAT16_DIRECTORY_ENTRY *dotdot = (FAT16_DIRECTORY_ENTRY *)&directory_sector[32];

        clear_bytes(dot, sizeof(FAT16_DIRECTORY_ENTRY));
        clear_bytes(dotdot, sizeof(FAT16_DIRECTORY_ENTRY));
        dot->name[0] = '.';
        dot->name[1] = ' ';
        dot->attributes = 0x10;
        dot->first_cluster = cluster;
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';
        dotdot->attributes = 0x10;
        dotdot->first_cluster = parent_cluster;
    }
    if (!virtio_block_write_sector(data_start_sector + ((UINT32)cluster - 2), directory_sector)) {
        (void)fat16_free_chain(cluster);
        return 0;
    }

    clear_bytes(target, sizeof(FAT16_DIRECTORY_ENTRY));
    set_entry_name(target, name);
    target->attributes = 0x10;
    target->first_cluster = cluster;
    if (!virtio_block_write_sector(target_sector, root_sector_data)) {
        (void)fat16_free_chain(cluster);
        return 0;
    }
    return 1;
}

int fat16_delete_root_directory(const char *path)
{
    UINT8 root_sector_data[512];
    UINT8 directory_sector[512];
    FAT16_DIRECTORY_ENTRY *target;
    UINT32 target_sector;
    UINT32 index;
    UINT16 parent_cluster;
    char name[11];

    if (!resolve_parent(path, &parent_cluster, name)) {
        return 0;
    }
    if (!find_directory_entry_slot(parent_cluster, name, root_sector_data, &target_sector, &target, 0)) {
        return 0;
    }
    if ((target->attributes & 0x10) == 0 || target->first_cluster < 2) {
        return 0;
    }
    if (!virtio_block_read_sector(data_start_sector + ((UINT32)target->first_cluster - 2), directory_sector)) {
        return 0;
    }
    for (index = 2; index < 16; index++) {
        FAT16_DIRECTORY_ENTRY *entry = (FAT16_DIRECTORY_ENTRY *)&directory_sector[index * 32];

        if (entry->name[0] != 0 && entry->name[0] != 0xE5) {
            return 0;
        }
    }
    {
        UINT16 cluster = target->first_cluster;
        target->name[0] = 0xE5;
        if (!virtio_block_write_sector(target_sector, root_sector_data)) {
            return 0;
        }
        return fat16_free_chain(cluster);
    }
}

int fat16_self_test(void)
{
    UINT8 buffer[64];
    FAT16_FILE_INFO entries[8];
    static const char disk_name[11] = { 'D','I','S','K',' ',' ',' ',' ','T','X','T' };
    UINT64 bytes;
    UINT64 count;
    UINT64 nested_count;

    if (!fat16_initialize()) {
        return 0;
    }

    bytes = fat16_read_root_file(disk_name, buffer, sizeof(buffer));
    count = fat16_list_root(entries, 8);
    nested_count = fat16_list_directory("/ASAS", entries, 8);
    return bytes > 10 && buffer[0] == 'A' && buffer[5] == 'O' && count >= 4 && nested_count >= 3;
}

/* ---- Multi-device mount ---- */
static UINT8 fat16_mounted_target = 0xFFU;
static UINT8 fat16_mounted_lun    = 0xFFU;

int fat16_mount_device(UINT8 target, UINT8 lun)
{
    if (target == fat16_mounted_target && lun == fat16_mounted_lun) {
        return 1; /* already mounted, BPB still valid */
    }
    virtio_block_select_device(target, lun);
    if (!fat16_initialize()) {
        return 0;
    }
    fat16_mounted_target = target;
    fat16_mounted_lun    = lun;
    return 1;
}
