#include "ext2.h"
#include "heap.h"

#define EXT2_SUPER_OFFSET 1024U
#define EXT2_SUPER_SIZE 1024U
#define EXT2_MAGIC 0xEF53U
#define EXT2_ROOT_INODE 2U
#define EXT2_N_BLOCKS 15U
#define EXT2_NAME_LEN 255U
#define EXT2_MAX_BLOCK_SIZE 4096U
#define EXT2_MAX_EXTENTS 64U

#define EXT2_S_IFMT  0xF000U
#define EXT2_S_IFREG 0x8000U
#define EXT2_S_IFDIR 0x4000U

#define EXT2_STATE_CLEAN 0x0001U

#define EXT2_FEATURE_COMPAT_DIR_PREALLOC 0x0001U
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL  0x0004U
#define EXT2_FEATURE_COMPAT_EXT_ATTR     0x0008U
#define EXT2_FEATURE_COMPAT_RESIZE_INO   0x0010U
#define EXT2_FEATURE_COMPAT_DIR_INDEX    0x0020U

#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define EXT2_FEATURE_INCOMPAT_EXTENTS  0x0040U
#define EXT2_FEATURE_INCOMPAT_64BIT    0x0080U
#define EXT2_FEATURE_INCOMPAT_FLEX_BG  0x0200U

#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001U
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE   0x0002U
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR    0x0004U
#define EXT2_FEATURE_RO_COMPAT_HUGE_FILE    0x0008U
#define EXT2_FEATURE_RO_COMPAT_GDT_CSUM     0x0010U
#define EXT2_FEATURE_RO_COMPAT_DIR_NLINK    0x0020U
#define EXT2_FEATURE_RO_COMPAT_EXTRA_ISIZE  0x0040U
#define EXT2_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400U

#define EXT4_EXTENTS_FL 0x00080000U
#define EXT4_EXTENT_MAGIC 0xF30AU

typedef struct {
    UINT32 mode;
    UINT32 flags;
    UINT64 size;
    UINT32 blocks[EXT2_N_BLOCKS];
} EXT2_INODE;

typedef struct {
    UINT64 block;
    UINT32 length;
} EXT2_EXTENT;

struct EXT2_CONTEXT {
    ASAS_BLOCK_DEVICE *device;
    UINT32 block_size;
    UINT32 inode_size;
    UINT32 inodes_per_group;
    UINT32 blocks_per_group;
    UINT32 group_count;
    UINT32 descriptor_size;
    UINT32 feature_compat;
    UINT32 feature_incompat;
    UINT32 feature_ro_compat;
    UINT16 state;
    UINT8 read_only_reason;
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

static UINT64 read_le64_lohi(UINT32 low, UINT32 high)
{
    return (UINT64)low | ((UINT64)high << 32);
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

static int read_bytes(EXT2_CONTEXT *context, UINT64 byte_offset,
                      void *buffer, UINT32 size)
{
    UINT8 sector[4096];
    UINT8 *out = (UINT8 *)buffer;
    UINT32 done = 0;
    if (context == 0 || context->device == 0 || buffer == 0 ||
        context->device->logical_block_size < 512U ||
        context->device->logical_block_size > sizeof(sector)) return 0;
    while (done < size) {
        UINT64 absolute = byte_offset + done;
        UINT64 lba = absolute / context->device->logical_block_size;
        UINT32 skip = (UINT32)(absolute % context->device->logical_block_size);
        UINT32 chunk = context->device->logical_block_size - skip;
        UINT32 index;
        if (chunk > size - done) chunk = size - done;
        if (!block_device_read(context->device, lba, 1, sector)) return 0;
        for (index = 0; index < chunk; index++)
            out[done + index] = sector[skip + index];
        done += chunk;
    }
    return 1;
}

static int read_block(EXT2_CONTEXT *context, UINT64 block, void *buffer)
{
    return read_bytes(context, block * (UINT64)context->block_size,
                      buffer, context->block_size);
}

static int load_super(EXT2_CONTEXT *context)
{
    UINT8 super[EXT2_SUPER_SIZE];
    UINT32 blocks_count;
    UINT32 log_block_size;
    UINT32 supported_compat;
    UINT32 supported_incompat;
    UINT32 supported_ro;
    if (!read_bytes(context, EXT2_SUPER_OFFSET, super, sizeof(super)) ||
        read_le16(super + 56U) != EXT2_MAGIC) return 0;
    blocks_count = read_le32(super + 4U);
    log_block_size = read_le32(super + 24U);
    if (log_block_size > 2U) return 0;
    context->block_size = 1024U << log_block_size;
    context->blocks_per_group = read_le32(super + 32U);
    context->inodes_per_group = read_le32(super + 40U);
    context->state = read_le16(super + 58U);
    context->feature_compat = read_le32(super + 92U);
    context->feature_incompat = read_le32(super + 96U);
    context->feature_ro_compat = read_le32(super + 100U);
    context->inode_size = read_le32(super + 76U) >= 1U
        ? read_le16(super + 88U) : 128U;
    context->descriptor_size = 32U;
    if ((context->feature_incompat & EXT2_FEATURE_INCOMPAT_64BIT) != 0) {
        context->descriptor_size = read_le16(super + 254U);
        if (context->descriptor_size < 64U) return 0;
    }
    if (context->inode_size < 128U || context->inode_size > context->block_size ||
        context->blocks_per_group == 0 || context->inodes_per_group == 0 ||
        blocks_count == 0) return 0;
    context->group_count =
        (blocks_count + context->blocks_per_group - 1U) /
        context->blocks_per_group;

    supported_compat = EXT2_FEATURE_COMPAT_DIR_PREALLOC |
                       EXT2_FEATURE_COMPAT_HAS_JOURNAL |
                       EXT2_FEATURE_COMPAT_EXT_ATTR |
                       EXT2_FEATURE_COMPAT_RESIZE_INO |
                       EXT2_FEATURE_COMPAT_DIR_INDEX;
    supported_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE |
                         EXT2_FEATURE_INCOMPAT_EXTENTS |
                         EXT2_FEATURE_INCOMPAT_64BIT |
                         EXT2_FEATURE_INCOMPAT_FLEX_BG;
    supported_ro = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
                   EXT2_FEATURE_RO_COMPAT_LARGE_FILE |
                   EXT2_FEATURE_RO_COMPAT_BTREE_DIR |
                   EXT2_FEATURE_RO_COMPAT_HUGE_FILE |
                   EXT2_FEATURE_RO_COMPAT_GDT_CSUM |
                   EXT2_FEATURE_RO_COMPAT_DIR_NLINK |
                   EXT2_FEATURE_RO_COMPAT_EXTRA_ISIZE;
    if ((context->feature_compat & ~supported_compat) != 0 ||
        (context->feature_incompat & ~supported_incompat) != 0 ||
        (context->feature_ro_compat & ~supported_ro) != 0) return 0;
    context->read_only_reason =
        (context->state & EXT2_STATE_CLEAN) == 0 ||
        (context->feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) != 0 ||
        (context->feature_ro_compat & EXT2_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
    return 1;
}

static UINT64 descriptor_table_block(EXT2_CONTEXT *context)
{
    return context->block_size == 1024U ? 2U : 1U;
}

static int inode_table_block(EXT2_CONTEXT *context, UINT32 group,
                             UINT64 *block)
{
    UINT8 descriptor[64];
    UINT64 offset;
    UINT32 low;
    UINT32 high = 0;
    if (context == 0 || block == 0 || group >= context->group_count)
        return 0;
    offset = descriptor_table_block(context) * (UINT64)context->block_size +
             group * (UINT64)context->descriptor_size;
    if (!read_bytes(context, offset, descriptor, context->descriptor_size))
        return 0;
    low = read_le32(descriptor + 8U);
    if (context->descriptor_size >= 64U)
        high = read_le32(descriptor + 40U);
    *block = read_le64_lohi(low, high);
    return *block != 0;
}

static int read_inode(EXT2_CONTEXT *context, UINT32 inode_number,
                      EXT2_INODE *inode)
{
    UINT8 raw[512];
    UINT32 group;
    UINT32 index;
    UINT64 table;
    UINT64 offset;
    UINT32 block_index;
    if (context == 0 || inode == 0 || inode_number == 0 ||
        context->inode_size > sizeof(raw)) return 0;
    group = (inode_number - 1U) / context->inodes_per_group;
    index = (inode_number - 1U) % context->inodes_per_group;
    if (!inode_table_block(context, group, &table)) return 0;
    offset = table * (UINT64)context->block_size +
             index * (UINT64)context->inode_size;
    if (!read_bytes(context, offset, raw, context->inode_size)) return 0;
    inode->mode = read_le16(raw);
    inode->size = read_le64_lohi(read_le32(raw + 4U), read_le32(raw + 108U));
    inode->flags = read_le32(raw + 32U);
    for (block_index = 0; block_index < EXT2_N_BLOCKS; block_index++)
        inode->blocks[block_index] = read_le32(raw + 40U + block_index * 4U);
    return inode->mode != 0;
}

static UINT32 inode_type(const EXT2_INODE *inode)
{
    return inode == 0 ? 0 : (inode->mode & EXT2_S_IFMT);
}

static int collect_extent_tree(EXT2_CONTEXT *context, UINT64 block,
                               EXT2_EXTENT *extents, UINT32 *count,
                               UINT32 depth)
{
    UINT8 buffer[EXT2_MAX_BLOCK_SIZE];
    UINT16 entries;
    UINT16 index;
    if (context == 0 || extents == 0 || count == 0 ||
        depth > 5U || !read_block(context, block, buffer) ||
        read_le16(buffer) != EXT4_EXTENT_MAGIC) return 0;
    entries = read_le16(buffer + 2U);
    if (entries > read_le16(buffer + 4U)) return 0;
    if (read_le16(buffer + 6U) == 0) {
        for (index = 0; index < entries; index++) {
            UINT32 offset = 12U + index * 12U;
            UINT16 length;
            UINT64 start;
            if (*count >= EXT2_MAX_EXTENTS) return 0;
            length = read_le16(buffer + offset + 4U) & 0x7FFFU;
            start = ((UINT64)read_le16(buffer + offset + 6U) << 32) |
                    read_le32(buffer + offset + 8U);
            extents[*count].block = start;
            extents[*count].length = (UINT32)length * context->block_size;
            (*count)++;
        }
        return 1;
    }
    for (index = 0; index < entries; index++) {
        UINT32 offset = 12U + index * 12U;
        UINT64 child = ((UINT64)read_le16(buffer + offset + 10U) << 32) |
                       read_le32(buffer + offset + 4U);
        if (!collect_extent_tree(context, child, extents, count, depth + 1U))
            return 0;
    }
    return 1;
}

static int collect_inode_extents(EXT2_CONTEXT *context, const EXT2_INODE *inode,
                                 EXT2_EXTENT *extents, UINT32 *count)
{
    const UINT8 *root = (const UINT8 *)inode->blocks;
    UINT16 entries;
    UINT16 index;
    if (context == 0 || inode == 0 || extents == 0 || count == 0 ||
        (inode->flags & EXT4_EXTENTS_FL) == 0 ||
        read_le16(root) != EXT4_EXTENT_MAGIC) return 0;
    *count = 0;
    entries = read_le16(root + 2U);
    if (entries > read_le16(root + 4U)) return 0;
    if (read_le16(root + 6U) == 0) {
        for (index = 0; index < entries; index++) {
            UINT32 offset = 12U + index * 12U;
            UINT16 length;
            UINT64 start;
            if (*count >= EXT2_MAX_EXTENTS) return 0;
            length = read_le16(root + offset + 4U) & 0x7FFFU;
            start = ((UINT64)read_le16(root + offset + 6U) << 32) |
                    read_le32(root + offset + 8U);
            extents[*count].block = start;
            extents[*count].length = (UINT32)length * context->block_size;
            (*count)++;
        }
        return 1;
    }
    for (index = 0; index < entries; index++) {
        UINT32 offset = 12U + index * 12U;
        UINT64 child = ((UINT64)read_le16(root + offset + 10U) << 32) |
                       read_le32(root + offset + 4U);
        if (!collect_extent_tree(context, child, extents, count, 1U))
            return 0;
    }
    return 1;
}

static int read_inode_bytes(EXT2_CONTEXT *context, const EXT2_INODE *inode,
                            UINT64 offset, void *buffer, UINT64 capacity)
{
    UINT8 block[EXT2_MAX_BLOCK_SIZE];
    UINT8 indirect[EXT2_MAX_BLOCK_SIZE];
    UINT8 *out = (UINT8 *)buffer;
    UINT64 done = 0;
    if (context == 0 || inode == 0 || buffer == 0 || offset > inode->size)
        return 0;
    if (capacity > inode->size - offset) capacity = inode->size - offset;
    if ((inode->flags & EXT4_EXTENTS_FL) != 0) {
        EXT2_EXTENT extents[EXT2_MAX_EXTENTS];
        UINT32 count = 0;
        UINT32 index;
        if (!collect_inode_extents(context, inode, extents, &count)) return 0;
        for (index = 0; index < count && done < capacity; index++) {
            UINT64 length = extents[index].length;
            if (offset >= length) {
                offset -= length;
                continue;
            }
            while (offset < length && done < capacity) {
                UINT64 block_index = offset / context->block_size;
                UINT32 in_block = (UINT32)(offset % context->block_size);
                UINT32 chunk = context->block_size - in_block;
                UINT32 copy_index;
                if ((UINT64)chunk > capacity - done)
                    chunk = (UINT32)(capacity - done);
                if ((UINT64)chunk > length - offset)
                    chunk = (UINT32)(length - offset);
                if (!read_block(context, extents[index].block + block_index,
                                block)) return 0;
                for (copy_index = 0; copy_index < chunk; copy_index++)
                    out[done + copy_index] = block[in_block + copy_index];
                done += chunk;
                offset += chunk;
            }
            offset = 0;
        }
        return done == capacity;
    }
    while (done < capacity) {
        UINT64 logical = (offset + done) / context->block_size;
        UINT32 in_block = (UINT32)((offset + done) % context->block_size);
        UINT32 physical = 0;
        UINT32 chunk = context->block_size - in_block;
        UINT32 copy_index;
        if ((UINT64)chunk > capacity - done) chunk = (UINT32)(capacity - done);
        if (logical < 12U) {
            physical = inode->blocks[(UINT32)logical];
        } else {
            UINT32 indirect_index = (UINT32)(logical - 12U);
            if (indirect_index >= context->block_size / 4U ||
                inode->blocks[12] == 0 ||
                !read_block(context, inode->blocks[12], indirect)) return 0;
            physical = read_le32(indirect + indirect_index * 4U);
        }
        if (physical == 0) {
            for (copy_index = 0; copy_index < chunk; copy_index++)
                out[done + copy_index] = 0;
        } else {
            if (!read_block(context, physical, block)) return 0;
            for (copy_index = 0; copy_index < chunk; copy_index++)
                out[done + copy_index] = block[in_block + copy_index];
        }
        done += chunk;
    }
    return 1;
}

static int find_in_directory(EXT2_CONTEXT *context, const EXT2_INODE *directory,
                             const char *name, EXT2_INODE *result)
{
    UINT8 block[EXT2_MAX_BLOCK_SIZE];
    UINT64 offset = 0;
    if (context == 0 || directory == 0 || name == 0 || result == 0 ||
        inode_type(directory) != EXT2_S_IFDIR) return 0;
    while (offset < directory->size) {
        UINT32 chunk = context->block_size;
        UINT32 cursor = 0;
        if ((UINT64)chunk > directory->size - offset)
            chunk = (UINT32)(directory->size - offset);
        if (!read_inode_bytes(context, directory, offset, block, chunk))
            return 0;
        while (cursor + 8U <= chunk) {
            UINT32 inode_number = read_le32(block + cursor);
            UINT16 rec_len = read_le16(block + cursor + 4U);
            UINT8 name_len = block[cursor + 6U];
            char entry_name[256];
            UINT32 index;
            if (rec_len < 8U || cursor + rec_len > chunk) break;
            if (inode_number != 0 && name_len != 0 &&
                name_len <= EXT2_NAME_LEN && 8U + name_len <= rec_len) {
                for (index = 0; index < name_len && index + 1U < 256U; index++)
                    entry_name[index] = (char)block[cursor + 8U + index];
                entry_name[index] = '\0';
                if (strings_equal_ci(entry_name, name))
                    return read_inode(context, inode_number, result);
            }
            cursor += rec_len;
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

static int find_inode(EXT2_CONTEXT *context, const char *path,
                      EXT2_INODE *inode)
{
    EXT2_INODE current;
    char component[256];
    const char *cursor = path;
    if (context == 0 || path == 0 || inode == 0 ||
        !read_inode(context, EXT2_ROOT_INODE, &current)) return 0;
    while (next_path_component(&cursor, component)) {
        EXT2_INODE next;
        if (!find_in_directory(context, &current, component, &next)) return 0;
        current = next;
    }
    *inode = current;
    return 1;
}

int ext2_probe(ASAS_BLOCK_DEVICE *device)
{
    EXT2_CONTEXT context;
    if (device == 0 || device->logical_block_size < 512U ||
        device->logical_block_size > 4096U) return 0;
    context.device = device;
    context.block_size = 1024U;
    return load_super(&context);
}

EXT2_CONTEXT *ext2_context_create(ASAS_BLOCK_DEVICE *device)
{
    EXT2_CONTEXT *context;
    if (device == 0) return 0;
    context = (EXT2_CONTEXT *)kmalloc(sizeof(EXT2_CONTEXT));
    if (context == 0) return 0;
    context->device = device;
    context->block_size = 1024U;
    context->read_only_reason = 1;
    if (!load_super(context)) {
        kfree(context);
        return 0;
    }
    return context;
}

void ext2_context_destroy(EXT2_CONTEXT *context)
{
    if (context != 0) kfree(context);
}

int ext2_context_exists(EXT2_CONTEXT *context, const char *path)
{
    EXT2_INODE inode;
    return find_inode(context, path, &inode);
}

int ext2_context_is_directory(EXT2_CONTEXT *context, const char *path)
{
    EXT2_INODE inode;
    return find_inode(context, path, &inode) &&
           inode_type(&inode) == EXT2_S_IFDIR;
}

int ext2_context_is_read_only(EXT2_CONTEXT *context)
{
    return context == 0 || context->read_only_reason != 0;
}

UINT64 ext2_context_file_size(EXT2_CONTEXT *context, const char *path)
{
    EXT2_INODE inode;
    if (!find_inode(context, path, &inode) ||
        inode_type(&inode) == EXT2_S_IFDIR) return 0;
    return inode.size;
}

UINT64 ext2_context_read_file(EXT2_CONTEXT *context, const char *path,
                              void *buffer, UINT64 capacity)
{
    EXT2_INODE inode;
    UINT64 bytes;
    if (buffer == 0 || !find_inode(context, path, &inode) ||
        inode_type(&inode) != EXT2_S_IFREG) return 0;
    bytes = capacity < inode.size ? capacity : inode.size;
    if (bytes == 0) return 0;
    return read_inode_bytes(context, &inode, 0, buffer, bytes) ? bytes : 0;
}

UINT64 ext2_context_list_directory(EXT2_CONTEXT *context, const char *path,
                                   EXT2_FILE_INFO *entries, UINT64 capacity)
{
    EXT2_INODE directory;
    UINT8 block[EXT2_MAX_BLOCK_SIZE];
    UINT64 offset = 0;
    UINT64 count = 0;
    if (entries == 0 || capacity == 0 ||
        !find_inode(context, path, &directory) ||
        inode_type(&directory) != EXT2_S_IFDIR) return 0;
    while (offset < directory.size && count < capacity) {
        UINT32 chunk = context->block_size;
        UINT32 cursor = 0;
        if ((UINT64)chunk > directory.size - offset)
            chunk = (UINT32)(directory.size - offset);
        if (!read_inode_bytes(context, &directory, offset, block, chunk))
            return count;
        while (cursor + 8U <= chunk && count < capacity) {
            UINT32 inode_number = read_le32(block + cursor);
            UINT16 rec_len = read_le16(block + cursor + 4U);
            UINT8 name_len = block[cursor + 6U];
            if (rec_len < 8U || cursor + rec_len > chunk) break;
            if (inode_number != 0 && name_len != 0 &&
                name_len <= EXT2_NAME_LEN && 8U + name_len <= rec_len) {
                EXT2_INODE child;
                UINT32 index;
                for (index = 0; index < name_len && index + 1U < 256U; index++)
                    entries[count].name[index] =
                        (char)block[cursor + 8U + index];
                entries[count].name[index] = '\0';
                if (read_inode(context, inode_number, &child)) {
                    entries[count].is_directory =
                        inode_type(&child) == EXT2_S_IFDIR;
                    entries[count].size = entries[count].is_directory
                        ? 0 : child.size;
                    entries[count].read_only =
                        (UINT8)ext2_context_is_read_only(context);
                    count++;
                }
            }
            cursor += rec_len;
        }
        offset += chunk;
    }
    return count;
}

int ext2_context_write_file(EXT2_CONTEXT *context, const char *path,
                            const void *buffer, UINT64 size)
{
    (void)context; (void)path; (void)buffer; (void)size;
    return 0;
}

int ext2_context_delete_file(EXT2_CONTEXT *context, const char *path)
{
    (void)context; (void)path;
    return 0;
}

int ext2_context_create_directory(EXT2_CONTEXT *context, const char *path)
{
    (void)context; (void)path;
    return 0;
}

int ext2_context_delete_directory(EXT2_CONTEXT *context, const char *path)
{
    (void)context; (void)path;
    return 0;
}

int ext2_context_rename(EXT2_CONTEXT *context, const char *source,
                        const char *destination)
{
    (void)context; (void)source; (void)destination;
    return 0;
}

int ext2_context_sync(EXT2_CONTEXT *context)
{
    if (context == 0 || context->device == 0) return 0;
    return block_device_flush(context->device);
}
