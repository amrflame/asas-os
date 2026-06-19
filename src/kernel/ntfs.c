/*
 * ntfs.c -- NTFS driver for Asas OS
 *
 * Supports:
 *   - NTFS volume detection and initialization
 *   - Reading the MFT via the MFT's own runlist
 *   - Listing directories through INDEX_ROOT and INDEX_ALLOCATION
 *   - Reading resident, multi-extent, sparse, and compressed file data
 *   - UTF-16LE to UTF-8 name conversion
 *
 * Limitations:
 *   - Writes are limited to existing unnamed streams within allocated capacity
 *   - Encrypted streams are rejected
 *   - Compression support is read-only (LZNT1)
 */

#include "ntfs.h"
#include "virtio_block.h"
#include "logger.h"
#include "block_device.h"
#include "heap.h"

/* Signed types not in uefi.h */
typedef signed char        INT8;
typedef long long          INT64;

/* -----------------------------------------------------------------------
 * Packed on-disk structures
 * --------------------------------------------------------------------- */
#pragma pack(push, 1)

/* Relevant fields extracted from the NTFS boot sector */
typedef struct {
    UINT8  jmp[3];
    UINT8  oem[8];              /* "NTFS    " */
    UINT16 bytes_per_sector;
    UINT8  sectors_per_cluster;
    UINT8  unused[25];
    UINT64 total_sectors;
    UINT64 mft_lcn;
    UINT64 mft_mirror_lcn;
    INT8   clusters_per_file_record; /* negative: record = 2^(-n) bytes */
    UINT8  pad_cfr[3];
    INT8   clusters_per_index_record;
    UINT8  pad_cir[3];
    UINT64 volume_serial;
} NTFS_BOOT;

/* MFT file record header */
typedef struct {
    UINT8  signature[4];    /* "FILE" */
    UINT16 fixup_offset;
    UINT16 fixup_count;
    UINT64 lsn;
    UINT16 sequence;
    UINT16 hard_links;
    UINT16 attr_offset;
    UINT16 flags;           /* 0x01=in-use, 0x02=directory */
    UINT32 used_size;
    UINT32 alloc_size;
    UINT64 base_record;
    UINT16 next_attr_id;
    UINT16 pad;
    UINT32 record_number;
} NTFS_FILE_RECORD;

/* Attribute header (common part) */
typedef struct {
    UINT32 type;
    UINT32 length;
    UINT8  non_resident;
    UINT8  name_length;
    UINT16 name_offset;
    UINT16 flags;
    UINT16 id;
} NTFS_ATTR_HEADER;  /* 16 bytes */

/* Resident attribute extension (follows NTFS_ATTR_HEADER) */
typedef struct {
    UINT32 value_length;
    UINT16 value_offset;
    UINT16 attr_flags;
} NTFS_RESIDENT;

/* Non-resident attribute extension (follows NTFS_ATTR_HEADER) */
typedef struct {
    UINT64 lowest_vcn;
    UINT64 highest_vcn;
    UINT16 runlist_offset;
    UINT16 compression_unit;
    UINT32 pad;
    UINT64 alloc_size;
    UINT64 data_size;
    UINT64 init_size;
} NTFS_NONRESIDENT;

/* $FILE_NAME attribute value */
typedef struct {
    UINT64 parent_ref;
    UINT64 create_time;
    UINT64 change_time;
    UINT64 mft_change_time;
    UINT64 access_time;
    UINT64 alloc_size;
    UINT64 real_size;
    UINT32 file_flags;      /* 0x10000000=directory, 0x00000001=read-only */
    UINT32 reparse_tag;
    UINT8  name_length;     /* in UTF-16 characters */
    UINT8  name_space;      /* 0=POSIX,1=Win32,2=DOS,3=Win32&DOS */
    /* UTF-16LE name follows immediately */
} NTFS_FILE_NAME;   /* 66 bytes = 0x42 */

typedef struct {
    UINT64 create_time;
    UINT64 change_time;
    UINT64 mft_change_time;
    UINT64 access_time;
    UINT32 file_attributes;
    UINT32 max_versions;
    UINT32 version;
    UINT32 class_id;
} NTFS_STANDARD_INFORMATION;

/* Index entry for $FILE_NAME index ($I30) */
typedef struct {
    UINT64 mft_ref;         /* lower 48 bits = record number */
    UINT16 length;
    UINT16 key_length;
    UINT8  flags;           /* 0x01=has sub-node, 0x02=last entry */
    UINT8  reserved[3];
    /* $FILE_NAME key follows if key_length > 0 */
} NTFS_INDEX_ENTRY;  /* 16 bytes */

/* Index root attribute value header */
typedef struct {
    UINT32 attr_type;           /* 0x30 = $FILE_NAME */
    UINT32 collation_rule;
    UINT32 bytes_per_ie;
    UINT8  clusters_per_ie;
    UINT8  pad[3];
    /* NTFS_INDEX_HEADER follows */
} NTFS_INDEX_ROOT;

/* Index header (directly after NTFS_INDEX_ROOT in the value) */
typedef struct {
    UINT32 first_entry_offset;  /* relative to start of this struct */
    UINT32 total_size;
    UINT32 alloc_size;
    UINT8  flags;
    UINT8  pad[3];
} NTFS_INDEX_HEADER;

typedef struct {
    UINT8 signature[4];
    UINT16 fixup_offset;
    UINT16 fixup_count;
    UINT64 lsn;
    UINT64 vcn;
} NTFS_INDEX_BLOCK;

typedef struct {
    UINT32 type;
    UINT16 length;
    UINT8 name_length;
    UINT8 name_offset;
    UINT64 lowest_vcn;
    UINT64 file_reference;
    UINT16 attribute_id;
} NTFS_ATTRIBUTE_LIST_ENTRY;

#pragma pack(pop)

#define ATTR_STANDARD_INFORMATION 0x10u
#define ATTR_ATTRIBUTE_LIST 0x20u
#define ATTR_FILE_NAME  0x30u
#define ATTR_DATA       0x80u
#define ATTR_VOLUME_INFORMATION 0x70u
#define ATTR_INDEX_ROOT 0x90u
#define ATTR_INDEX_ALLOCATION 0xA0u
#define ATTR_BITMAP     0xB0u
#define ATTR_END        0xFFFFFFFFu

#define MFT_RECORD_ROOT 5u
#define NTFS_MAX_SECTOR_SIZE 4096U

/* -----------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------- */
#define MAX_RUNS 128
#define NTFS_MAX_STREAM_RUNS 256
#define NTFS_MAX_ATTRIBUTE_LIST_SIZE (1024U * 1024U)
#define NTFS_MAX_COMPRESSION_UNIT_SIZE (1024U * 1024U)

typedef struct { UINT64 lcn; UINT64 len; UINT8 sparse; } NTFS_RUN;

typedef struct NTFS_TRANSACTION_ENTRY NTFS_TRANSACTION_ENTRY;
typedef struct NTFS_TRANSACTION NTFS_TRANSACTION;

struct NTFS_TRANSACTION_ENTRY {
    UINT64 lba;
    UINT8 *before;
    NTFS_TRANSACTION_ENTRY *previous;
};

struct NTFS_TRANSACTION {
    NTFS_TRANSACTION_ENTRY *last;
    UINT32 entry_count;
    UINT32 barrier_count;
    UINT32 fail_barrier;
    UINT8 active;
    UINT8 rolling_back;
};

typedef struct {
    NTFS_RUN runs[NTFS_MAX_STREAM_RUNS];
    UINT32 run_count;
    UINT64 data_size;
    UINT64 allocated_size;
    UINT64 initialized_size;
    UINT64 next_vcn;
    const UINT8 *resident_value;
    UINT32 resident_length;
    UINT16 flags;
    UINT16 compression_unit;
    UINT8 resident;
    UINT8 found;
} NTFS_STREAM;

struct NTFS_CONTEXT {
    ASAS_BLOCK_DEVICE *device;
    NTFS_BOOT boot;
    UINT32 bytes_per_cluster;
    UINT32 bytes_per_sector;
    UINT32 mft_record_size;
    UINT32 index_record_size;
    UINT64 mft_data_size;
    NTFS_RUN mft_runs[MAX_RUNS];
    UINT32 mft_run_count;
    int initialized;
    UINT8 write_allowed;
    NTFS_TRANSACTION *transaction;
    UINT16 *upcase;
    UINT32 upcase_count;
    NTFS_READ_ONLY_REASON read_only_reason;
    UINT16 volume_flags;
    UINT8 mft_buf[4096];
    UINT8 write_buf[4096];
};

static NTFS_CONTEXT legacy_context;
static UINT8 ntfs_test_4k_record[4096];
static NTFS_STREAM ntfs_test_stream;
static UINT8 ntfs_test_disk[8 * 512];
static const char *ntfs_last_error = "ntfs: no recent error";

static void ntfs_set_error(const char *message)
{
    ntfs_last_error = message;
}

static int ntfs_test_device_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                                 UINT32 count, void *buffer)
{
    UINT32 index;
    (void)device;
    if (lba >= 8 || count > 8U - (UINT32)lba) return 0;
    for (index = 0; index < count * 512U; index++)
        ((UINT8 *)buffer)[index] =
            ntfs_test_disk[(UINT32)lba * 512U + index];
    return 1;
}

static int ntfs_test_device_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                                  UINT32 count, const void *buffer)
{
    UINT32 index;
    (void)device;
    if (lba >= 8 || count > 8U - (UINT32)lba) return 0;
    for (index = 0; index < count * 512U; index++)
        ntfs_test_disk[(UINT32)lba * 512U + index] =
            ((const UINT8 *)buffer)[index];
    return 1;
}

static int ntfs_test_device_flush(ASAS_BLOCK_DEVICE *device)
{
    (void)device;
    return 1;
}

static const ASAS_BLOCK_DEVICE_OPS ntfs_test_device_ops = {
    ntfs_test_device_read,
    ntfs_test_device_write,
    ntfs_test_device_flush
};

/* Single working buffer — large enough for any MFT record (usually 1 KB) */
#define MFT_BUF_SIZE 4096
static int ntfs_read_sector(NTFS_CONTEXT *context, UINT64 lba, void *buffer)
{
    if (context->device != 0) {
        return block_device_read(context->device, lba, 1, buffer);
    }
    return virtio_block_read_sector(lba, buffer);
}

static int ntfs_write_sector_raw(NTFS_CONTEXT *context, UINT64 lba,
                                 const void *buffer)
{
    if (context->device != 0) {
        return block_device_write(context->device, lba, 1, buffer);
    }
    return virtio_block_write_sector(lba, buffer);
}

static NTFS_TRANSACTION_ENTRY *ntfs_transaction_find(
    NTFS_TRANSACTION *transaction, UINT64 lba)
{
    NTFS_TRANSACTION_ENTRY *entry = transaction->last;
    while (entry != 0) {
        if (entry->lba == lba) return entry;
        entry = entry->previous;
    }
    return 0;
}

static int ntfs_transaction_snapshot(NTFS_CONTEXT *context, UINT64 lba)
{
    NTFS_TRANSACTION *transaction = context->transaction;
    NTFS_TRANSACTION_ENTRY *entry;
    if (transaction == 0 || !transaction->active ||
        transaction->rolling_back) return 1;
    if (ntfs_transaction_find(transaction, lba) != 0) return 1;
    entry = (NTFS_TRANSACTION_ENTRY *)kmalloc(sizeof(*entry));
    if (entry == 0) return 0;
    entry->before = (UINT8 *)kmalloc(context->bytes_per_sector);
    if (entry->before == 0) {
        kfree(entry);
        return 0;
    }
    if (!ntfs_read_sector(context, lba, entry->before)) {
        kfree(entry->before);
        kfree(entry);
        return 0;
    }
    entry->lba = lba;
    entry->previous = transaction->last;
    transaction->last = entry;
    transaction->entry_count++;
    return 1;
}

static int ntfs_write_sector(NTFS_CONTEXT *context, UINT64 lba,
                             const void *buffer)
{
    if (!ntfs_transaction_snapshot(context, lba)) return 0;
    return ntfs_write_sector_raw(context, lba, buffer);
}

static int ntfs_transaction_barrier(NTFS_CONTEXT *context)
{
    NTFS_TRANSACTION *transaction = context->transaction;
    if (transaction != 0 && transaction->active) {
        transaction->barrier_count++;
        if (transaction->fail_barrier != 0 &&
            transaction->barrier_count == transaction->fail_barrier)
            return 0;
    }
    return context->device == 0 || block_device_flush(context->device);
}

static void ntfs_transaction_release(NTFS_TRANSACTION *transaction)
{
    NTFS_TRANSACTION_ENTRY *entry = transaction->last;
    while (entry != 0) {
        NTFS_TRANSACTION_ENTRY *previous = entry->previous;
        kfree(entry->before);
        kfree(entry);
        entry = previous;
    }
    transaction->last = 0;
    transaction->entry_count = 0;
}

static int ntfs_transaction_begin(NTFS_CONTEXT *context,
                                  NTFS_TRANSACTION *transaction,
                                  UINT32 fail_barrier)
{
    UINT8 *bytes = (UINT8 *)transaction;
    UINT32 index;
    if (context == 0 || transaction == 0 || context->transaction != 0 ||
        !context->write_allowed) return 0;
    for (index = 0; index < sizeof(*transaction); index++) bytes[index] = 0;
    transaction->active = 1;
    transaction->fail_barrier = fail_barrier;
    context->transaction = transaction;
    return 1;
}

static int ntfs_transaction_rollback(NTFS_CONTEXT *context,
                                     NTFS_TRANSACTION *transaction)
{
    NTFS_TRANSACTION_ENTRY *entry;
    int restored = 1;
    if (context == 0 || transaction == 0 ||
        context->transaction != transaction || !transaction->active) return 0;
    transaction->rolling_back = 1;
    entry = transaction->last;
    while (entry != 0) {
        if (!ntfs_write_sector_raw(context, entry->lba, entry->before))
            restored = 0;
        entry = entry->previous;
    }
    if (context->device != 0 && !block_device_flush(context->device))
        restored = 0;
    transaction->active = 0;
    context->transaction = 0;
    ntfs_transaction_release(transaction);
    return restored;
}

static int ntfs_transaction_commit(NTFS_CONTEXT *context,
                                   NTFS_TRANSACTION *transaction)
{
    if (context == 0 || transaction == 0 ||
        context->transaction != transaction || !transaction->active) return 0;
    if (!ntfs_transaction_barrier(context)) {
        (void)ntfs_transaction_rollback(context, transaction);
        return 0;
    }
    transaction->active = 0;
    context->transaction = 0;
    ntfs_transaction_release(transaction);
    return 1;
}

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */
static void ntfs_copy(void *dst, const void *src, UINT32 n)
{
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    UINT32 i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static int ntfs_ichar_eq(char a, char b)
{
    if (a >= 'a' && a <= 'z') a = (char)(a - 32);
    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    return a == b;
}

static void utf16_to_utf8(const UINT8 *utf16, UINT32 len_chars,
                          char *out, UINT32 cap)
{
    UINT32 i, j = 0;
    for (i = 0; i < len_chars && j + 1 < cap; i++) {
        UINT32 cp = (UINT16)utf16[i * 2] |
                    ((UINT16)utf16[i * 2 + 1] << 8);
        if (cp >= 0xD800U && cp <= 0xDBFFU && i + 1 < len_chars) {
            UINT32 low = (UINT16)utf16[(i + 1) * 2] |
                         ((UINT16)utf16[(i + 1) * 2 + 1] << 8);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                cp = 0x10000U + ((cp - 0xD800U) << 10) +
                     (low - 0xDC00U);
                i++;
            }
        }
        if (cp < 0x80U) out[j++] = (char)cp;
        else if (cp < 0x800U && j + 2 < cap) {
            out[j++] = (char)(0xC0U | (cp >> 6));
            out[j++] = (char)(0x80U | (cp & 0x3FU));
        } else if (cp < 0x10000U && j + 3 < cap) {
            out[j++] = (char)(0xE0U | (cp >> 12));
            out[j++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
            out[j++] = (char)(0x80U | (cp & 0x3FU));
        } else if (cp <= 0x10FFFFU && j + 4 < cap) {
            out[j++] = (char)(0xF0U | (cp >> 18));
            out[j++] = (char)(0x80U | ((cp >> 12) & 0x3FU));
            out[j++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
            out[j++] = (char)(0x80U | (cp & 0x3FU));
        }
    }
    out[j] = '\0';
}

/* Case-insensitive ASCII comparison (stops at '/' or '\0' on b) */
static int ntfs_name_matches(const char *a, const char *b)
{
    while (*a && *b && *b != '/') {
        if (!ntfs_ichar_eq(*a, *b)) return 0;
        a++; b++;
    }
    return *a == '\0' && (*b == '\0' || *b == '/');
}

static void ntfs_zero(void *buffer, UINT64 size);
static int write_mft_record(NTFS_CONTEXT *context, UINT64 record_n,
                            const UINT8 *applied_record);
static int read_mft_record(NTFS_CONTEXT *context, UINT64 record_n);
static int ntfs_allocate_mft_records(NTFS_CONTEXT *context, UINT32 count,
                                     UINT64 *records);
const char *ntfs_context_read_only_reason_string(const NTFS_CONTEXT *context);
static UINT32 ntfs_align8(UINT32 value) { return (value + 7U) & ~7U; }

static int utf8_to_utf16(const char *text, UINT16 *output, UINT32 capacity,
                         UINT32 *length)
{
    UINT32 source = 0;
    UINT32 target = 0;
    while (text[source] != '\0') {
        UINT32 cp;
        UINT8 first = (UINT8)text[source++];
        if (first < 0x80U) cp = first;
        else if ((first & 0xE0U) == 0xC0U) {
            UINT8 second = (UINT8)text[source++];
            if ((second & 0xC0U) != 0x80U) return 0;
            cp = ((UINT32)(first & 0x1FU) << 6) | (second & 0x3FU);
            if (cp < 0x80U) return 0;
        } else if ((first & 0xF0U) == 0xE0U) {
            UINT8 second = (UINT8)text[source++];
            UINT8 third = (UINT8)text[source++];
            if ((second & 0xC0U) != 0x80U || (third & 0xC0U) != 0x80U)
                return 0;
            cp = ((UINT32)(first & 0x0FU) << 12) |
                 ((UINT32)(second & 0x3FU) << 6) | (third & 0x3FU);
            if (cp < 0x800U || (cp >= 0xD800U && cp <= 0xDFFFU)) return 0;
        } else if ((first & 0xF8U) == 0xF0U) {
            UINT8 second = (UINT8)text[source++];
            UINT8 third = (UINT8)text[source++];
            UINT8 fourth = (UINT8)text[source++];
            if ((second & 0xC0U) != 0x80U || (third & 0xC0U) != 0x80U ||
                (fourth & 0xC0U) != 0x80U) return 0;
            cp = ((UINT32)(first & 7U) << 18) |
                 ((UINT32)(second & 0x3FU) << 12) |
                 ((UINT32)(third & 0x3FU) << 6) | (fourth & 0x3FU);
            if (cp < 0x10000U || cp > 0x10FFFFU) return 0;
        } else return 0;
        if (cp < 0x10000U) {
            if (target >= capacity) return 0;
            output[target++] = (UINT16)cp;
        } else {
            if (target + 1U >= capacity) return 0;
            cp -= 0x10000U;
            output[target++] = (UINT16)(0xD800U | (cp >> 10));
            output[target++] = (UINT16)(0xDC00U | (cp & 0x3FFU));
        }
    }
    *length = target;
    return target != 0 && target <= 255U;
}

static int ntfs_append_resident_attribute(UINT8 *record, UINT32 record_size,
                                          UINT32 type, const UINT16 *name,
                                          UINT8 name_length,
                                          const void *value,
                                          UINT32 value_length,
                                          UINT16 id)
{
    NTFS_FILE_RECORD *header = (NTFS_FILE_RECORD *)record;
    UINT32 name_bytes = (UINT32)name_length * 2U;
    UINT32 value_offset = ntfs_align8(sizeof(NTFS_ATTR_HEADER) +
                                      sizeof(NTFS_RESIDENT) + name_bytes);
    UINT32 attribute_length = ntfs_align8(value_offset + value_length);
    NTFS_ATTR_HEADER *attribute;
    NTFS_RESIDENT *resident;
    UINT32 index;
    if (header->used_size < 8 || attribute_length > record_size - header->used_size)
        return 0;
    attribute = (NTFS_ATTR_HEADER *)(record + header->used_size - 8U);
    for (index = 0; index < attribute_length + 8U; index++)
        ((UINT8 *)attribute)[index] = 0;
    attribute->type = type;
    attribute->length = attribute_length;
    attribute->name_length = name_length;
    attribute->name_offset = name_length == 0 ? 0 :
        (UINT16)(sizeof(NTFS_ATTR_HEADER) + sizeof(NTFS_RESIDENT));
    attribute->id = id;
    resident = (NTFS_RESIDENT *)((UINT8 *)attribute + sizeof(NTFS_ATTR_HEADER));
    resident->value_length = value_length;
    resident->value_offset = (UINT16)value_offset;
    if (name_length != 0)
        ntfs_copy((UINT8 *)attribute + attribute->name_offset, name, name_bytes);
    if (value_length != 0)
        ntfs_copy((UINT8 *)attribute + value_offset, value, value_length);
    *(UINT32 *)((UINT8 *)attribute + attribute_length) = ATTR_END;
    header->used_size += attribute_length;
    if (header->next_attr_id <= id) header->next_attr_id = id + 1U;
    return 1;
}

static int ntfs_append_nonresident_extent(
    UINT8 *record, UINT32 record_size, UINT32 type,
    const UINT16 *name, UINT8 name_length, const UINT8 *runlist,
    UINT32 runlist_length, UINT64 lowest_vcn, UINT64 cluster_count,
    UINT64 allocated_size, UINT64 data_size, UINT64 initialized_size,
    UINT16 flags, UINT16 compression_unit, UINT16 id)
{
    NTFS_FILE_RECORD *header = (NTFS_FILE_RECORD *)record;
    UINT32 name_bytes = (UINT32)name_length * 2U;
    UINT32 runlist_offset = ntfs_align8(sizeof(NTFS_ATTR_HEADER) +
                                        sizeof(NTFS_NONRESIDENT) + name_bytes);
    UINT32 attribute_length = ntfs_align8(runlist_offset + runlist_length);
    NTFS_ATTR_HEADER *attribute;
    NTFS_NONRESIDENT *nonresident;
    UINT32 index;
    if (cluster_count == 0 || header->used_size < 8 ||
        attribute_length > record_size - header->used_size) return 0;
    attribute = (NTFS_ATTR_HEADER *)(record + header->used_size - 8U);
    for (index = 0; index < attribute_length + 8U; index++)
        ((UINT8 *)attribute)[index] = 0;
    attribute->type = type;
    attribute->length = attribute_length;
    attribute->non_resident = 1;
    attribute->name_length = name_length;
    attribute->name_offset = name_length == 0 ? 0 :
        (UINT16)(sizeof(NTFS_ATTR_HEADER) + sizeof(NTFS_NONRESIDENT));
    attribute->flags = flags;
    attribute->id = id;
    nonresident = (NTFS_NONRESIDENT *)
        ((UINT8 *)attribute + sizeof(NTFS_ATTR_HEADER));
    nonresident->lowest_vcn = lowest_vcn;
    nonresident->highest_vcn = lowest_vcn + cluster_count - 1U;
    nonresident->runlist_offset = (UINT16)runlist_offset;
    nonresident->compression_unit = compression_unit;
    nonresident->alloc_size = allocated_size;
    nonresident->data_size = data_size;
    nonresident->init_size = initialized_size;
    if (name_length != 0)
        ntfs_copy((UINT8 *)attribute + attribute->name_offset, name, name_bytes);
    ntfs_copy((UINT8 *)attribute + runlist_offset, runlist, runlist_length);
    *(UINT32 *)((UINT8 *)attribute + attribute_length) = ATTR_END;
    header->used_size += attribute_length;
    if (header->next_attr_id <= id) header->next_attr_id = id + 1U;
    return 1;
}

static int ntfs_append_nonresident_attribute(
    UINT8 *record, UINT32 record_size, UINT32 type,
    const UINT16 *name, UINT8 name_length, const UINT8 *runlist,
    UINT32 runlist_length, UINT64 cluster_count, UINT64 allocated_size,
    UINT64 data_size, UINT16 id)
{
    return ntfs_append_nonresident_extent(
        record, record_size, type, name, name_length, runlist,
        runlist_length, 0, cluster_count, allocated_size, data_size,
        data_size, 0, 0, id);
}

static UINT32 ntfs_unsigned_bytes(UINT64 value)
{
    UINT32 bytes = 1;
    while (bytes < 8U && (value >> (bytes * 8U)) != 0) bytes++;
    return bytes;
}

static UINT32 ntfs_signed_bytes(INT64 value)
{
    UINT32 bytes;
    for (bytes = 1; bytes < 8U; bytes++) {
        INT64 minimum = -(1LL << (bytes * 8U - 1U));
        INT64 maximum = (1LL << (bytes * 8U - 1U)) - 1LL;
        if (value >= minimum && value <= maximum) return bytes;
    }
    return 8;
}

static UINT32 ntfs_encode_cluster_runlist(const UINT64 *clusters,
                                          UINT32 cluster_count,
                                          UINT8 *runlist, UINT32 capacity,
                                          NTFS_RUN *runs,
                                          UINT32 *run_count)
{
    UINT32 source = 0;
    UINT32 output = 0;
    UINT32 count = 0;
    INT64 previous_lcn = 0;
    while (source < cluster_count) {
        UINT64 start = clusters[source];
        UINT64 length = 1;
        INT64 delta;
        UINT32 length_bytes;
        UINT32 offset_bytes;
        UINT32 index;
        while (source + length < cluster_count &&
               clusters[source + (UINT32)length] == start + length)
            length++;
        delta = (INT64)start - previous_lcn;
        length_bytes = ntfs_unsigned_bytes(length);
        offset_bytes = ntfs_signed_bytes(delta);
        if (count >= NTFS_MAX_STREAM_RUNS ||
            output + 1U + length_bytes + offset_bytes + 1U > capacity)
            return 0;
        runlist[output++] = (UINT8)(length_bytes | (offset_bytes << 4));
        for (index = 0; index < length_bytes; index++)
            runlist[output++] = (UINT8)(length >> (index * 8U));
        for (index = 0; index < offset_bytes; index++)
            runlist[output++] = (UINT8)((UINT64)delta >> (index * 8U));
        runs[count].lcn = start;
        runs[count].len = length;
        runs[count].sparse = 0;
        count++;
        previous_lcn = (INT64)start;
        source += (UINT32)length;
    }
    runlist[output++] = 0;
    *run_count = count;
    return output;
}

static UINT32 ntfs_encode_runs(const NTFS_RUN *runs, UINT32 run_count,
                               UINT8 *runlist, UINT32 capacity)
{
    UINT32 output = 0;
    UINT32 run_index;
    INT64 previous_lcn = 0;
    for (run_index = 0; run_index < run_count; run_index++) {
        INT64 delta = (INT64)runs[run_index].lcn - previous_lcn;
        UINT32 length_bytes = ntfs_unsigned_bytes(runs[run_index].len);
        UINT32 offset_bytes = runs[run_index].sparse ? 0 :
                              ntfs_signed_bytes(delta);
        UINT32 index;
        if (output + 1U + length_bytes + offset_bytes + 1U > capacity)
            return 0;
        runlist[output++] = (UINT8)(length_bytes | (offset_bytes << 4));
        for (index = 0; index < length_bytes; index++)
            runlist[output++] =
                (UINT8)(runs[run_index].len >> (index * 8U));
        for (index = 0; index < offset_bytes; index++)
            runlist[output++] = (UINT8)((UINT64)delta >> (index * 8U));
        if (!runs[run_index].sparse) previous_lcn = (INT64)runs[run_index].lcn;
    }
    runlist[output++] = 0;
    return output;
}

static UINT32 ntfs_attribute_list_add(UINT8 *list, UINT32 capacity,
                                      UINT32 offset, UINT32 type,
                                      UINT64 lowest_vcn,
                                      UINT64 file_reference,
                                      UINT16 attribute_id)
{
    NTFS_ATTRIBUTE_LIST_ENTRY *entry;
    UINT32 length = ntfs_align8(sizeof(NTFS_ATTRIBUTE_LIST_ENTRY));
    if (offset > capacity || length > capacity - offset) return 0;
    entry = (NTFS_ATTRIBUTE_LIST_ENTRY *)(list + offset);
    ntfs_zero(entry, length);
    entry->type = type;
    entry->length = (UINT16)length;
    entry->lowest_vcn = lowest_vcn;
    entry->file_reference = file_reference;
    entry->attribute_id = attribute_id;
    return offset + length;
}

static int ntfs_initialize_file_record(NTFS_CONTEXT *context, UINT8 *record,
                                       UINT64 record_number, UINT16 sequence,
                                       int directory)
{
    NTFS_FILE_RECORD *header = (NTFS_FILE_RECORD *)record;
    UINT32 sector_count = context->mft_record_size / context->bytes_per_sector;
    UINT32 index;
    UINT32 attribute_offset;
    if (sector_count == 0 || sector_count + 1U > 0xFFFFU) return 0;
    for (index = 0; index < context->mft_record_size; index++) record[index] = 0;
    record[0] = 'F'; record[1] = 'I'; record[2] = 'L'; record[3] = 'E';
    header->fixup_offset = (UINT16)ntfs_align8(sizeof(NTFS_FILE_RECORD));
    header->fixup_count = (UINT16)(sector_count + 1U);
    attribute_offset = ntfs_align8(header->fixup_offset +
                                   header->fixup_count * 2U);
    if (attribute_offset + 8U > context->mft_record_size) return 0;
    header->sequence = sequence == 0 ? 1 : sequence;
    header->hard_links = 1;
    header->attr_offset = (UINT16)attribute_offset;
    header->flags = (UINT16)(0x0001U | (directory ? 0x0002U : 0));
    header->used_size = attribute_offset + 8U;
    header->alloc_size = context->mft_record_size;
    header->record_number = (UINT32)record_number;
    *(UINT32 *)(record + attribute_offset) = ATTR_END;
    return 1;
}

static UINT32 ntfs_build_file_name_value(UINT8 *buffer, UINT32 capacity,
                                         UINT64 parent_reference,
                                         const UINT16 *name,
                                         UINT8 name_length, int directory,
                                         UINT64 data_size)
{
    NTFS_FILE_NAME *file_name = (NTFS_FILE_NAME *)buffer;
    UINT32 size = sizeof(NTFS_FILE_NAME) + (UINT32)name_length * 2U;
    UINT32 index;
    if (size > capacity || name_length == 0) return 0;
    for (index = 0; index < size; index++) buffer[index] = 0;
    file_name->parent_ref = parent_reference;
    file_name->alloc_size = data_size;
    file_name->real_size = data_size;
    file_name->file_flags = directory ? 0x10000000U : 0x00000020U;
    file_name->name_length = name_length;
    file_name->name_space = 1;
    ntfs_copy(buffer + sizeof(NTFS_FILE_NAME), name,
              (UINT32)name_length * 2U);
    return size;
}

static UINT32 ntfs_build_index_root(UINT8 *buffer, UINT32 capacity)
{
    static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
    NTFS_INDEX_ROOT *root = (NTFS_INDEX_ROOT *)buffer;
    NTFS_INDEX_HEADER *header;
    NTFS_INDEX_ENTRY *end;
    UINT32 size = sizeof(NTFS_INDEX_ROOT) + sizeof(NTFS_INDEX_HEADER) +
                  sizeof(NTFS_INDEX_ENTRY);
    UINT32 index;
    (void)i30_name;
    if (capacity < size) return 0;
    for (index = 0; index < size; index++) buffer[index] = 0;
    root->attr_type = ATTR_FILE_NAME;
    root->collation_rule = 1;
    root->bytes_per_ie = 4096;
    root->clusters_per_ie = 1;
    header = (NTFS_INDEX_HEADER *)(buffer + sizeof(NTFS_INDEX_ROOT));
    header->first_entry_offset = sizeof(NTFS_INDEX_HEADER);
    header->total_size = sizeof(NTFS_INDEX_HEADER) + sizeof(NTFS_INDEX_ENTRY);
    header->alloc_size = header->total_size;
    end = (NTFS_INDEX_ENTRY *)((UINT8 *)header + header->first_entry_offset);
    end->length = sizeof(NTFS_INDEX_ENTRY);
    end->flags = 0x02U;
    return size;
}

static int ntfs_build_basic_record(NTFS_CONTEXT *context, UINT8 *record,
                                   UINT64 record_number, UINT16 sequence,
                                   UINT64 parent_reference,
                                   const UINT16 *name, UINT8 name_length,
                                   int directory, const void *data,
                                   UINT32 data_size)
{
    static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
    NTFS_STANDARD_INFORMATION standard;
    UINT8 file_name_value[sizeof(NTFS_FILE_NAME) + 510U];
    UINT8 index_root[sizeof(NTFS_INDEX_ROOT) + sizeof(NTFS_INDEX_HEADER) +
                     sizeof(NTFS_INDEX_ENTRY)];
    UINT32 file_name_size;
    UINT32 index_root_size;
    ntfs_zero(&standard, sizeof(standard));
    standard.file_attributes = directory ? 0x10000000U : 0x00000020U;
    if (!ntfs_initialize_file_record(context, record, record_number,
                                     sequence, directory)) return 0;
    if (!ntfs_append_resident_attribute(record, context->mft_record_size,
                                        ATTR_STANDARD_INFORMATION, 0, 0,
                                        &standard, sizeof(standard), 0))
        return 0;
    file_name_size = ntfs_build_file_name_value(file_name_value,
                                                sizeof(file_name_value),
                                                parent_reference, name,
                                                name_length, directory,
                                                data_size);
    if (file_name_size == 0 ||
        !ntfs_append_resident_attribute(record, context->mft_record_size,
                                        ATTR_FILE_NAME, 0, 0,
                                        file_name_value, file_name_size, 1))
        return 0;
    if (!directory) {
        if (!ntfs_append_resident_attribute(record, context->mft_record_size,
                                            ATTR_DATA, 0, 0, data,
                                            data_size, 2)) return 0;
    } else {
        index_root_size = ntfs_build_index_root(index_root, sizeof(index_root));
        if (index_root_size == 0 ||
            !ntfs_append_resident_attribute(record, context->mft_record_size,
                                            ATTR_INDEX_ROOT, i30_name, 4,
                                            index_root, index_root_size, 2))
            return 0;
    }
    return 1;
}

static int ntfs_build_nonresident_file_record(
    NTFS_CONTEXT *context, UINT8 *record, UINT64 record_number,
    UINT16 sequence, UINT64 parent_reference, const UINT16 *name,
    UINT8 name_length, const UINT8 *runlist, UINT32 runlist_length,
    UINT64 cluster_count, UINT64 allocated_size, UINT64 data_size)
{
    NTFS_STANDARD_INFORMATION standard;
    UINT8 file_name_value[sizeof(NTFS_FILE_NAME) + 510U];
    UINT32 file_name_size;
    ntfs_zero(&standard, sizeof(standard));
    standard.file_attributes = 0x00000020U;
    if (!ntfs_initialize_file_record(context, record, record_number,
                                     sequence, 0) ||
        !ntfs_append_resident_attribute(record, context->mft_record_size,
                                        ATTR_STANDARD_INFORMATION, 0, 0,
                                        &standard, sizeof(standard), 0))
        return 0;
    file_name_size = ntfs_build_file_name_value(file_name_value,
                                                sizeof(file_name_value),
                                                parent_reference, name,
                                                name_length, 0, data_size);
    if (file_name_size == 0 ||
        !ntfs_append_resident_attribute(record, context->mft_record_size,
                                        ATTR_FILE_NAME, 0, 0,
                                        file_name_value, file_name_size, 1) ||
        !ntfs_append_nonresident_attribute(record, context->mft_record_size,
                                           ATTR_DATA, 0, 0, runlist,
                                           runlist_length, cluster_count,
                                           allocated_size, data_size, 2))
        return 0;
    return 1;
}

static int ntfs_write_split_nonresident_file(
    NTFS_CONTEXT *context, UINT64 base_record, UINT16 base_sequence,
    UINT64 parent_reference, const UINT16 *name, UINT8 name_length,
    const NTFS_RUN *runs, UINT32 run_count, UINT64 allocated_size,
    UINT64 data_size)
{
    UINT32 extent_starts[NTFS_MAX_STREAM_RUNS];
    UINT32 extent_counts[NTFS_MAX_STREAM_RUNS];
    UINT64 extent_vcns[NTFS_MAX_STREAM_RUNS];
    UINT64 extension_records[NTFS_MAX_STREAM_RUNS];
    UINT16 extension_sequences[NTFS_MAX_STREAM_RUNS];
    UINT32 extent_count = 0;
    UINT32 run_index = 0;
    UINT64 vcn = 0;
    UINT32 maximum_runlist;
    UINT8 *scratch;
    UINT8 *record;
    UINT8 *list;
    UINT32 list_capacity;
    UINT32 list_size = 0;
    UINT32 extent_index;
    NTFS_STANDARD_INFORMATION standard;
    UINT8 file_name_value[sizeof(NTFS_FILE_NAME) + 510U];
    UINT32 file_name_size;
    UINT64 base_reference = base_record | ((UINT64)base_sequence << 48);
    if (run_count == 0 || run_count > NTFS_MAX_STREAM_RUNS) return 0;
    maximum_runlist = context->mft_record_size - 256U;
    scratch = (UINT8 *)kmalloc(context->mft_record_size);
    record = (UINT8 *)kmalloc(context->mft_record_size);
    list_capacity = (run_count + 3U) *
                    ntfs_align8(sizeof(NTFS_ATTRIBUTE_LIST_ENTRY));
    list = (UINT8 *)kmalloc(list_capacity);
    if (scratch == 0 || record == 0 || list == 0) goto failed;
    while (run_index < run_count) {
        UINT32 count = 1;
        UINT32 encoded = ntfs_encode_runs(runs + run_index, count,
                                          scratch, maximum_runlist);
        if (encoded == 0 || extent_count >= NTFS_MAX_STREAM_RUNS) goto failed;
        while (run_index + count < run_count) {
            UINT32 next = ntfs_encode_runs(runs + run_index, count + 1U,
                                           scratch, maximum_runlist);
            if (next == 0) break;
            count++;
        }
        extent_starts[extent_count] = run_index;
        extent_counts[extent_count] = count;
        extent_vcns[extent_count] = vcn;
        while (count != 0) {
            vcn += runs[run_index].len;
            run_index++;
            count--;
        }
        extent_count++;
    }
    if (!ntfs_allocate_mft_records(context, extent_count,
                                   extension_records)) goto failed;
    for (extent_index = 0; extent_index < extent_count; extent_index++) {
        extension_sequences[extent_index] = 1;
        if (read_mft_record(context, extension_records[extent_index])) {
            extension_sequences[extent_index] = (UINT16)
                (((NTFS_FILE_RECORD *)context->mft_buf)->sequence + 1U);
            if (extension_sequences[extent_index] == 0)
                extension_sequences[extent_index] = 1;
        }
    }
    list_size = ntfs_attribute_list_add(list, list_capacity, list_size,
                                        ATTR_STANDARD_INFORMATION, 0,
                                        base_reference, 0);
    if (list_size == 0) goto failed;
    list_size = ntfs_attribute_list_add(list, list_capacity, list_size,
                                        ATTR_ATTRIBUTE_LIST, 0,
                                        base_reference, 2);
    if (list_size == 0) goto failed;
    list_size = ntfs_attribute_list_add(list, list_capacity, list_size,
                                        ATTR_FILE_NAME, 0,
                                        base_reference, 1);
    if (list_size == 0) goto failed;
    for (extent_index = 0; extent_index < extent_count; extent_index++) {
        UINT64 reference = extension_records[extent_index] |
            ((UINT64)extension_sequences[extent_index] << 48);
        list_size = ntfs_attribute_list_add(
            list, list_capacity, list_size, ATTR_DATA,
            extent_vcns[extent_index], reference, 0);
        if (list_size == 0) goto failed;
    }
    ntfs_zero(&standard, sizeof(standard));
    standard.file_attributes = 0x00000020U;
    file_name_size = ntfs_build_file_name_value(
        file_name_value, sizeof(file_name_value), parent_reference,
        name, name_length, 0, data_size);
    if (file_name_size == 0 ||
        !ntfs_initialize_file_record(context, record, base_record,
                                     base_sequence, 0) ||
        !ntfs_append_resident_attribute(record, context->mft_record_size,
                                        ATTR_STANDARD_INFORMATION, 0, 0,
                                        &standard, sizeof(standard), 0) ||
        !ntfs_append_resident_attribute(record, context->mft_record_size,
                                        ATTR_ATTRIBUTE_LIST, 0, 0,
                                        list, list_size, 2) ||
        !ntfs_append_resident_attribute(record, context->mft_record_size,
                                        ATTR_FILE_NAME, 0, 0,
                                        file_name_value, file_name_size, 1))
        goto failed;
    if (!write_mft_record(context, base_record, record)) goto failed;
    for (extent_index = 0; extent_index < extent_count; extent_index++) {
        UINT32 encoded = ntfs_encode_runs(
            runs + extent_starts[extent_index], extent_counts[extent_index],
            scratch, maximum_runlist);
        UINT64 clusters = 0;
        UINT32 index;
        if (encoded == 0 ||
            !ntfs_initialize_file_record(context, record,
                                         extension_records[extent_index],
                                         extension_sequences[extent_index], 0))
            goto failed;
        ((NTFS_FILE_RECORD *)record)->base_record = base_reference;
        ((NTFS_FILE_RECORD *)record)->hard_links = 0;
        for (index = 0; index < extent_counts[extent_index]; index++)
            clusters += runs[extent_starts[extent_index] + index].len;
        if (!ntfs_append_nonresident_extent(
                record, context->mft_record_size, ATTR_DATA, 0, 0,
                scratch, encoded, extent_vcns[extent_index], clusters,
                extent_index == 0 ? allocated_size : 0,
                extent_index == 0 ? data_size : 0,
                extent_index == 0 ? data_size : 0, 0, 0, 0) ||
            !write_mft_record(context, extension_records[extent_index],
                              record)) goto failed;
    }
    kfree(list);
    kfree(record);
    kfree(scratch);
    return 1;
failed:
    if (list != 0) kfree(list);
    if (record != 0) kfree(record);
    if (scratch != 0) kfree(scratch);
    return 0;
}

/* -----------------------------------------------------------------------
 * Runlist decoding
 * --------------------------------------------------------------------- */
static UINT32 decode_runlist(const UINT8 *rl, UINT32 rl_length,
                             NTFS_RUN *runs, UINT32 max_runs)
{
    const UINT8 *cursor = rl;
    const UINT8 *end = rl + rl_length;
    UINT32 count = 0;
    UINT64 current_lcn = 0;

    while (cursor < end && *cursor && count < max_runs) {
        UINT8  hdr      = *cursor++;
        UINT8  len_size = hdr & 0x0Fu;
        UINT8  off_size = (hdr >> 4) & 0x0Fu;
        UINT64 run_len  = 0;
        UINT64 run_off  = 0;
        UINT32 i;

        if (len_size == 0 || len_size > 8 || off_size > 8 ||
            (UINT32)(end - cursor) < (UINT32)len_size + off_size) return 0;

        for (i = 0; i < len_size; i++)
            run_len |= (UINT64)(*cursor++) << (i * 8);
        for (i = 0; i < off_size; i++)
            run_off |= (UINT64)(*cursor++) << (i * 8);

        /* Sign-extend the LCN delta */
        if (off_size > 0 && (run_off >> (off_size * 8 - 1)) & 1u) {
            UINT64 sign_ext = (UINT64)(~0) << (off_size * 8);
            run_off |= sign_ext;
        }

        if (off_size != 0) current_lcn += (INT64)run_off;
        runs[count].lcn = current_lcn;
        runs[count].len = run_len;
        runs[count].sparse = off_size == 0;
        if (run_len == 0) return 0;
        count++;
    }
    return cursor < end && *cursor == 0 ? count : 0;
}

/* Read data from a non-resident attribute using pre-decoded runs */
static UINT64 read_from_runs(
    NTFS_CONTEXT *context,
    const NTFS_RUN *runs,
    UINT32 run_count,
    UINT64 data_size,
    UINT64 file_offset,
    void  *buf,
    UINT64 n)
{
    UINT8  *out       = (UINT8 *)buf;
    UINT64 total      = 0;
    UINT64 vcn_byte   = file_offset;
    UINT32 ri;

    if (file_offset >= data_size) return 0;
    if (n > data_size - file_offset) n = data_size - file_offset;

    for (ri = 0; ri < run_count && total < n; ri++) {
        UINT64 run_bytes = runs[ri].len * context->bytes_per_cluster;

        if (vcn_byte >= run_bytes) {
            vcn_byte -= run_bytes;
            continue;
        }

        {
            UINT64 lba_run  = runs[ri].lcn * context->boot.sectors_per_cluster;
            UINT64 byte_in  = vcn_byte;
            UINT64 to_read  = run_bytes - byte_in;
            if (to_read > n - total) to_read = n - total;

            while (to_read > 0) {
                UINT8  sec[NTFS_MAX_SECTOR_SIZE];
                UINT64 sec_off = byte_in % context->bytes_per_sector;
                UINT64 lba     = lba_run + byte_in / context->bytes_per_sector;
                UINT64 avail, chunk;

                if (!runs[ri].sparse && !ntfs_read_sector(context, lba, sec)) return total;
                avail = context->bytes_per_sector - sec_off;
                chunk = to_read < avail ? to_read : avail;
                if (runs[ri].sparse) {
                    UINT32 zero;
                    for (zero = 0; zero < (UINT32)chunk; zero++) out[total + zero] = 0;
                } else ntfs_copy(out + total, sec + sec_off, (UINT32)chunk);
                total   += chunk;
                byte_in += chunk;
                to_read -= chunk;
            }
            vcn_byte = 0;
        }
    }
    return total;
}

static UINT32 decode_writable_runlist(const UINT8 *runlist, UINT32 length,
                                      NTFS_RUN *runs, UINT32 max_runs)
{
    const UINT8 *cursor = runlist;
    const UINT8 *end = runlist + length;
    UINT64 current_lcn = 0;
    UINT32 count = 0;
    while (cursor < end && *cursor != 0 && count < max_runs) {
        UINT8 header = *cursor++;
        UINT8 length_size = header & 0x0FU;
        UINT8 offset_size = header >> 4;
        UINT64 run_length = 0;
        UINT64 run_offset = 0;
        UINT32 index;
        if (length_size == 0 || length_size > 8 || offset_size == 0 ||
            offset_size > 8 || (UINT32)(end - cursor) <
                              (UINT32)length_size + offset_size) return 0;
        for (index = 0; index < length_size; index++)
            run_length |= (UINT64)(*cursor++) << (index * 8U);
        for (index = 0; index < offset_size; index++)
            run_offset |= (UINT64)(*cursor++) << (index * 8U);
        if ((run_offset >> (offset_size * 8U - 1U)) & 1U)
            run_offset |= ~0ULL << (offset_size * 8U);
        current_lcn += (INT64)run_offset;
        if (run_length == 0) return 0;
        runs[count].lcn = current_lcn;
        runs[count].len = run_length;
        runs[count].sparse = 0;
        count++;
    }
    return cursor < end && *cursor == 0 ? count : 0;
}

static int write_to_runs(NTFS_CONTEXT *context, const NTFS_RUN *runs,
                         UINT32 run_count, UINT64 allocated_size,
                         const void *buffer, UINT64 size)
{
    const UINT8 *source = (const UINT8 *)buffer;
    UINT64 written = 0;
    UINT32 run_index;
    if (size > allocated_size) return 0;
    for (run_index = 0; run_index < run_count && written < size; run_index++) {
        UINT64 run_bytes = runs[run_index].len * context->bytes_per_cluster;
        UINT64 run_offset = 0;
        while (run_offset < run_bytes && written < size) {
            UINT8 sector[NTFS_MAX_SECTOR_SIZE];
            UINT64 lba = runs[run_index].lcn *
                         context->boot.sectors_per_cluster +
                         run_offset / context->bytes_per_sector;
            UINT32 sector_offset = (UINT32)(run_offset % context->bytes_per_sector);
            UINT32 chunk = (UINT32)(size - written);
            if (chunk > context->bytes_per_sector - sector_offset)
                chunk = context->bytes_per_sector - sector_offset;
            if (sector_offset != 0 || chunk != context->bytes_per_sector) {
                if (!ntfs_read_sector(context, lba, sector)) return 0;
            }
            ntfs_copy(sector + sector_offset, source + written, chunk);
            if (!ntfs_write_sector(context, lba, sector)) return 0;
            run_offset += chunk;
            written += chunk;
        }
    }
    if (written != size) return 0;
    return ntfs_transaction_barrier(context);
}

static int write_to_runs_at(NTFS_CONTEXT *context, const NTFS_RUN *runs,
                            UINT32 run_count, UINT64 allocated_size,
                            UINT64 file_offset, const void *buffer, UINT64 size)
{
    const UINT8 *source = (const UINT8 *)buffer;
    UINT64 skipped = file_offset;
    UINT64 written = 0;
    UINT32 run_index;
    if (file_offset > allocated_size || size > allocated_size - file_offset)
        return 0;
    for (run_index = 0; run_index < run_count && written < size; run_index++) {
        UINT64 run_bytes = runs[run_index].len * context->bytes_per_cluster;
        UINT64 run_offset = 0;
        if (skipped >= run_bytes) {
            skipped -= run_bytes;
            continue;
        }
        run_offset = skipped;
        skipped = 0;
        while (run_offset < run_bytes && written < size) {
            UINT8 sector[NTFS_MAX_SECTOR_SIZE];
            UINT64 lba;
            UINT32 sector_offset;
            UINT32 chunk;
            if (runs[run_index].sparse) return 0;
            lba = runs[run_index].lcn * context->boot.sectors_per_cluster +
                  run_offset / context->bytes_per_sector;
            sector_offset = (UINT32)(run_offset % context->bytes_per_sector);
            chunk = (UINT32)(size - written);
            if (chunk > context->bytes_per_sector - sector_offset)
                chunk = context->bytes_per_sector - sector_offset;
            if ((UINT64)chunk > run_bytes - run_offset)
                chunk = (UINT32)(run_bytes - run_offset);
            if (sector_offset != 0 || chunk != context->bytes_per_sector) {
                if (!ntfs_read_sector(context, lba, sector)) return 0;
            }
            ntfs_copy(sector + sector_offset, source + written, chunk);
            if (!ntfs_write_sector(context, lba, sector)) return 0;
            run_offset += chunk;
            written += chunk;
        }
    }
    return written == size;
}

static int ntfs_bitmap_find_clear(const UINT8 *bitmap, UINT64 bit_count,
                                  UINT64 first_bit, UINT64 *results,
                                  UINT32 result_count)
{
    UINT64 bit;
    UINT32 found = 0;
    if (bitmap == 0 || results == 0 || result_count == 0 ||
        first_bit >= bit_count) return 0;
    for (bit = first_bit; bit < bit_count && found < result_count; bit++) {
        if ((bitmap[bit >> 3] & (1U << (bit & 7U))) == 0)
            results[found++] = bit;
    }
    return found == result_count;
}

static void ntfs_bitmap_set(UINT8 *bitmap, UINT64 bit, int allocated)
{
    UINT8 mask = (UINT8)(1U << (bit & 7U));
    if (allocated) bitmap[bit >> 3] |= mask;
    else bitmap[bit >> 3] &= (UINT8)~mask;
}

/* -----------------------------------------------------------------------
 * MFT record I/O
 * --------------------------------------------------------------------- */

/* Validate and apply the update sequence array without partially mutating
 * a corrupt record. */
static int apply_fixup(UINT8 *record, UINT32 record_size, UINT32 sector_size)
{
    const NTFS_FILE_RECORD *hdr = (const NTFS_FILE_RECORD *)record;
    const UINT16 *fixup;
    UINT16 check;
    UINT32 i;
    UINT32 sector_count;

    if (record == 0 || record_size < 8U ||
        sector_size < 512 || sector_size > NTFS_MAX_SECTOR_SIZE ||
        (record_size % sector_size) != 0) return 0;
    sector_count = record_size / sector_size;
    if (hdr->fixup_offset < 8U ||
        (hdr->fixup_offset & 1U) != 0 ||
        hdr->fixup_count != sector_count + 1U ||
        (UINT32)hdr->fixup_offset + (UINT32)hdr->fixup_count * 2U > record_size) {
        return 0;
    }
    fixup = (const UINT16 *)(record + hdr->fixup_offset);
    check = fixup[0];

    for (i = 1; i < (UINT32)hdr->fixup_count; i++) {
        UINT32 offset = i * sector_size - 2U;
        const UINT16 *word = (const UINT16 *)(record + offset);
        if (*word != check) return 0;
    }
    for (i = 1; i < (UINT32)hdr->fixup_count; i++) {
        UINT32 offset = i * sector_size - 2U;
        UINT16 *word = (UINT16 *)(record + offset);
        *word = fixup[i];
    }
    return 1;
}

static int protect_fixup(UINT8 *record, UINT32 record_size, UINT32 sector_size)
{
    NTFS_FILE_RECORD *hdr = (NTFS_FILE_RECORD *)record;
    UINT16 *fixup;
    UINT16 sequence;
    UINT32 sector_count;
    UINT32 index;
    if (record == 0 || record_size < 8U ||
        sector_size < 512 || sector_size > NTFS_MAX_SECTOR_SIZE ||
        (record_size % sector_size) != 0) return 0;
    sector_count = record_size / sector_size;
    if (hdr->fixup_offset < 8U ||
        (hdr->fixup_offset & 1U) != 0 ||
        hdr->fixup_count != sector_count + 1U ||
        (UINT32)hdr->fixup_offset + (UINT32)hdr->fixup_count * 2U > record_size)
        return 0;
    fixup = (UINT16 *)(record + hdr->fixup_offset);
    sequence = (UINT16)(fixup[0] + 1U);
    if (sequence == 0) sequence = 1;
    fixup[0] = sequence;
    for (index = 1; index <= sector_count; index++) {
        UINT16 *tail = (UINT16 *)(record + index * sector_size - 2U);
        fixup[index] = *tail;
        *tail = sequence;
    }
    return 1;
}

static int write_mft_record(NTFS_CONTEXT *context, UINT64 record_n,
                            const UINT8 *applied_record)
{
    UINT8 *protected_record = context->write_buf;
    UINT64 logical_offset = record_n * context->mft_record_size;
    UINT32 written = 0;
    UINT32 run_index;
    if (context->device != 0 &&
        block_device_has_capability(context->device,
                                    BLOCK_DEVICE_FLAG_READ_ONLY)) return 0;
    ntfs_copy(protected_record, applied_record, context->mft_record_size);
    if (!protect_fixup(protected_record, context->mft_record_size,
                       context->bytes_per_sector)) return 0;
    for (run_index = 0; run_index < context->mft_run_count &&
         written < context->mft_record_size; run_index++) {
        UINT64 run_bytes = context->mft_runs[run_index].len *
                           context->bytes_per_cluster;
        if (logical_offset >= run_bytes) {
            logical_offset -= run_bytes;
            continue;
        }
        while (logical_offset < run_bytes && written < context->mft_record_size) {
            UINT8 sector[NTFS_MAX_SECTOR_SIZE];
            UINT64 physical_byte = context->mft_runs[run_index].lcn *
                                   context->bytes_per_cluster + logical_offset;
            UINT64 lba = physical_byte / context->bytes_per_sector;
            UINT32 sector_offset = (UINT32)(physical_byte % context->bytes_per_sector);
            UINT32 chunk = context->mft_record_size - written;
            if (chunk > context->bytes_per_sector - sector_offset)
                chunk = context->bytes_per_sector - sector_offset;
            if ((UINT64)chunk > run_bytes - logical_offset)
                chunk = (UINT32)(run_bytes - logical_offset);
            if (sector_offset != 0 || chunk != context->bytes_per_sector) {
                if (!ntfs_read_sector(context, lba, sector)) return 0;
            }
            ntfs_copy(sector + sector_offset, protected_record + written, chunk);
            if (!ntfs_write_sector(context, lba, sector)) return 0;
            logical_offset += chunk;
            written += chunk;
        }
        logical_offset = 0;
    }
    if (written != context->mft_record_size) return 0;
    return ntfs_transaction_barrier(context);
}

static int validate_loaded_file_record(NTFS_CONTEXT *context)
{
    return apply_fixup(context->mft_buf, context->mft_record_size,
                       context->bytes_per_sector) &&
           context->mft_buf[0] == 'F' && context->mft_buf[1] == 'I' &&
           context->mft_buf[2] == 'L' && context->mft_buf[3] == 'E';
}

static int read_mft_mirror_record(NTFS_CONTEXT *context, UINT64 record_n)
{
    UINT64 physical_byte;
    UINT32 read = 0;
    if (record_n >= 4 || context->boot.mft_mirror_lcn == 0) return 0;
    physical_byte = context->boot.mft_mirror_lcn *
                    context->bytes_per_cluster +
                    record_n * context->mft_record_size;
    while (read < context->mft_record_size) {
        UINT8 sector[NTFS_MAX_SECTOR_SIZE];
        UINT64 lba = physical_byte / context->bytes_per_sector;
        UINT32 offset = (UINT32)(physical_byte % context->bytes_per_sector);
        UINT32 chunk = context->mft_record_size - read;
        if (chunk > context->bytes_per_sector - offset)
            chunk = context->bytes_per_sector - offset;
        if (!ntfs_read_sector(context, lba, sector)) return 0;
        ntfs_copy(context->mft_buf + read, sector + offset, chunk);
        read += chunk;
        physical_byte += chunk;
    }
    return validate_loaded_file_record(context);
}

/* Read MFT record n into the context buffer, with $MFTMirr fallback. */
static int read_mft_record(NTFS_CONTEXT *context, UINT64 record_n)
{
    UINT64 byte_offset = record_n * context->mft_record_size;
    UINT32 read_so_far = 0;
    UINT32 ri;

    for (ri = 0; ri < context->mft_run_count &&
         read_so_far < context->mft_record_size; ri++) {
        UINT64 run_bytes = context->mft_runs[ri].len * context->bytes_per_cluster;

        if (byte_offset >= run_bytes) {
            byte_offset -= run_bytes;
            continue;
        }

        if (context->mft_runs[ri].sparse)
            return read_mft_mirror_record(context, record_n);
        {
            while (byte_offset < run_bytes &&
                   read_so_far < context->mft_record_size) {
                UINT8  sec[NTFS_MAX_SECTOR_SIZE];
                UINT64 physical_byte = context->mft_runs[ri].lcn *
                                       context->bytes_per_cluster +
                                       byte_offset;
                UINT64 lba = physical_byte / context->bytes_per_sector;
                UINT32 offset =
                    (UINT32)(physical_byte % context->bytes_per_sector);
                UINT32 chunk = context->mft_record_size - read_so_far;
                if (chunk > context->bytes_per_sector - offset)
                    chunk = context->bytes_per_sector - offset;
                if ((UINT64)chunk > run_bytes - byte_offset)
                    chunk = (UINT32)(run_bytes - byte_offset);
                if (!ntfs_read_sector(context, lba, sec))
                    return read_mft_mirror_record(context, record_n);
                ntfs_copy(context->mft_buf + read_so_far,
                          sec + offset, chunk);
                read_so_far += chunk;
                byte_offset += chunk;
            }
            byte_offset = 0;
        }
    }

    if (read_so_far < context->mft_record_size ||
        !validate_loaded_file_record(context))
        return read_mft_mirror_record(context, record_n);
    return 1;
}

static int ntfs_set_volume_dirty(NTFS_CONTEXT *context, int dirty)
{
    NTFS_FILE_RECORD *record;
    UINT32 offset;
    if (!read_mft_record(context, 3)) return 0;
    record = (NTFS_FILE_RECORD *)context->mft_buf;
    offset = record->attr_offset;
    while (offset + sizeof(NTFS_ATTR_HEADER) <= record->used_size) {
        NTFS_ATTR_HEADER *attribute =
            (NTFS_ATTR_HEADER *)(context->mft_buf + offset);
        if (attribute->type == ATTR_END) break;
        if (attribute->length < sizeof(NTFS_ATTR_HEADER) ||
            attribute->length > record->used_size - offset) return 0;
        if (attribute->type == ATTR_VOLUME_INFORMATION &&
            !attribute->non_resident) {
            NTFS_RESIDENT *resident = (NTFS_RESIDENT *)
                ((UINT8 *)attribute + sizeof(NTFS_ATTR_HEADER));
            UINT8 *value;
            UINT16 flags;
            if (attribute->length < sizeof(NTFS_ATTR_HEADER) +
                                    sizeof(NTFS_RESIDENT) ||
                resident->value_offset > attribute->length ||
                resident->value_length < 12 ||
                resident->value_length >
                    attribute->length - resident->value_offset) return 0;
            value = (UINT8 *)attribute + resident->value_offset;
            flags = (UINT16)value[10] | ((UINT16)value[11] << 8);
            if (dirty) flags |= 0x0001U;
            else flags &= 0xFFFEU;
            value[10] = (UINT8)flags;
            value[11] = (UINT8)(flags >> 8);
            return write_mft_record(context, 3, context->mft_buf);
        }
        offset += attribute->length;
    }
    return 0;
}

static int ntfs_verify_volume_dirty(NTFS_CONTEXT *context, int dirty)
{
    const NTFS_FILE_RECORD *record;
    UINT32 offset;
    if (!read_mft_record(context, 3)) return 0;
    record = (const NTFS_FILE_RECORD *)context->mft_buf;
    offset = record->attr_offset;
    while (offset + sizeof(NTFS_ATTR_HEADER) <= record->used_size) {
        const NTFS_ATTR_HEADER *attribute =
            (const NTFS_ATTR_HEADER *)(context->mft_buf + offset);
        if (attribute->type == ATTR_END) break;
        if (attribute->length < sizeof(NTFS_ATTR_HEADER) ||
            attribute->length > record->used_size - offset) return 0;
        if (attribute->type == ATTR_VOLUME_INFORMATION &&
            !attribute->non_resident) {
            const NTFS_RESIDENT *resident = (const NTFS_RESIDENT *)
                ((const UINT8 *)attribute + sizeof(NTFS_ATTR_HEADER));
            const UINT8 *value;
            UINT16 flags;
            if (attribute->length < sizeof(NTFS_ATTR_HEADER) +
                                    sizeof(NTFS_RESIDENT) ||
                resident->value_offset > attribute->length ||
                resident->value_length < 12 ||
                resident->value_length >
                    attribute->length - resident->value_offset) return 0;
            value = (const UINT8 *)attribute + resident->value_offset;
            flags = (UINT16)value[10] | ((UINT16)value[11] << 8);
            return ((flags & 0x0001U) != 0) == (dirty != 0);
        }
        offset += attribute->length;
    }
    return 0;
}

static int ntfs_mutation_begin(NTFS_CONTEXT *context,
                               NTFS_TRANSACTION *transaction,
                               UINT32 fail_barrier)
{
    if (!ntfs_transaction_begin(context, transaction, fail_barrier)) return 0;
    if (!ntfs_set_volume_dirty(context, 1) ||
        !ntfs_verify_volume_dirty(context, 1) ||
        !ntfs_transaction_barrier(context)) {
        (void)ntfs_transaction_rollback(context, transaction);
        return 0;
    }
    return 1;
}

static int ntfs_mutation_commit(NTFS_CONTEXT *context,
                                NTFS_TRANSACTION *transaction)
{
    if (!ntfs_transaction_barrier(context) ||
        !ntfs_set_volume_dirty(context, 0) ||
        !ntfs_verify_volume_dirty(context, 0)) {
        (void)ntfs_transaction_rollback(context, transaction);
        return 0;
    }
    return ntfs_transaction_commit(context, transaction);
}

static UINT64 read_from_runs(NTFS_CONTEXT *context, const NTFS_RUN *runs,
                             UINT32 run_count, UINT64 data_size,
                             UINT64 file_offset, void *buf, UINT64 n);
static int collect_attribute_stream(NTFS_CONTEXT *context, UINT64 base_record,
                                    UINT32 type, int unnamed_only,
                                    NTFS_STREAM *stream);
static UINT32 ntfs_collect_extension_records(NTFS_CONTEXT *context,
                                             UINT64 base_record,
                                             UINT32 type, UINT64 *records,
                                             UINT32 capacity);
static UINT64 ntfs_resolve(NTFS_CONTEXT *context, const char *path);
static int ntfs_verify_file_allocation(NTFS_CONTEXT *context,
                                       const char *path);

static int ntfs_remount_verify_path(NTFS_CONTEXT *context,
                                    const char *path, int should_exist,
                                    int check_size, UINT64 expected_size)
{
    NTFS_CONTEXT *verification;
    int exists;
    int valid;
    if (context->device == 0) return 1;
    verification = ntfs_context_create(context->device);
    if (verification == 0) return 0;
    exists = ntfs_context_exists(verification, path);
    valid = exists == should_exist;
    if (valid && exists && check_size)
        valid = ntfs_context_file_size(verification, path) == expected_size;
    if (valid && exists &&
        !ntfs_context_is_directory(verification, path))
        valid = ntfs_verify_file_allocation(verification, path);
    ntfs_context_destroy(verification);
    return valid;
}

static int ntfs_stream_bit_is_set(NTFS_CONTEXT *context,
                                  const NTFS_STREAM *bitmap, UINT64 bit)
{
    UINT8 value;
    UINT64 byte = bit >> 3;
    if (byte >= bitmap->data_size) return 0;
    if (bitmap->resident)
        value = bitmap->resident_value[byte];
    else if (read_from_runs(context, bitmap->runs, bitmap->run_count,
                            bitmap->data_size, byte, &value, 1) != 1)
        return 0;
    return (value & (1U << (bit & 7U))) != 0;
}

static int ntfs_verify_file_allocation(NTFS_CONTEXT *context,
                                       const char *path)
{
    NTFS_STREAM *data;
    NTFS_STREAM *cluster_bitmap;
    NTFS_STREAM *mft_bitmap;
    UINT64 extension_records[NTFS_MAX_STREAM_RUNS];
    UINT32 extension_count;
    UINT32 run_index;
    UINT32 index;
    UINT64 record_number = ntfs_resolve(context, path);
    int valid = 0;
    if (record_number == 0 || ntfs_context_is_directory(context, path))
        return record_number != 0;
    data = (NTFS_STREAM *)kmalloc(sizeof(*data));
    cluster_bitmap = (NTFS_STREAM *)kmalloc(sizeof(*cluster_bitmap));
    mft_bitmap = (NTFS_STREAM *)kmalloc(sizeof(*mft_bitmap));
    if (data == 0 || cluster_bitmap == 0 || mft_bitmap == 0) goto done;
    if (!collect_attribute_stream(context, record_number, ATTR_DATA, 1, data) ||
        !collect_attribute_stream(context, 6, ATTR_DATA, 1, cluster_bitmap) ||
        !collect_attribute_stream(context, 0, ATTR_BITMAP, 0, mft_bitmap) ||
        !ntfs_stream_bit_is_set(context, mft_bitmap, record_number)) goto done;
    for (run_index = 0; run_index < data->run_count; run_index++) {
        UINT64 cluster;
        if (data->runs[run_index].sparse) continue;
        for (cluster = 0; cluster < data->runs[run_index].len; cluster++)
            if (!ntfs_stream_bit_is_set(
                    context, cluster_bitmap,
                    data->runs[run_index].lcn + cluster)) goto done;
    }
    extension_count = ntfs_collect_extension_records(
        context, record_number, ATTR_DATA, extension_records,
        NTFS_MAX_STREAM_RUNS);
    for (index = 0; index < extension_count; index++) {
        if (!ntfs_stream_bit_is_set(context, mft_bitmap,
                                    extension_records[index]) ||
            !read_mft_record(context, extension_records[index]) ||
            (((NTFS_FILE_RECORD *)context->mft_buf)->base_record &
             0x0000FFFFFFFFFFFFULL) != record_number) goto done;
    }
    valid = 1;
done:
    if (mft_bitmap != 0) kfree(mft_bitmap);
    if (cluster_bitmap != 0) kfree(cluster_bitmap);
    if (data != 0) kfree(data);
    return valid;
}

/* -----------------------------------------------------------------------
 * Attribute iteration
 * --------------------------------------------------------------------- */
typedef int (*ATTR_CB)(
    UINT32 type,
    const UINT8 *value,   /* NULL for non-resident */
    UINT32 value_len,
    UINT8  non_resident,
    const UINT8 *runlist,
    UINT32 runlist_len,
    UINT16 flags,
    void  *ctx);

static void iterate_attrs(const UINT8 *record, ATTR_CB cb, void *ctx)
{
    const NTFS_FILE_RECORD *hdr = (const NTFS_FILE_RECORD *)record;
    UINT32 off = hdr->attr_offset;

    if (hdr->used_size > hdr->alloc_size || hdr->alloc_size > MFT_BUF_SIZE ||
        off < sizeof(NTFS_FILE_RECORD) || off > hdr->used_size) return;
    while (off + sizeof(NTFS_ATTR_HEADER) <= hdr->used_size) {
        const NTFS_ATTR_HEADER *ah = (const NTFS_ATTR_HEADER *)(record + off);
        if (ah->type == ATTR_END || ah->length == 0) break;
        if (ah->length < sizeof(NTFS_ATTR_HEADER) ||
            ah->length > hdr->used_size - off) return;

        if (!ah->non_resident) {
            const NTFS_RESIDENT *res =
                (const NTFS_RESIDENT *)(record + off + sizeof(NTFS_ATTR_HEADER));
            if (ah->length < sizeof(NTFS_ATTR_HEADER) + sizeof(NTFS_RESIDENT) ||
                res->value_offset > ah->length ||
                res->value_length > ah->length - res->value_offset) return;
            const UINT8 *val = record + off + res->value_offset;
            if (!cb(ah->type, val, res->value_length, 0, 0, 0,
                    ah->flags, ctx)) return;
        } else {
            const NTFS_NONRESIDENT *nr =
                (const NTFS_NONRESIDENT *)(record + off + sizeof(NTFS_ATTR_HEADER));
            if (ah->length < sizeof(NTFS_ATTR_HEADER) +
                             sizeof(NTFS_NONRESIDENT) ||
                nr->runlist_offset >= ah->length) return;
            const UINT8 *rl     = record + off + nr->runlist_offset;
            UINT32       rl_len = ah->length - nr->runlist_offset;
            if (!cb(ah->type, 0, (UINT32)nr->data_size, 1, rl, rl_len,
                    ah->flags, ctx)) return;
        }
        off += ah->length;
    }
}

/* Generic "find attribute by type" callback context */
typedef struct {
    UINT32       target_type;
    const UINT8 *value;
    UINT32       value_len;
    UINT8        non_resident;
    const UINT8 *runlist;
    UINT32       runlist_len;
    UINT16       flags;
    UINT8        found;
} FIND_ATTR_CTX;

typedef struct {
    const UINT8 *value;
    const UINT8 *runlist;
    UINT32 value_length;
    UINT32 runlist_length;
    UINT64 lowest_vcn;
    UINT64 highest_vcn;
    UINT64 allocated_size;
    UINT64 data_size;
    UINT64 initialized_size;
    UINT16 flags;
    UINT16 id;
    UINT16 compression_unit;
    UINT8 non_resident;
    UINT8 found;
} NTFS_ATTR_VIEW;

static int ntfs_compare_names(const NTFS_CONTEXT *context,
                              const UINT16 *left, UINT32 left_length,
                              const UINT16 *right, UINT32 right_length);
static int ntfs_convert_index_root_and_insert(
    NTFS_CONTEXT *context, UINT64 parent_record, UINT64 child_reference,
    const UINT8 *file_name_value, UINT16 file_name_size);
static int ntfs_index_allocation_insert(
    NTFS_CONTEXT *context, UINT64 parent_record, UINT64 child_reference,
    const UINT8 *file_name_value, UINT16 file_name_size);
static int ntfs_index_allocation_remove(NTFS_CONTEXT *context,
                                        UINT64 parent_record,
                                        UINT64 child_record);
static int ntfs_index_allocation_split_insert(
    NTFS_CONTEXT *context, UINT64 parent_record, NTFS_STREAM *stream,
    UINT32 record_index, UINT64 child_reference,
    const UINT8 *file_name_value, UINT16 file_name_size);
static int ntfs_index_rebuild_tree(NTFS_CONTEXT *context,
                                   UINT64 parent_record,
                                   UINT64 excluded_record);
static int ntfs_grow_index_allocation(NTFS_CONTEXT *context,
                                      UINT64 parent_record,
                                      NTFS_STREAM *stream,
                                      UINT32 new_record_index,
                                      UINT64 *new_vcn);
static int ntfs_build_leaf_record(NTFS_CONTEXT *context, UINT8 *record,
                                  UINT64 vcn, const UINT8 *entries,
                                  const UINT32 *offsets, UINT32 first,
                                  UINT32 end);
static UINT8 *load_index_bitmap(NTFS_CONTEXT *context, UINT64 base_record,
                                UINT64 *bitmap_size);
static int index_bitmap_active(const UINT8 *bitmap, UINT64 bitmap_size,
                               UINT32 index);
static int collect_attribute_stream(NTFS_CONTEXT *context, UINT64 base_record,
                                    UINT32 type, int unnamed_only,
                                    NTFS_STREAM *stream);
static UINT32 ntfs_index_copy_entry(UINT8 *destination, UINT32 capacity,
                                    UINT64 reference, const UINT8 *key,
                                    UINT16 key_length, UINT8 flags,
                                    UINT64 child_vcn);
static NTFS_ATTR_HEADER *ntfs_find_mutable_resident_attribute(
    NTFS_CONTEXT *context, UINT32 type, NTFS_RESIDENT **resident);
static NTFS_ATTR_HEADER *ntfs_find_mutable_resident_named_attribute(
    NTFS_CONTEXT *context, UINT32 type, const UINT16 *name,
    UINT8 name_length, NTFS_RESIDENT **resident);
static int ntfs_resize_resident_attribute(NTFS_CONTEXT *context,
                                          NTFS_ATTR_HEADER *attribute,
                                          INT64 delta);
static int ntfs_allocate_clusters(NTFS_CONTEXT *context, UINT32 count,
                                  UINT64 *clusters);
static int ntfs_zero_cluster_list(NTFS_CONTEXT *context,
                                  const UINT64 *clusters, UINT32 count);
static NTFS_FILE_NAME *ntfs_loaded_primary_file_name(
    NTFS_CONTEXT *context, NTFS_RESIDENT **resident);
static int ntfs_release_stream_clusters(NTFS_CONTEXT *context,
                                        const NTFS_STREAM *stream);

static int find_attr_cb(
    UINT32 type, const UINT8 *val, UINT32 val_len,
    UINT8 nr, const UINT8 *rl, UINT32 rl_len, UINT16 flags, void *ctx_ptr)
{
    FIND_ATTR_CTX *ctx = (FIND_ATTR_CTX *)ctx_ptr;
    if (type == ctx->target_type) {
        ctx->value        = val;
        ctx->value_len    = val_len;
        ctx->non_resident = nr;
        ctx->runlist      = rl;
        ctx->runlist_len  = rl_len;
        ctx->flags        = flags;
        ctx->found        = 1;
        return 0; /* stop */
    }
    return 1; /* continue */
}

/* -----------------------------------------------------------------------
 * Directory index traversal
 * --------------------------------------------------------------------- */

static UINT64 list_index_header(const UINT8 *ih_ptr, UINT32 available,
                                NTFS_FILE_INFO *out, UINT64 cap)
{
    NTFS_INDEX_HEADER ih;
    UINT32 offset;
    UINT64 count = 0;
    if (available < sizeof(ih)) return 0;
    ntfs_copy(&ih, ih_ptr, sizeof(ih));
    if (ih.total_size > available || ih.first_entry_offset < sizeof(ih))
        return 0;
    offset = ih.first_entry_offset;
    while (offset + sizeof(NTFS_INDEX_ENTRY) <= ih.total_size && count < cap) {
        NTFS_INDEX_ENTRY ie;
        ntfs_copy(&ie, ih_ptr + offset, sizeof(ie));
        if (ie.length < sizeof(NTFS_INDEX_ENTRY) ||
            ie.length > ih.total_size - offset) return count;
        if (ie.flags & 0x02) break;
        if (ie.key_length >= (UINT16)sizeof(NTFS_FILE_NAME) &&
            ie.key_length <= ie.length - sizeof(NTFS_INDEX_ENTRY)) {
            const NTFS_FILE_NAME *fn = (const NTFS_FILE_NAME *)
                (ih_ptr + offset + sizeof(NTFS_INDEX_ENTRY));
            const UINT8 *name_utf16 = (const UINT8 *)fn +
                                      sizeof(NTFS_FILE_NAME);
            UINT64 rec_num = ie.mft_ref & 0x0000FFFFFFFFFFFFull;
            UINT32 required = sizeof(NTFS_FILE_NAME) +
                              (UINT32)fn->name_length * 2U;
            if (rec_num >= 12 && fn->name_space != 2 &&
                required <= ie.key_length) {
                utf16_to_utf8(name_utf16, fn->name_length,
                              out[count].name, 256);
                out[count].size = fn->real_size;
                out[count].is_directory =
                    (fn->file_flags & 0x10000000u) ? 1 : 0;
                out[count].read_only =
                    (fn->file_flags & (0x00000001u | 0x00000400u)) ? 1 : 0;
                out[count].reparse_point =
                    (fn->file_flags & 0x00000400u) ? 1 : 0;
                out[count].compressed =
                    (fn->file_flags & 0x00000800u) ? 1 : 0;
                out[count].sparse =
                    (fn->file_flags & 0x00000200u) ? 1 : 0;
                out[count].encrypted =
                    (fn->file_flags & 0x00004000u) ? 1 : 0;
                count++;
            }
        }
        offset += ie.length;
    }
    return count;
}

static int find_loaded_attribute(NTFS_CONTEXT *context, UINT32 type,
                                 UINT16 id, UINT64 lowest_vcn,
                                 int match_identity, int unnamed_only,
                                 NTFS_ATTR_VIEW *view)
{
    const NTFS_FILE_RECORD *record =
        (const NTFS_FILE_RECORD *)context->mft_buf;
    UINT32 offset = record->attr_offset;
    UINT8 *bytes = (UINT8 *)view;
    UINT32 index;
    for (index = 0; index < sizeof(*view); index++) bytes[index] = 0;
    if (record->used_size > record->alloc_size ||
        record->alloc_size > context->mft_record_size ||
        offset < sizeof(NTFS_FILE_RECORD) || offset > record->used_size)
        return 0;
    while (offset + sizeof(NTFS_ATTR_HEADER) <= record->used_size) {
        const NTFS_ATTR_HEADER *attribute =
            (const NTFS_ATTR_HEADER *)(context->mft_buf + offset);
        if (attribute->type == ATTR_END) break;
        if (attribute->length < sizeof(NTFS_ATTR_HEADER) ||
            attribute->length > record->used_size - offset) return 0;
        if (attribute->type == type &&
            (!unnamed_only || attribute->name_length == 0)) {
            UINT64 attribute_vcn = 0;
            if (attribute->non_resident) {
                const NTFS_NONRESIDENT *nonresident;
                if (attribute->length < sizeof(NTFS_ATTR_HEADER) +
                                        sizeof(NTFS_NONRESIDENT)) return 0;
                nonresident = (const NTFS_NONRESIDENT *)
                    ((const UINT8 *)attribute + sizeof(NTFS_ATTR_HEADER));
                attribute_vcn = nonresident->lowest_vcn;
            }
            if (!match_identity ||
                (attribute->id == id && attribute_vcn == lowest_vcn)) {
                view->flags = attribute->flags;
                view->id = attribute->id;
                view->non_resident = attribute->non_resident;
                if (!attribute->non_resident) {
                    const NTFS_RESIDENT *resident = (const NTFS_RESIDENT *)
                        ((const UINT8 *)attribute + sizeof(NTFS_ATTR_HEADER));
                    if (attribute->length < sizeof(NTFS_ATTR_HEADER) +
                                            sizeof(NTFS_RESIDENT) ||
                        resident->value_offset > attribute->length ||
                        resident->value_length >
                            attribute->length - resident->value_offset)
                        return 0;
                    view->value = (const UINT8 *)attribute +
                                  resident->value_offset;
                    view->value_length = resident->value_length;
                    view->data_size = resident->value_length;
                } else {
                    const NTFS_NONRESIDENT *nonresident =
                        (const NTFS_NONRESIDENT *)((const UINT8 *)attribute +
                                                   sizeof(NTFS_ATTR_HEADER));
                    if (nonresident->runlist_offset >= attribute->length)
                        return 0;
                    view->lowest_vcn = nonresident->lowest_vcn;
                    view->highest_vcn = nonresident->highest_vcn;
                    view->allocated_size = nonresident->alloc_size;
                    view->data_size = nonresident->data_size;
                    view->initialized_size = nonresident->init_size;
                    view->compression_unit = nonresident->compression_unit;
                    view->runlist = (const UINT8 *)attribute +
                                    nonresident->runlist_offset;
                    view->runlist_length = attribute->length -
                                           nonresident->runlist_offset;
                }
                view->found = 1;
                return 1;
            }
        }
        offset += attribute->length;
    }
    return 0;
}

static int ntfs_index_entry_compare(NTFS_CONTEXT *context,
                                    const UINT8 *entries,
                                    UINT32 left_offset,
                                    UINT32 right_offset)
{
    const NTFS_INDEX_ENTRY *left =
        (const NTFS_INDEX_ENTRY *)(entries + left_offset);
    const NTFS_INDEX_ENTRY *right =
        (const NTFS_INDEX_ENTRY *)(entries + right_offset);
    const NTFS_FILE_NAME *left_name = (const NTFS_FILE_NAME *)
        ((const UINT8 *)left + sizeof(NTFS_INDEX_ENTRY));
    const NTFS_FILE_NAME *right_name = (const NTFS_FILE_NAME *)
        ((const UINT8 *)right + sizeof(NTFS_INDEX_ENTRY));
    return ntfs_compare_names(
        context,
        (const UINT16 *)((const UINT8 *)left_name + sizeof(NTFS_FILE_NAME)),
        left_name->name_length,
        (const UINT16 *)((const UINT8 *)right_name + sizeof(NTFS_FILE_NAME)),
        right_name->name_length);
}

static int ntfs_index_collect_entry(UINT8 *entries, UINT32 capacity,
                                    UINT32 *bytes, UINT32 *offsets,
                                    UINT32 *count,
                                    const NTFS_INDEX_ENTRY *source,
                                    UINT64 excluded_record)
{
    UINT32 length;
    if ((source->flags & 0x02U) != 0) return 1;
    if ((source->mft_ref & 0x0000FFFFFFFFFFFFULL) == excluded_record)
        return 1;
    if (source->key_length < sizeof(NTFS_FILE_NAME)) return 0;
    length = ntfs_align8(sizeof(NTFS_INDEX_ENTRY) + source->key_length);
    if (*count >= 4096U || length > capacity - *bytes) return 0;
    offsets[*count] = *bytes;
    if (ntfs_index_copy_entry(entries + *bytes, length, source->mft_ref,
                              (const UINT8 *)source +
                                  sizeof(NTFS_INDEX_ENTRY),
                              source->key_length, 0, 0) == 0) return 0;
    *bytes += length;
    (*count)++;
    return 1;
}

static int ntfs_index_rebuild_tree(NTFS_CONTEXT *context,
                                   UINT64 parent_record,
                                   UINT64 excluded_record)
{
    NTFS_STREAM *stream = (NTFS_STREAM *)kmalloc(sizeof(NTFS_STREAM));
    UINT8 *entries = 0;
    UINT32 *offsets = 0;
    UINT8 *record = 0;
    UINT32 entry_capacity;
    UINT32 bytes = 0;
    UINT32 count = 0;
    UINT32 record_count;
    UINT32 record_index;
    UINT32 index;
    UINT32 leaf_first[256];
    UINT32 leaf_end[256];
    UINT32 separators[255];
    UINT32 leaf_count = 0;
    UINT8 *active_bitmap = 0;
    UINT64 active_bitmap_size = 0;
    NTFS_RESIDENT *root_resident;
    NTFS_ATTR_HEADER *root_attribute;
    NTFS_INDEX_ROOT *root;
    NTFS_INDEX_HEADER *root_header;
    if (stream == 0) return 0;
    if (!collect_attribute_stream(context, parent_record,
                                  ATTR_INDEX_ALLOCATION, 0, stream) ||
        stream->resident) goto failed;
    record_count = (UINT32)(stream->data_size / context->index_record_size);
    active_bitmap = load_index_bitmap(context, parent_record,
                                      &active_bitmap_size);
    entry_capacity = (UINT32)stream->data_size + context->mft_record_size;
    entries = (UINT8 *)kmalloc(entry_capacity);
    offsets = (UINT32 *)kmalloc(4096U * sizeof(UINT32));
    record = (UINT8 *)kmalloc(context->index_record_size);
    if (entries == 0 || offsets == 0 || record == 0) goto failed;
    if (!read_mft_record(context, parent_record)) goto failed;
    {
        static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
        root_attribute = ntfs_find_mutable_resident_named_attribute(
            context, ATTR_INDEX_ROOT, i30_name, 4, &root_resident);
    }
    if (root_attribute == 0) goto failed;
    root = (NTFS_INDEX_ROOT *)((UINT8 *)root_attribute +
                               root_resident->value_offset);
    root_header = (NTFS_INDEX_HEADER *)((UINT8 *)root +
                                        sizeof(NTFS_INDEX_ROOT));
    index = root_header->first_entry_offset;
    while (index + sizeof(NTFS_INDEX_ENTRY) <= root_header->total_size) {
        NTFS_INDEX_ENTRY *entry =
            (NTFS_INDEX_ENTRY *)((UINT8 *)root_header + index);
        if (entry->length < sizeof(NTFS_INDEX_ENTRY) ||
            entry->length > root_header->total_size - index ||
            !ntfs_index_collect_entry(entries, entry_capacity, &bytes,
                                      offsets, &count, entry,
                                      excluded_record)) goto failed;
        if ((entry->flags & 0x02U) != 0) break;
        index += entry->length;
    }
    for (record_index = 0; record_index < record_count; record_index++) {
        NTFS_INDEX_HEADER *header;
        UINT32 offset;
        if (active_bitmap != 0 &&
            !index_bitmap_active(active_bitmap, active_bitmap_size,
                                 record_index)) continue;
        if (read_from_runs(context, stream->runs, stream->run_count,
                           stream->data_size,
                           (UINT64)record_index * context->index_record_size,
                           record, context->index_record_size) !=
            context->index_record_size ||
            !apply_fixup(record, context->index_record_size,
                         context->bytes_per_sector)) continue;
        header = (NTFS_INDEX_HEADER *)(record + sizeof(NTFS_INDEX_BLOCK));
        offset = header->first_entry_offset;
        while (offset + sizeof(NTFS_INDEX_ENTRY) <= header->total_size) {
            NTFS_INDEX_ENTRY *entry =
                (NTFS_INDEX_ENTRY *)((UINT8 *)header + offset);
            if (entry->length < sizeof(NTFS_INDEX_ENTRY) ||
                entry->length > header->total_size - offset ||
                !ntfs_index_collect_entry(entries, entry_capacity, &bytes,
                                          offsets, &count, entry,
                                          excluded_record)) goto failed;
            if ((entry->flags & 0x02U) != 0) break;
            offset += entry->length;
        }
    }
    for (index = 1; index < count; index++) {
        UINT32 value = offsets[index];
        UINT32 position = index;
        while (position > 0 && ntfs_index_entry_compare(
                   context, entries, offsets[position - 1U], value) > 0) {
            offsets[position] = offsets[position - 1U];
            position--;
        }
        offsets[position] = value;
    }
    if (count == 0) leaf_count = 0;
    else {
        UINT32 cursor = 0;
        UINT32 usa_bytes = (context->index_record_size /
                            context->bytes_per_sector + 1U) * 2U;
        UINT32 first_offset = ntfs_align8(sizeof(NTFS_INDEX_HEADER) +
                                          usa_bytes);
        UINT32 leaf_capacity = context->index_record_size -
                               sizeof(NTFS_INDEX_BLOCK) - first_offset;
        while (cursor < count) {
            UINT32 start = cursor;
            UINT32 used = sizeof(NTFS_INDEX_ENTRY);
            if (leaf_count >= 256U) goto failed;
            while (cursor < count) {
                NTFS_INDEX_ENTRY *entry =
                    (NTFS_INDEX_ENTRY *)(entries + offsets[cursor]);
                if (used + entry->length > leaf_capacity) break;
                used += entry->length;
                cursor++;
            }
            if (cursor == start) goto failed;
            leaf_first[leaf_count] = start;
            if (cursor < count) {
                if (cursor - start < 2U) goto failed;
                separators[leaf_count] = cursor - 1U;
                leaf_end[leaf_count] = cursor - 1U;
            } else leaf_end[leaf_count] = cursor;
            leaf_count++;
        }
    }
    while (record_count < leaf_count) {
        UINT64 ignored_vcn;
        if (!ntfs_grow_index_allocation(context, parent_record, stream,
                                        record_count, &ignored_vcn))
            goto failed;
        record_count++;
    }
    for (record_index = 0; record_index < leaf_count; record_index++) {
        UINT64 vcn = ((UINT64)record_index * context->index_record_size) /
                     context->bytes_per_cluster;
        if (!ntfs_build_leaf_record(context, record, vcn, entries, offsets,
                                    leaf_first[record_index],
                                    leaf_end[record_index]) ||
            !write_to_runs_at(context, stream->runs, stream->run_count,
                              stream->allocated_size,
                              (UINT64)record_index *
                                  context->index_record_size,
                              record, context->index_record_size))
            goto failed;
    }
    if (!read_mft_record(context, parent_record)) goto failed;
    {
        static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
        root_attribute = ntfs_find_mutable_resident_named_attribute(
            context, ATTR_INDEX_ROOT, i30_name, 4, &root_resident);
    }
    if (root_attribute == 0) goto failed;
    {
        UINT32 root_value_size = sizeof(NTFS_INDEX_ROOT) +
                                 sizeof(NTFS_INDEX_HEADER) +
                                 sizeof(NTFS_INDEX_ENTRY);
        for (index = 0; index + 1U < leaf_count; index++) {
            NTFS_INDEX_ENTRY *entry =
                (NTFS_INDEX_ENTRY *)(entries + offsets[separators[index]]);
            root_value_size += ntfs_align8(sizeof(NTFS_INDEX_ENTRY) +
                                           entry->key_length + 8U);
        }
        {
            UINT32 new_attribute_length = ntfs_align8(
                root_resident->value_offset + root_value_size);
            INT64 delta = (INT64)new_attribute_length -
                          root_attribute->length;
            if (!ntfs_resize_resident_attribute(context, root_attribute,
                                                delta)) goto failed;
        }
        root_attribute = ntfs_find_mutable_resident_attribute(
            context, ATTR_INDEX_ROOT, &root_resident);
        if (root_attribute == 0) goto failed;
        root_resident->value_length = root_value_size;
        root = (NTFS_INDEX_ROOT *)((UINT8 *)root_attribute +
                                   root_resident->value_offset);
        ntfs_zero(root, root_value_size);
        root->attr_type = ATTR_FILE_NAME;
        root->collation_rule = 1;
        root->bytes_per_ie = context->index_record_size;
        root->clusters_per_ie = context->boot.clusters_per_index_record;
        root_header = (NTFS_INDEX_HEADER *)((UINT8 *)root +
                                            sizeof(NTFS_INDEX_ROOT));
        root_header->first_entry_offset = sizeof(NTFS_INDEX_HEADER);
        root_header->alloc_size = root_value_size - sizeof(NTFS_INDEX_ROOT);
        root_header->flags = leaf_count == 0 ? 0 : 1;
        index = root_header->first_entry_offset;
        for (record_index = 0; record_index + 1U < leaf_count;
             record_index++) {
            NTFS_INDEX_ENTRY *separator = (NTFS_INDEX_ENTRY *)
                (entries + offsets[separators[record_index]]);
            UINT64 vcn = ((UINT64)record_index *
                          context->index_record_size) /
                         context->bytes_per_cluster;
            UINT32 length = ntfs_index_copy_entry(
                (UINT8 *)root_header + index,
                root_header->alloc_size - index, separator->mft_ref,
                (UINT8 *)separator + sizeof(NTFS_INDEX_ENTRY),
                separator->key_length, 0x01U, vcn);
            if (length == 0) goto failed;
            index += length;
        }
        {
            UINT64 vcn = leaf_count == 0 ? 0 :
                ((UINT64)(leaf_count - 1U) * context->index_record_size) /
                context->bytes_per_cluster;
            UINT32 length = ntfs_index_copy_entry(
                (UINT8 *)root_header + index,
                root_header->alloc_size - index, 0, 0, 0,
                leaf_count == 0 ? 0x02U : 0x03U, vcn);
            if (length == 0) goto failed;
            index += length;
        }
        root_header->total_size = index;
    }
    {
        NTFS_RESIDENT *bitmap;
        NTFS_ATTR_HEADER *bitmap_attribute =
            ntfs_find_mutable_resident_attribute(context, ATTR_BITMAP,
                                                  &bitmap);
        if (bitmap_attribute == 0) goto failed;
        for (index = 0; index < bitmap->value_length; index++)
            *((UINT8 *)bitmap_attribute + bitmap->value_offset + index) = 0;
        for (index = 0; index < leaf_count; index++)
            ntfs_bitmap_set((UINT8 *)bitmap_attribute + bitmap->value_offset,
                            index, 1);
    }
    if (!write_mft_record(context, parent_record, context->mft_buf))
        goto failed;
    if (active_bitmap != 0) kfree(active_bitmap);
    kfree(record); kfree(offsets); kfree(entries); kfree(stream);
    return ntfs_transaction_barrier(context);
failed:
    if (record != 0) kfree(record);
    if (offsets != 0) kfree(offsets);
    if (entries != 0) kfree(entries);
    if (active_bitmap != 0) kfree(active_bitmap);
    if (stream != 0) kfree(stream);
    return 0;
}

static NTFS_ATTR_HEADER *ntfs_find_mutable_nonresident_attribute(
    NTFS_CONTEXT *context, UINT32 type, NTFS_NONRESIDENT **nonresident)
{
    NTFS_FILE_RECORD *record = (NTFS_FILE_RECORD *)context->mft_buf;
    UINT32 offset = record->attr_offset;
    while (offset + sizeof(NTFS_ATTR_HEADER) <= record->used_size) {
        NTFS_ATTR_HEADER *attribute =
            (NTFS_ATTR_HEADER *)(context->mft_buf + offset);
        if (attribute->type == ATTR_END) break;
        if (attribute->length < sizeof(NTFS_ATTR_HEADER) ||
            attribute->length > record->used_size - offset) return 0;
        if (attribute->type == type && attribute->non_resident) {
            NTFS_NONRESIDENT *value = (NTFS_NONRESIDENT *)
                ((UINT8 *)attribute + sizeof(NTFS_ATTR_HEADER));
            if (attribute->length < sizeof(NTFS_ATTR_HEADER) +
                                    sizeof(NTFS_NONRESIDENT) ||
                value->runlist_offset >= attribute->length) return 0;
            *nonresident = value;
            return attribute;
        }
        offset += attribute->length;
    }
    return 0;
}

static int ntfs_root_select_child(NTFS_CONTEXT *context,
                                  UINT64 parent_record,
                                  const UINT8 *file_name_value,
                                  UINT64 *child_vcn)
{
    static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
    NTFS_RESIDENT *resident;
    NTFS_ATTR_HEADER *attribute;
    NTFS_INDEX_ROOT *root;
    NTFS_INDEX_HEADER *header;
    UINT32 offset;
    const NTFS_FILE_NAME *candidate =
        (const NTFS_FILE_NAME *)file_name_value;
    if (!read_mft_record(context, parent_record)) return 0;
    attribute = ntfs_find_mutable_resident_named_attribute(
        context, ATTR_INDEX_ROOT, i30_name, 4, &resident);
    if (attribute == 0) return 0;
    root = (NTFS_INDEX_ROOT *)((UINT8 *)attribute + resident->value_offset);
    header = (NTFS_INDEX_HEADER *)((UINT8 *)root + sizeof(NTFS_INDEX_ROOT));
    if (header->flags == 0) return 0;
    offset = header->first_entry_offset;
    while (offset + sizeof(NTFS_INDEX_ENTRY) <= header->total_size) {
        NTFS_INDEX_ENTRY *entry =
            (NTFS_INDEX_ENTRY *)((UINT8 *)header + offset);
        if (entry->length < sizeof(NTFS_INDEX_ENTRY) ||
            entry->length > header->total_size - offset ||
            (entry->flags & 0x01U) == 0) return 0;
        if ((entry->flags & 0x02U) != 0) {
            *child_vcn = *(UINT64 *)((UINT8 *)entry + entry->length - 8U);
            return 1;
        }
        if (entry->key_length >= sizeof(NTFS_FILE_NAME)) {
            const NTFS_FILE_NAME *separator = (const NTFS_FILE_NAME *)
                ((UINT8 *)entry + sizeof(NTFS_INDEX_ENTRY));
            if (ntfs_compare_names(
                    context,
                    (const UINT16 *)((const UINT8 *)candidate +
                                     sizeof(NTFS_FILE_NAME)),
                    candidate->name_length,
                    (const UINT16 *)((const UINT8 *)separator +
                                     sizeof(NTFS_FILE_NAME)),
                    separator->name_length) < 0) {
                *child_vcn = *(UINT64 *)((UINT8 *)entry +
                                         entry->length - 8U);
                return 1;
            }
        }
        offset += entry->length;
    }
    return 0;
}

static int ntfs_index_allocation_insert(
    NTFS_CONTEXT *context, UINT64 parent_record, UINT64 child_reference,
    const UINT8 *file_name_value, UINT16 file_name_size)
{
    NTFS_STREAM *stream = (NTFS_STREAM *)kmalloc(sizeof(NTFS_STREAM));
    UINT8 *record = (UINT8 *)kmalloc(context->index_record_size);
    UINT32 record_count;
    UINT32 record_index;
    UINT32 entry_length = ntfs_align8(sizeof(NTFS_INDEX_ENTRY) +
                                      file_name_size);
    const NTFS_FILE_NAME *new_name =
        (const NTFS_FILE_NAME *)file_name_value;
    UINT32 full_record = 0xFFFFFFFFU;
    UINT64 selected_vcn;
    UINT32 selected_record;
    if (stream == 0 || record == 0) {
        if (stream != 0) kfree(stream);
        if (record != 0) kfree(record);
        return 0;
    }
    if (!ntfs_root_select_child(context, parent_record, file_name_value,
                                &selected_vcn) ||
        !collect_attribute_stream(context, parent_record,
                                  ATTR_INDEX_ALLOCATION, 0, stream) ||
        stream->resident || (stream->flags & 0x4001U) != 0) goto failed;
    record_count = (UINT32)(stream->data_size / context->index_record_size);
    selected_record = (UINT32)((selected_vcn * context->bytes_per_cluster) /
                               context->index_record_size);
    if (selected_record >= record_count) goto failed;
    for (record_index = selected_record;
         record_index <= selected_record; record_index++) {
        NTFS_INDEX_HEADER *header;
        UINT32 offset;
        UINT32 insert_offset = 0;
        if (read_from_runs(context, stream->runs, stream->run_count,
                           stream->data_size,
                           (UINT64)record_index * context->index_record_size,
                           record, context->index_record_size) !=
            context->index_record_size ||
            record[0] != 'I' || record[1] != 'N' ||
            record[2] != 'D' || record[3] != 'X' ||
            !apply_fixup(record, context->index_record_size,
                         context->bytes_per_sector)) continue;
        header = (NTFS_INDEX_HEADER *)(record + sizeof(NTFS_INDEX_BLOCK));
        if (header->flags != 0 || header->total_size > header->alloc_size)
            continue;
        if (entry_length > header->alloc_size - header->total_size) {
            if (full_record == 0xFFFFFFFFU) full_record = record_index;
            continue;
        }
        offset = header->first_entry_offset;
        while (offset + sizeof(NTFS_INDEX_ENTRY) <= header->total_size) {
            NTFS_INDEX_ENTRY *entry =
                (NTFS_INDEX_ENTRY *)((UINT8 *)header + offset);
            if (entry->length < sizeof(NTFS_INDEX_ENTRY) ||
                entry->length > header->total_size - offset) goto failed;
            if ((entry->flags & 0x02U) != 0) {
                insert_offset = offset;
                break;
            }
            if (entry->key_length >= sizeof(NTFS_FILE_NAME)) {
                const NTFS_FILE_NAME *existing = (const NTFS_FILE_NAME *)
                    ((UINT8 *)entry + sizeof(NTFS_INDEX_ENTRY));
                int comparison = ntfs_compare_names(
                    context,
                    (const UINT16 *)((const UINT8 *)new_name +
                                     sizeof(NTFS_FILE_NAME)),
                    new_name->name_length,
                    (const UINT16 *)((const UINT8 *)existing +
                                     sizeof(NTFS_FILE_NAME)),
                    existing->name_length);
                if (comparison == 0) goto failed;
                if (comparison < 0) {
                    insert_offset = offset;
                    break;
                }
            }
            offset += entry->length;
        }
        if (insert_offset == 0) goto failed;
        for (offset = header->total_size; offset > insert_offset; offset--)
            ((UINT8 *)header)[offset + entry_length - 1U] =
                ((UINT8 *)header)[offset - 1U];
        if (ntfs_index_copy_entry((UINT8 *)header + insert_offset,
                                  entry_length, child_reference,
                                  file_name_value, file_name_size, 0, 0) == 0)
            goto failed;
        header->total_size += entry_length;
        if (!protect_fixup(record, context->index_record_size,
                           context->bytes_per_sector) ||
            !write_to_runs_at(context, stream->runs, stream->run_count,
                              stream->allocated_size,
                              (UINT64)record_index *
                                  context->index_record_size,
                              record, context->index_record_size) ||
            !ntfs_transaction_barrier(context)) goto failed;
        kfree(record);
        kfree(stream);
        return 1;
    }
    if (full_record != 0xFFFFFFFFU) {
        int result = ntfs_index_allocation_split_insert(
            context, parent_record, stream, full_record,
            child_reference, file_name_value, file_name_size);
        kfree(record);
        kfree(stream);
        return result;
    }
failed:
    kfree(record);
    kfree(stream);
    return 0;
}

static int ntfs_index_allocation_remove(NTFS_CONTEXT *context,
                                        UINT64 parent_record,
                                        UINT64 child_record)
{
    NTFS_STREAM *stream = (NTFS_STREAM *)kmalloc(sizeof(NTFS_STREAM));
    UINT8 *record = (UINT8 *)kmalloc(context->index_record_size);
    UINT32 record_count;
    UINT32 record_index;
    if (stream == 0 || record == 0) {
        if (stream != 0) kfree(stream);
        if (record != 0) kfree(record);
        return 0;
    }
    if (!collect_attribute_stream(context, parent_record,
                                  ATTR_INDEX_ALLOCATION, 0, stream) ||
        stream->resident || (stream->flags & 0x4001U) != 0) goto failed;
    record_count = (UINT32)(stream->data_size / context->index_record_size);
    for (record_index = 0; record_index < record_count; record_index++) {
        NTFS_INDEX_HEADER *header;
        UINT32 offset;
        if (read_from_runs(context, stream->runs, stream->run_count,
                           stream->data_size,
                           (UINT64)record_index * context->index_record_size,
                           record, context->index_record_size) !=
            context->index_record_size ||
            record[0] != 'I' || record[1] != 'N' ||
            record[2] != 'D' || record[3] != 'X' ||
            !apply_fixup(record, context->index_record_size,
                         context->bytes_per_sector)) continue;
        header = (NTFS_INDEX_HEADER *)(record + sizeof(NTFS_INDEX_BLOCK));
        offset = header->first_entry_offset;
        while (offset + sizeof(NTFS_INDEX_ENTRY) <= header->total_size) {
            NTFS_INDEX_ENTRY *entry =
                (NTFS_INDEX_ENTRY *)((UINT8 *)header + offset);
            UINT32 index;
            if (entry->length < sizeof(NTFS_INDEX_ENTRY) ||
                entry->length > header->total_size - offset) goto failed;
            if ((entry->mft_ref & 0x0000FFFFFFFFFFFFULL) == child_record) {
                UINT32 remove_length = entry->length;
                for (index = offset + remove_length;
                     index < header->total_size; index++)
                    ((UINT8 *)header)[index - remove_length] =
                        ((UINT8 *)header)[index];
                header->total_size -= remove_length;
                if (!protect_fixup(record, context->index_record_size,
                                   context->bytes_per_sector) ||
                    !write_to_runs_at(context, stream->runs,
                                      stream->run_count,
                                      stream->allocated_size,
                                      (UINT64)record_index *
                                          context->index_record_size,
                                      record, context->index_record_size) ||
                    !ntfs_transaction_barrier(context)) goto failed;
                kfree(record);
                kfree(stream);
                return 1;
            }
            if ((entry->flags & 0x02U) != 0) break;
            offset += entry->length;
        }
    }
failed:
    kfree(record);
    kfree(stream);
    return 0;
}

static UINT32 ntfs_index_copy_entry(UINT8 *destination, UINT32 capacity,
                                    UINT64 reference, const UINT8 *key,
                                    UINT16 key_length, UINT8 flags,
                                    UINT64 child_vcn)
{
    UINT32 length = ntfs_align8(sizeof(NTFS_INDEX_ENTRY) + key_length +
                                ((flags & 0x01U) != 0 ? 8U : 0U));
    NTFS_INDEX_ENTRY *entry;
    if (length > capacity || length > 0xFFFFU) return 0;
    ntfs_zero(destination, length);
    entry = (NTFS_INDEX_ENTRY *)destination;
    entry->mft_ref = reference;
    entry->length = (UINT16)length;
    entry->key_length = key_length;
    entry->flags = flags;
    if (key_length != 0)
        ntfs_copy(destination + sizeof(NTFS_INDEX_ENTRY), key, key_length);
    if ((flags & 0x01U) != 0)
        *(UINT64 *)(destination + length - 8U) = child_vcn;
    return length;
}

static int ntfs_grow_index_allocation(NTFS_CONTEXT *context,
                                      UINT64 parent_record,
                                      NTFS_STREAM *stream,
                                      UINT32 new_record_index,
                                      UINT64 *new_vcn)
{
    static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
    UINT32 cluster_count = (context->index_record_size +
                            context->bytes_per_cluster - 1U) /
                           context->bytes_per_cluster;
    UINT64 clusters[8];
    NTFS_RUN *combined;
    UINT32 combined_count;
    UINT8 *runlist;
    UINT32 runlist_length;
    UINT32 index;
    NTFS_NONRESIDENT *allocation;
    NTFS_ATTR_HEADER *allocation_attribute;
    NTFS_RESIDENT *bitmap;
    NTFS_ATTR_HEADER *bitmap_attribute;
    UINT32 bitmap_bytes = (new_record_index + 8U) / 8U;
    if (cluster_count == 0 || cluster_count > 8U ||
        stream->run_count + cluster_count > NTFS_MAX_STREAM_RUNS) return 0;
    combined = (NTFS_RUN *)kmalloc(sizeof(NTFS_RUN) * NTFS_MAX_STREAM_RUNS);
    runlist = (UINT8 *)kmalloc(context->mft_record_size);
    if (combined == 0 || runlist == 0) {
        if (combined != 0) kfree(combined);
        if (runlist != 0) kfree(runlist);
        return 0;
    }
    for (index = 0; index < stream->run_count; index++)
        combined[index] = stream->runs[index];
    combined_count = stream->run_count;
    if (!ntfs_allocate_clusters(context, cluster_count, clusters) ||
        !ntfs_zero_cluster_list(context, clusters, cluster_count)) goto failed;
    for (index = 0; index < cluster_count; index++) {
        if (combined_count != 0 &&
            !combined[combined_count - 1U].sparse &&
            combined[combined_count - 1U].lcn +
                combined[combined_count - 1U].len == clusters[index]) {
            combined[combined_count - 1U].len++;
        } else {
            combined[combined_count].lcn = clusters[index];
            combined[combined_count].len = 1;
            combined[combined_count].sparse = 0;
            combined_count++;
        }
    }
    runlist_length = ntfs_encode_runs(combined, combined_count, runlist,
                                      context->mft_record_size);
    if (runlist_length == 0 || !read_mft_record(context, parent_record))
        goto failed;
    allocation_attribute = ntfs_find_mutable_nonresident_attribute(
        context, ATTR_INDEX_ALLOCATION, &allocation);
    if (allocation_attribute == 0) goto failed;
    {
        UINT32 new_length = ntfs_align8(allocation->runlist_offset +
                                        runlist_length);
        INT64 delta = (INT64)new_length - allocation_attribute->length;
        if (!ntfs_resize_resident_attribute(context, allocation_attribute,
                                            delta)) goto failed;
    }
    allocation_attribute = ntfs_find_mutable_nonresident_attribute(
        context, ATTR_INDEX_ALLOCATION, &allocation);
    if (allocation_attribute == 0 ||
        runlist_length > allocation_attribute->length -
                         allocation->runlist_offset) goto failed;
    ntfs_zero((UINT8 *)allocation_attribute + allocation->runlist_offset,
              allocation_attribute->length - allocation->runlist_offset);
    ntfs_copy((UINT8 *)allocation_attribute + allocation->runlist_offset,
              runlist, runlist_length);
    allocation->highest_vcn += cluster_count;
    allocation->alloc_size += (UINT64)cluster_count *
                              context->bytes_per_cluster;
    allocation->data_size += context->index_record_size;
    allocation->init_size = allocation->data_size;
    bitmap_attribute = ntfs_find_mutable_resident_attribute(
        context, ATTR_BITMAP, &bitmap);
    if (bitmap_attribute == 0) goto failed;
    if (bitmap_bytes > bitmap->value_length) {
        UINT32 new_length = ntfs_align8(bitmap->value_offset + bitmap_bytes);
        INT64 delta = (INT64)new_length - bitmap_attribute->length;
        if (!ntfs_resize_resident_attribute(context, bitmap_attribute, delta))
            goto failed;
        bitmap_attribute = ntfs_find_mutable_resident_attribute(
            context, ATTR_BITMAP, &bitmap);
        if (bitmap_attribute == 0) goto failed;
        ntfs_zero((UINT8 *)bitmap_attribute + bitmap->value_offset +
                  bitmap->value_length,
                  bitmap_bytes - bitmap->value_length);
        bitmap->value_length = bitmap_bytes;
    }
    ntfs_bitmap_set((UINT8 *)bitmap_attribute + bitmap->value_offset,
                    new_record_index, 1);
    if (!write_mft_record(context, parent_record, context->mft_buf))
        goto failed;
    *new_vcn = ((UINT64)new_record_index * context->index_record_size) /
               context->bytes_per_cluster;
    if (!collect_attribute_stream(context, parent_record,
                                  ATTR_INDEX_ALLOCATION, 0, stream))
        goto failed;
    kfree(runlist);
    kfree(combined);
    (void)i30_name;
    return 1;
failed:
    kfree(runlist);
    kfree(combined);
    return 0;
}

static int ntfs_root_insert_separator(NTFS_CONTEXT *context,
                                      UINT64 parent_record,
                                      const NTFS_INDEX_ENTRY *median,
                                      UINT64 old_vcn, UINT64 new_vcn)
{
    static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
    NTFS_RESIDENT *resident;
    NTFS_ATTR_HEADER *attribute;
    NTFS_INDEX_ROOT *root;
    NTFS_INDEX_HEADER *header;
    UINT32 offset;
    UINT32 insert_offset = 0;
    UINT32 pointer_offset = 0;
    UINT32 separator_length = ntfs_align8(sizeof(NTFS_INDEX_ENTRY) +
                                          median->key_length + 8U);
    if (!read_mft_record(context, parent_record)) return 0;
    attribute = ntfs_find_mutable_resident_named_attribute(
        context, ATTR_INDEX_ROOT, i30_name, 4, &resident);
    if (attribute == 0) return 0;
    root = (NTFS_INDEX_ROOT *)((UINT8 *)attribute + resident->value_offset);
    header = (NTFS_INDEX_HEADER *)((UINT8 *)root + sizeof(NTFS_INDEX_ROOT));
    if (header->flags == 0) return 0;
    offset = header->first_entry_offset;
    while (offset + sizeof(NTFS_INDEX_ENTRY) <= header->total_size) {
        NTFS_INDEX_ENTRY *entry =
            (NTFS_INDEX_ENTRY *)((UINT8 *)header + offset);
        if (entry->length < sizeof(NTFS_INDEX_ENTRY) ||
            entry->length > header->total_size - offset) return 0;
        if ((entry->flags & 0x01U) != 0 &&
            *(UINT64 *)((UINT8 *)entry + entry->length - 8U) == old_vcn) {
            insert_offset = offset;
            pointer_offset = offset + separator_length;
            break;
        }
        offset += entry->length;
    }
    if (insert_offset == 0 && header->first_entry_offset != 0) return 0;
    if (!ntfs_resize_resident_attribute(context, attribute,
                                        separator_length)) return 0;
    attribute = ntfs_find_mutable_resident_named_attribute(
        context, ATTR_INDEX_ROOT, i30_name, 4, &resident);
    if (attribute == 0) return 0;
    root = (NTFS_INDEX_ROOT *)((UINT8 *)attribute + resident->value_offset);
    header = (NTFS_INDEX_HEADER *)((UINT8 *)root + sizeof(NTFS_INDEX_ROOT));
    for (offset = header->total_size; offset > insert_offset; offset--)
        ((UINT8 *)header)[offset + separator_length - 1U] =
            ((UINT8 *)header)[offset - 1U];
    if (ntfs_index_copy_entry((UINT8 *)header + insert_offset,
                              separator_length, median->mft_ref,
                              (const UINT8 *)median +
                                  sizeof(NTFS_INDEX_ENTRY),
                              median->key_length, 0x01U, old_vcn) == 0)
        return 0;
    {
        NTFS_INDEX_ENTRY *right =
            (NTFS_INDEX_ENTRY *)((UINT8 *)header + pointer_offset);
        if ((right->flags & 0x01U) == 0) return 0;
        *(UINT64 *)((UINT8 *)right + right->length - 8U) = new_vcn;
    }
    header->total_size += separator_length;
    header->alloc_size += separator_length;
    resident->value_length += separator_length;
    return write_mft_record(context, parent_record, context->mft_buf);
}

static int ntfs_build_leaf_record(NTFS_CONTEXT *context, UINT8 *record,
                                  UINT64 vcn, const UINT8 *entries,
                                  const UINT32 *offsets, UINT32 first,
                                  UINT32 end)
{
    NTFS_INDEX_BLOCK *block;
    NTFS_INDEX_HEADER *header;
    UINT32 output;
    UINT32 index;
    ntfs_zero(record, context->index_record_size);
    block = (NTFS_INDEX_BLOCK *)record;
    block->signature[0] = 'I'; block->signature[1] = 'N';
    block->signature[2] = 'D'; block->signature[3] = 'X';
    block->fixup_offset = sizeof(NTFS_INDEX_BLOCK) + sizeof(NTFS_INDEX_HEADER);
    block->fixup_count = (UINT16)(context->index_record_size /
                                  context->bytes_per_sector + 1U);
    block->vcn = vcn;
    header = (NTFS_INDEX_HEADER *)(record + sizeof(NTFS_INDEX_BLOCK));
    header->first_entry_offset = ntfs_align8(sizeof(NTFS_INDEX_HEADER) +
                                             block->fixup_count * 2U);
    header->alloc_size = context->index_record_size -
                         sizeof(NTFS_INDEX_BLOCK);
    output = header->first_entry_offset;
    for (index = first; index < end; index++) {
        const NTFS_INDEX_ENTRY *source =
            (const NTFS_INDEX_ENTRY *)(entries + offsets[index]);
        UINT32 length = ntfs_index_copy_entry(
            (UINT8 *)header + output, header->alloc_size - output,
            source->mft_ref,
            (const UINT8 *)source + sizeof(NTFS_INDEX_ENTRY),
            source->key_length, 0, 0);
        if (length == 0) return 0;
        output += length;
    }
    {
        UINT32 length = ntfs_index_copy_entry(
            (UINT8 *)header + output, header->alloc_size - output,
            0, 0, 0, 0x02U, 0);
        if (length == 0) return 0;
        output += length;
    }
    header->total_size = output;
    return protect_fixup(record, context->index_record_size,
                         context->bytes_per_sector);
}

static int ntfs_index_allocation_split_insert(
    NTFS_CONTEXT *context, UINT64 parent_record, NTFS_STREAM *stream,
    UINT32 record_index, UINT64 child_reference,
    const UINT8 *file_name_value, UINT16 file_name_size)
{
    UINT8 *record = (UINT8 *)kmalloc(context->index_record_size);
    UINT8 *right_record = (UINT8 *)kmalloc(context->index_record_size);
    UINT8 *entries = (UINT8 *)kmalloc(context->index_record_size * 2U);
    UINT32 offsets[256];
    UINT32 count = 0;
    UINT32 bytes = 0;
    UINT32 offset;
    UINT32 insert_index = 0;
    UINT32 median_index;
    UINT32 new_record_index =
        (UINT32)(stream->data_size / context->index_record_size);
    UINT64 old_vcn = ((UINT64)record_index * context->index_record_size) /
                     context->bytes_per_cluster;
    UINT64 new_vcn;
    NTFS_INDEX_ENTRY *median;
    if (record == 0 || right_record == 0 || entries == 0) goto failed;
    if (read_from_runs(context, stream->runs, stream->run_count,
                       stream->data_size,
                       (UINT64)record_index * context->index_record_size,
                       record, context->index_record_size) !=
        context->index_record_size ||
        !apply_fixup(record, context->index_record_size,
                     context->bytes_per_sector)) goto failed;
    {
        NTFS_INDEX_HEADER *header =
            (NTFS_INDEX_HEADER *)(record + sizeof(NTFS_INDEX_BLOCK));
        offset = header->first_entry_offset;
        while (offset + sizeof(NTFS_INDEX_ENTRY) <= header->total_size) {
            NTFS_INDEX_ENTRY *entry =
                (NTFS_INDEX_ENTRY *)((UINT8 *)header + offset);
            if (entry->length < sizeof(NTFS_INDEX_ENTRY) ||
                entry->length > header->total_size - offset) goto failed;
            if ((entry->flags & 0x02U) != 0) break;
            if (count >= 255U || bytes + entry->length >
                context->index_record_size * 2U) goto failed;
            offsets[count++] = bytes;
            ntfs_copy(entries + bytes, entry, entry->length);
            bytes += entry->length;
            offset += entry->length;
        }
    }
    for (insert_index = 0; insert_index < count; insert_index++) {
        NTFS_INDEX_ENTRY *existing =
            (NTFS_INDEX_ENTRY *)(entries + offsets[insert_index]);
        const NTFS_FILE_NAME *existing_name = (const NTFS_FILE_NAME *)
            ((UINT8 *)existing + sizeof(NTFS_INDEX_ENTRY));
        const NTFS_FILE_NAME *new_name =
            (const NTFS_FILE_NAME *)file_name_value;
        if (ntfs_compare_names(
                context,
                (const UINT16 *)((const UINT8 *)new_name +
                                 sizeof(NTFS_FILE_NAME)),
                new_name->name_length,
                (const UINT16 *)((const UINT8 *)existing_name +
                                 sizeof(NTFS_FILE_NAME)),
                existing_name->name_length) < 0) break;
    }
    {
        UINT32 new_length = ntfs_align8(sizeof(NTFS_INDEX_ENTRY) +
                                        file_name_size);
        UINT32 index;
        if (count >= 255U || bytes + new_length >
            context->index_record_size * 2U) goto failed;
        for (index = count; index > insert_index; index--)
            offsets[index] = offsets[index - 1U] + new_length;
        for (index = bytes; index > offsets[insert_index]; index--)
            entries[index + new_length - 1U] = entries[index - 1U];
        offsets[insert_index] = insert_index == count ? bytes :
                                offsets[insert_index];
        if (ntfs_index_copy_entry(entries + offsets[insert_index], new_length,
                                  child_reference, file_name_value,
                                  file_name_size, 0, 0) == 0) goto failed;
        count++;
        bytes += new_length;
    }
    median_index = count / 2U;
    median = (NTFS_INDEX_ENTRY *)(entries + offsets[median_index]);
    if (!ntfs_grow_index_allocation(context, parent_record, stream,
                                    new_record_index, &new_vcn) ||
        !ntfs_build_leaf_record(context, record, old_vcn, entries, offsets,
                                0, median_index) ||
        !ntfs_build_leaf_record(context, right_record, new_vcn, entries,
                                offsets, median_index + 1U, count) ||
        !write_to_runs_at(context, stream->runs, stream->run_count,
                          stream->allocated_size,
                          (UINT64)record_index * context->index_record_size,
                          record, context->index_record_size) ||
        !write_to_runs_at(context, stream->runs, stream->run_count,
                          stream->allocated_size,
                          (UINT64)new_record_index * context->index_record_size,
                          right_record, context->index_record_size) ||
        !ntfs_root_insert_separator(context, parent_record, median,
                                    old_vcn, new_vcn)) goto failed;
    kfree(entries); kfree(right_record); kfree(record);
    return ntfs_transaction_barrier(context);
failed:
    if (entries != 0) kfree(entries);
    if (right_record != 0) kfree(right_record);
    if (record != 0) kfree(record);
    return 0;
}

static int ntfs_convert_index_root_and_insert(
    NTFS_CONTEXT *context, UINT64 parent_record, UINT64 child_reference,
    const UINT8 *file_name_value, UINT16 file_name_size)
{
    static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
    NTFS_RESIDENT *resident;
    NTFS_ATTR_HEADER *attribute;
    NTFS_INDEX_ROOT *root;
    NTFS_INDEX_HEADER *root_header;
    UINT8 *index_record = 0;
    UINT64 *clusters = 0;
    UINT8 *runlist = 0;
    NTFS_RUN *runs = 0;
    UINT32 cluster_count;
    UINT32 run_count = 0;
    UINT32 runlist_length;
    NTFS_INDEX_BLOCK *block;
    NTFS_INDEX_HEADER *leaf_header;
    UINT32 leaf_offset;
    UINT32 source_offset;
    int inserted = 0;
    const NTFS_FILE_NAME *new_name =
        (const NTFS_FILE_NAME *)file_name_value;
    if (!read_mft_record(context, parent_record)) return 0;
    attribute = ntfs_find_mutable_resident_named_attribute(
        context, ATTR_INDEX_ROOT, i30_name, 4, &resident);
    if (attribute == 0) return 0;
    root = (NTFS_INDEX_ROOT *)((UINT8 *)attribute + resident->value_offset);
    root_header = (NTFS_INDEX_HEADER *)((UINT8 *)root +
                                        sizeof(NTFS_INDEX_ROOT));
    if (root_header->flags != 0 || root_header->total_size >
        resident->value_length - sizeof(NTFS_INDEX_ROOT)) return 0;
    cluster_count = (context->index_record_size + context->bytes_per_cluster - 1U) /
                    context->bytes_per_cluster;
    index_record = (UINT8 *)kmalloc(context->index_record_size);
    clusters = (UINT64 *)kmalloc((UINTN)cluster_count * sizeof(UINT64));
    runlist = (UINT8 *)kmalloc(context->mft_record_size);
    runs = (NTFS_RUN *)kmalloc(sizeof(NTFS_RUN) * NTFS_MAX_STREAM_RUNS);
    if (index_record == 0 || clusters == 0 || runlist == 0 || runs == 0 ||
        !ntfs_allocate_clusters(context, cluster_count, clusters)) goto failed;
    runlist_length = ntfs_encode_cluster_runlist(
        clusters, cluster_count, runlist, context->mft_record_size,
        runs, &run_count);
    if (runlist_length == 0) goto failed;
    ntfs_zero(index_record, context->index_record_size);
    block = (NTFS_INDEX_BLOCK *)index_record;
    block->signature[0] = 'I'; block->signature[1] = 'N';
    block->signature[2] = 'D'; block->signature[3] = 'X';
    block->fixup_offset = sizeof(NTFS_INDEX_BLOCK) + sizeof(NTFS_INDEX_HEADER);
    block->fixup_count = (UINT16)(context->index_record_size /
                                  context->bytes_per_sector + 1U);
    block->vcn = 0;
    leaf_header = (NTFS_INDEX_HEADER *)(index_record + sizeof(NTFS_INDEX_BLOCK));
    leaf_header->first_entry_offset = ntfs_align8(
        sizeof(NTFS_INDEX_HEADER) + block->fixup_count * 2U);
    leaf_header->alloc_size = context->index_record_size -
                              sizeof(NTFS_INDEX_BLOCK);
    leaf_offset = leaf_header->first_entry_offset;
    source_offset = root_header->first_entry_offset;
    while (source_offset + sizeof(NTFS_INDEX_ENTRY) <= root_header->total_size) {
        NTFS_INDEX_ENTRY *source = (NTFS_INDEX_ENTRY *)
            ((UINT8 *)root_header + source_offset);
        if (source->length < sizeof(NTFS_INDEX_ENTRY) ||
            source->length > root_header->total_size - source_offset)
            goto failed;
        if ((source->flags & 0x02U) != 0) {
            if (!inserted) {
                UINT32 length = ntfs_index_copy_entry(
                    (UINT8 *)leaf_header + leaf_offset,
                    leaf_header->alloc_size - leaf_offset,
                    child_reference, file_name_value, file_name_size, 0, 0);
                if (length == 0) goto failed;
                leaf_offset += length;
                inserted = 1;
            }
            {
                UINT32 length = ntfs_index_copy_entry(
                    (UINT8 *)leaf_header + leaf_offset,
                    leaf_header->alloc_size - leaf_offset, 0, 0, 0, 0x02U, 0);
                if (length == 0) goto failed;
                leaf_offset += length;
            }
            break;
        }
        if (!inserted && source->key_length >= sizeof(NTFS_FILE_NAME)) {
            const NTFS_FILE_NAME *existing = (const NTFS_FILE_NAME *)
                ((UINT8 *)source + sizeof(NTFS_INDEX_ENTRY));
            int comparison = ntfs_compare_names(
                context,
                (const UINT16 *)((const UINT8 *)new_name +
                                 sizeof(NTFS_FILE_NAME)),
                new_name->name_length,
                (const UINT16 *)((const UINT8 *)existing +
                                 sizeof(NTFS_FILE_NAME)),
                existing->name_length);
            if (comparison < 0) {
                UINT32 length = ntfs_index_copy_entry(
                    (UINT8 *)leaf_header + leaf_offset,
                    leaf_header->alloc_size - leaf_offset,
                    child_reference, file_name_value, file_name_size, 0, 0);
                if (length == 0) goto failed;
                leaf_offset += length;
                inserted = 1;
            }
        }
        {
            UINT32 length = ntfs_index_copy_entry(
                (UINT8 *)leaf_header + leaf_offset,
                leaf_header->alloc_size - leaf_offset, source->mft_ref,
                (UINT8 *)source + sizeof(NTFS_INDEX_ENTRY),
                source->key_length, 0, 0);
            if (length == 0) goto failed;
            leaf_offset += length;
        }
        source_offset += source->length;
    }
    leaf_header->total_size = leaf_offset;
    if (!protect_fixup(index_record, context->index_record_size,
                       context->bytes_per_sector) ||
        !ntfs_zero_cluster_list(context, clusters, cluster_count) ||
        !write_to_runs(context, runs, run_count,
                       (UINT64)cluster_count * context->bytes_per_cluster,
                       index_record, context->index_record_size)) goto failed;

    if (!read_mft_record(context, parent_record)) goto failed;
    attribute = ntfs_find_mutable_resident_named_attribute(
        context, ATTR_INDEX_ROOT, i30_name, 4, &resident);
    if (attribute == 0) goto failed;
    {
        UINT32 new_root_size = sizeof(NTFS_INDEX_ROOT) +
                               sizeof(NTFS_INDEX_HEADER) + 24U;
        UINT32 new_attribute_length = ntfs_align8(resident->value_offset +
                                                  new_root_size);
        INT64 delta = (INT64)new_attribute_length - attribute->length;
        if (!ntfs_resize_resident_attribute(context, attribute, delta))
            goto failed;
        attribute = ntfs_find_mutable_resident_named_attribute(
            context, ATTR_INDEX_ROOT, i30_name, 4, &resident);
        if (attribute == 0) goto failed;
        resident->value_length = new_root_size;
        root = (NTFS_INDEX_ROOT *)((UINT8 *)attribute + resident->value_offset);
        ntfs_zero(root, new_root_size);
        root->attr_type = ATTR_FILE_NAME;
        root->collation_rule = 1;
        root->bytes_per_ie = context->index_record_size;
        root->clusters_per_ie = context->boot.clusters_per_index_record;
        root_header = (NTFS_INDEX_HEADER *)((UINT8 *)root +
                                            sizeof(NTFS_INDEX_ROOT));
        root_header->first_entry_offset = sizeof(NTFS_INDEX_HEADER);
        root_header->total_size = sizeof(NTFS_INDEX_HEADER) + 24U;
        root_header->alloc_size = root_header->total_size;
        root_header->flags = 1;
        if (ntfs_index_copy_entry((UINT8 *)root_header +
                                  root_header->first_entry_offset, 24,
                                  0, 0, 0, 0x03U, 0) != 24U) goto failed;
    }
    {
        UINT8 bitmap = 1;
        if (!ntfs_append_nonresident_attribute(
                context->mft_buf, context->mft_record_size,
                ATTR_INDEX_ALLOCATION, i30_name, 4, runlist, runlist_length,
                cluster_count,
                (UINT64)cluster_count * context->bytes_per_cluster,
                context->index_record_size,
                ((NTFS_FILE_RECORD *)context->mft_buf)->next_attr_id) ||
            !ntfs_append_resident_attribute(
                context->mft_buf, context->mft_record_size, ATTR_BITMAP,
                i30_name, 4, &bitmap, 1,
                ((NTFS_FILE_RECORD *)context->mft_buf)->next_attr_id) ||
            !write_mft_record(context, parent_record, context->mft_buf))
            goto failed;
    }
    kfree(runs); kfree(runlist); kfree(clusters); kfree(index_record);
    return 1;

failed:
    if (runs != 0) kfree(runs);
    if (runlist != 0) kfree(runlist);
    if (clusters != 0) kfree(clusters);
    if (index_record != 0) kfree(index_record);
    return 0;
}

static NTFS_ATTR_HEADER *ntfs_find_mutable_resident_attribute(
    NTFS_CONTEXT *context, UINT32 type, NTFS_RESIDENT **resident)
{
    NTFS_FILE_RECORD *record = (NTFS_FILE_RECORD *)context->mft_buf;
    UINT32 offset = record->attr_offset;
    while (offset + sizeof(NTFS_ATTR_HEADER) <= record->used_size) {
        NTFS_ATTR_HEADER *attribute =
            (NTFS_ATTR_HEADER *)(context->mft_buf + offset);
        if (attribute->type == ATTR_END) break;
        if (attribute->length < sizeof(NTFS_ATTR_HEADER) ||
            attribute->length > record->used_size - offset) return 0;
        if (attribute->type == type && !attribute->non_resident) {
            NTFS_RESIDENT *value = (NTFS_RESIDENT *)
                ((UINT8 *)attribute + sizeof(NTFS_ATTR_HEADER));
            if (attribute->length < sizeof(NTFS_ATTR_HEADER) +
                                    sizeof(NTFS_RESIDENT) ||
                value->value_offset > attribute->length ||
                value->value_length >
                    attribute->length - value->value_offset) return 0;
            *resident = value;
            return attribute;
        }
        offset += attribute->length;
    }
    return 0;
}

static int ntfs_attribute_name_matches(const NTFS_ATTR_HEADER *attribute,
                                       const UINT16 *name, UINT8 name_length)
{
    UINT32 index;
    const UINT16 *attribute_name;
    if (attribute == 0 || attribute->name_length != name_length ||
        attribute->name_offset < sizeof(NTFS_ATTR_HEADER) ||
        attribute->name_offset + (UINT32)name_length * 2U >
            attribute->length) return 0;
    attribute_name = (const UINT16 *)((const UINT8 *)attribute +
                                      attribute->name_offset);
    for (index = 0; index < name_length; index++)
        if (attribute_name[index] != name[index]) return 0;
    return 1;
}

static NTFS_ATTR_HEADER *ntfs_find_mutable_resident_named_attribute(
    NTFS_CONTEXT *context, UINT32 type, const UINT16 *name,
    UINT8 name_length, NTFS_RESIDENT **resident)
{
    NTFS_FILE_RECORD *record = (NTFS_FILE_RECORD *)context->mft_buf;
    UINT32 offset = record->attr_offset;
    while (offset + sizeof(NTFS_ATTR_HEADER) <= record->used_size) {
        NTFS_ATTR_HEADER *attribute =
            (NTFS_ATTR_HEADER *)(context->mft_buf + offset);
        if (attribute->type == ATTR_END) break;
        if (attribute->length < sizeof(NTFS_ATTR_HEADER) ||
            attribute->length > record->used_size - offset) return 0;
        if (attribute->type == type && !attribute->non_resident &&
            ntfs_attribute_name_matches(attribute, name, name_length)) {
            NTFS_RESIDENT *value = (NTFS_RESIDENT *)
                ((UINT8 *)attribute + sizeof(NTFS_ATTR_HEADER));
            if (attribute->length < sizeof(NTFS_ATTR_HEADER) +
                                    sizeof(NTFS_RESIDENT) ||
                value->value_offset > attribute->length ||
                value->value_length >
                    attribute->length - value->value_offset) return 0;
            *resident = value;
            return attribute;
        }
        offset += attribute->length;
    }
    return 0;
}

static int ntfs_resize_resident_attribute(NTFS_CONTEXT *context,
                                          NTFS_ATTR_HEADER *attribute,
                                          INT64 delta)
{
    NTFS_FILE_RECORD *record = (NTFS_FILE_RECORD *)context->mft_buf;
    UINT32 attribute_offset = (UINT32)((UINT8 *)attribute - context->mft_buf);
    UINT32 old_end = attribute_offset + attribute->length;
    UINT32 index;
    if (delta > 0) {
        UINT32 grow = (UINT32)delta;
        if (grow > record->alloc_size - record->used_size) return 0;
        for (index = record->used_size; index > old_end; index--)
            context->mft_buf[index + grow - 1U] =
                context->mft_buf[index - 1U];
        for (index = 0; index < grow; index++)
            context->mft_buf[old_end + index] = 0;
        attribute->length += grow;
        record->used_size += grow;
    } else if (delta < 0) {
        UINT32 shrink = (UINT32)(-delta);
        if (shrink >= attribute->length || old_end > record->used_size)
            return 0;
        for (index = old_end; index < record->used_size; index++)
            context->mft_buf[index - shrink] = context->mft_buf[index];
        for (index = record->used_size - shrink; index < record->used_size;
             index++) context->mft_buf[index] = 0;
        attribute->length -= shrink;
        record->used_size -= shrink;
    }
    return 1;
}

static int ntfs_index_root_insert(NTFS_CONTEXT *context, UINT64 parent_record,
                                  UINT64 child_reference,
                                  const UINT8 *file_name_value,
                                  UINT16 file_name_size)
{
    static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
    NTFS_RESIDENT *resident;
    NTFS_ATTR_HEADER *attribute;
    NTFS_INDEX_ROOT *root;
    NTFS_INDEX_HEADER *header;
    UINT8 *entries;
    UINT32 offset;
    UINT32 insert_offset;
    UINT32 entry_length = ntfs_align8(sizeof(NTFS_INDEX_ENTRY) +
                                      file_name_size);
    const NTFS_FILE_NAME *new_name =
        (const NTFS_FILE_NAME *)file_name_value;
    if (!read_mft_record(context, parent_record)) return 0;
    attribute = ntfs_find_mutable_resident_named_attribute(
        context, ATTR_INDEX_ROOT, i30_name, 4, &resident);
    if (attribute == 0 || resident->value_length <
        sizeof(NTFS_INDEX_ROOT) + sizeof(NTFS_INDEX_HEADER) +
        sizeof(NTFS_INDEX_ENTRY)) return 0;
    root = (NTFS_INDEX_ROOT *)((UINT8 *)attribute + resident->value_offset);
    header = (NTFS_INDEX_HEADER *)((UINT8 *)root + sizeof(NTFS_INDEX_ROOT));
    if (root->attr_type != ATTR_FILE_NAME ||
        header->first_entry_offset < sizeof(NTFS_INDEX_HEADER) ||
        header->total_size > resident->value_length - sizeof(NTFS_INDEX_ROOT))
        return 0;
    if (header->flags != 0)
        return ntfs_index_allocation_insert(
            context, parent_record, child_reference,
            file_name_value, file_name_size);
    entries = (UINT8 *)header;
    offset = header->first_entry_offset;
    insert_offset = offset;
    while (offset + sizeof(NTFS_INDEX_ENTRY) <= header->total_size) {
        NTFS_INDEX_ENTRY *entry = (NTFS_INDEX_ENTRY *)(entries + offset);
        if (entry->length < sizeof(NTFS_INDEX_ENTRY) ||
            entry->length > header->total_size - offset) return 0;
        if (entry->flags & 0x02U) {
            insert_offset = offset;
            break;
        }
        if (entry->key_length >= sizeof(NTFS_FILE_NAME) &&
            entry->key_length <= entry->length - sizeof(NTFS_INDEX_ENTRY)) {
            const NTFS_FILE_NAME *existing = (const NTFS_FILE_NAME *)
                ((UINT8 *)entry + sizeof(NTFS_INDEX_ENTRY));
            const UINT16 *existing_name = (const UINT16 *)
                ((const UINT8 *)existing + sizeof(NTFS_FILE_NAME));
            const UINT16 *candidate_name = (const UINT16 *)
                ((const UINT8 *)new_name + sizeof(NTFS_FILE_NAME));
            int comparison = ntfs_compare_names(context, candidate_name,
                                                new_name->name_length,
                                                existing_name,
                                                existing->name_length);
            if (comparison == 0) return 0;
            if (comparison < 0) {
                insert_offset = offset;
                break;
            }
        }
        offset += entry->length;
    }
    if (!ntfs_resize_resident_attribute(context, attribute, entry_length))
        return ntfs_convert_index_root_and_insert(
            context, parent_record, child_reference,
            file_name_value, file_name_size);
    resident = (NTFS_RESIDENT *)((UINT8 *)attribute + sizeof(NTFS_ATTR_HEADER));
    root = (NTFS_INDEX_ROOT *)((UINT8 *)attribute + resident->value_offset);
    header = (NTFS_INDEX_HEADER *)((UINT8 *)root + sizeof(NTFS_INDEX_ROOT));
    entries = (UINT8 *)header;
    for (offset = header->total_size; offset > insert_offset; offset--)
        entries[offset + entry_length - 1U] = entries[offset - 1U];
    ntfs_zero(entries + insert_offset, entry_length);
    {
        NTFS_INDEX_ENTRY *entry =
            (NTFS_INDEX_ENTRY *)(entries + insert_offset);
        entry->mft_ref = child_reference;
        entry->length = (UINT16)entry_length;
        entry->key_length = file_name_size;
        ntfs_copy((UINT8 *)entry + sizeof(NTFS_INDEX_ENTRY),
                  file_name_value, file_name_size);
    }
    header->total_size += entry_length;
    header->alloc_size += entry_length;
    resident->value_length += entry_length;
    return write_mft_record(context, parent_record, context->mft_buf);
}

static int ntfs_index_root_remove(NTFS_CONTEXT *context, UINT64 parent_record,
                                  UINT64 child_record)
{
    static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
    NTFS_RESIDENT *resident;
    NTFS_ATTR_HEADER *attribute;
    NTFS_INDEX_ROOT *root;
    NTFS_INDEX_HEADER *header;
    UINT8 *entries;
    UINT32 offset;
    UINT32 remove_length = 0;
    if (!read_mft_record(context, parent_record)) return 0;
    attribute = ntfs_find_mutable_resident_named_attribute(
        context, ATTR_INDEX_ROOT, i30_name, 4, &resident);
    if (attribute == 0) return 0;
    root = (NTFS_INDEX_ROOT *)((UINT8 *)attribute + resident->value_offset);
    header = (NTFS_INDEX_HEADER *)((UINT8 *)root + sizeof(NTFS_INDEX_ROOT));
    if (header->total_size >
        resident->value_length - sizeof(NTFS_INDEX_ROOT)) return 0;
    if (header->flags != 0)
        return ntfs_index_rebuild_tree(context, parent_record,
                                       child_record) ||
               ntfs_index_allocation_remove(context, parent_record,
                                            child_record);
    entries = (UINT8 *)header;
    offset = header->first_entry_offset;
    while (offset + sizeof(NTFS_INDEX_ENTRY) <= header->total_size) {
        NTFS_INDEX_ENTRY *entry = (NTFS_INDEX_ENTRY *)(entries + offset);
        if (entry->length < sizeof(NTFS_INDEX_ENTRY) ||
            entry->length > header->total_size - offset) return 0;
        if ((entry->mft_ref & 0x0000FFFFFFFFFFFFULL) == child_record) {
            remove_length = entry->length;
            break;
        }
        if (entry->flags & 0x02U) break;
        offset += entry->length;
    }
    if (remove_length == 0) return 0;
    {
        UINT32 index;
        for (index = offset + remove_length; index < header->total_size; index++)
            entries[index - remove_length] = entries[index];
    }
    header->total_size -= remove_length;
    header->alloc_size -= remove_length;
    resident->value_length -= remove_length;
    return ntfs_resize_resident_attribute(context, attribute,
                                          -(INT64)remove_length) &&
           write_mft_record(context, parent_record, context->mft_buf);
}

static int append_stream_extent(NTFS_STREAM *stream,
                                const NTFS_ATTR_VIEW *attribute)
{
    UINT32 decoded;
    UINT32 index;
    UINT64 clusters = 0;
    if (!attribute->found) return 0;
    if (!attribute->non_resident) {
        if (stream->found || attribute->lowest_vcn != 0) return 0;
        stream->resident = 1;
        stream->resident_value = attribute->value;
        stream->resident_length = attribute->value_length;
        stream->data_size = attribute->value_length;
        stream->flags = attribute->flags;
        stream->found = 1;
        return 1;
    }
    if (stream->resident || attribute->highest_vcn < attribute->lowest_vcn ||
        attribute->lowest_vcn != stream->next_vcn ||
        stream->run_count >= NTFS_MAX_STREAM_RUNS) return 0;
    decoded = decode_runlist(attribute->runlist, attribute->runlist_length,
                             stream->runs + stream->run_count,
                             NTFS_MAX_STREAM_RUNS - stream->run_count);
    if (decoded == 0) return 0;
    for (index = 0; index < decoded; index++)
        clusters += stream->runs[stream->run_count + index].len;
    if (clusters != attribute->highest_vcn - attribute->lowest_vcn + 1U)
        return 0;
    if (!stream->found) {
        stream->flags = attribute->flags;
        stream->compression_unit = attribute->compression_unit;
        stream->allocated_size = attribute->allocated_size;
        stream->data_size = attribute->data_size;
        stream->initialized_size = attribute->initialized_size;
    } else if (stream->flags != attribute->flags ||
               stream->compression_unit != attribute->compression_unit) {
        return 0;
    }
    stream->run_count += decoded;
    stream->next_vcn = attribute->highest_vcn + 1U;
    stream->found = 1;
    return 1;
}

static void ntfs_zero(void *buffer, UINT64 size);

static int load_attribute_list(NTFS_CONTEXT *context, UINT8 **list_bytes,
                               UINT32 *list_size)
{
    NTFS_ATTR_VIEW list;
    UINT8 *buffer;
    if (!find_loaded_attribute(context, ATTR_ATTRIBUTE_LIST, 0, 0, 0, 1,
                               &list))
        return 0;
    if (list.data_size == 0 || list.data_size > NTFS_MAX_ATTRIBUTE_LIST_SIZE ||
        list.data_size > 0xFFFFFFFFULL) return -1;
    buffer = (UINT8 *)kmalloc((UINTN)list.data_size);
    if (buffer == 0) return -1;
    if (!list.non_resident) {
        ntfs_copy(buffer, list.value, list.value_length);
    } else {
        NTFS_STREAM *stream = (NTFS_STREAM *)kmalloc(sizeof(NTFS_STREAM));
        UINT64 available;
        UINT32 offset = 0;
        if (stream == 0) {
            kfree(buffer);
            return -1;
        }
        ntfs_zero(stream, sizeof(*stream));
        if (!append_stream_extent(stream, &list)) {
            kfree(stream);
            kfree(buffer);
            return -1;
        }
        available = stream->next_vcn * context->bytes_per_cluster;
        if (available > list.data_size) available = list.data_size;
        if (read_from_runs(context, stream->runs, stream->run_count,
                           available, 0, buffer, available) != available) {
            kfree(stream);
            kfree(buffer);
            return -1;
        }
        while (offset + sizeof(NTFS_ATTRIBUTE_LIST_ENTRY) <= available) {
            NTFS_ATTRIBUTE_LIST_ENTRY entry;
            ntfs_copy(&entry, buffer + offset, sizeof(entry));
            if (entry.length < sizeof(entry) || entry.length > available - offset)
                break;
            if (entry.type == ATTR_ATTRIBUTE_LIST && entry.name_length == 0 &&
                entry.lowest_vcn == stream->next_vcn) {
                UINT64 owner = entry.file_reference & 0x0000FFFFFFFFFFFFULL;
                NTFS_ATTR_VIEW continuation;
                UINT64 expanded;
                if (!read_mft_record(context, owner) ||
                    !find_loaded_attribute(context, ATTR_ATTRIBUTE_LIST,
                                           entry.attribute_id,
                                           entry.lowest_vcn, 1, 1,
                                           &continuation) ||
                    !append_stream_extent(stream, &continuation)) {
                    kfree(stream);
                    kfree(buffer);
                    return -1;
                }
                expanded = stream->next_vcn * context->bytes_per_cluster;
                if (expanded > list.data_size) expanded = list.data_size;
                if (expanded <= available ||
                    read_from_runs(context, stream->runs, stream->run_count,
                                   expanded, 0, buffer, expanded) != expanded) {
                    kfree(stream);
                    kfree(buffer);
                    return -1;
                }
                available = expanded;
            }
            offset += entry.length;
        }
        if (available < list.data_size) {
            kfree(stream);
            kfree(buffer);
            return -1;
        }
        kfree(stream);
    }
    *list_bytes = buffer;
    *list_size = (UINT32)list.data_size;
    return 1;
}

static int collect_attribute_stream(NTFS_CONTEXT *context, UINT64 base_record,
                                    UINT32 type, int unnamed_only,
                                    NTFS_STREAM *stream)
{
    NTFS_ATTR_VIEW direct;
    UINT8 *list_bytes = 0;
    UINT32 list_size = 0;
    UINT32 offset = 0;
    int list_status;
    UINT8 *stream_bytes = (UINT8 *)stream;
    UINT32 index;
    for (index = 0; index < sizeof(*stream); index++) stream_bytes[index] = 0;
    if (!read_mft_record(context, base_record)) return 0;
    list_status = load_attribute_list(context, &list_bytes, &list_size);
    if (list_status < 0) return 0;
    if (list_status == 0) {
        if (!find_loaded_attribute(context, type, 0, 0, 0, unnamed_only,
                                   &direct)) return 0;
        return append_stream_extent(stream, &direct);
    }
    while (offset + sizeof(NTFS_ATTRIBUTE_LIST_ENTRY) <= list_size) {
        NTFS_ATTRIBUTE_LIST_ENTRY entry;
        UINT64 owner;
        NTFS_ATTR_VIEW attribute;
        ntfs_copy(&entry, list_bytes + offset, sizeof(entry));
        if (entry.length < sizeof(entry) || entry.length > list_size - offset) {
            kfree(list_bytes);
            return 0;
        }
        if (entry.type == type && (!unnamed_only || entry.name_length == 0)) {
            owner = entry.file_reference & 0x0000FFFFFFFFFFFFULL;
            if (!read_mft_record(context, owner) ||
                !find_loaded_attribute(context, type, entry.attribute_id,
                                       entry.lowest_vcn, 1, unnamed_only,
                                       &attribute) ||
                !append_stream_extent(stream, &attribute)) {
                kfree(list_bytes);
                return 0;
            }
        }
        offset += entry.length;
    }
    kfree(list_bytes);
    if (stream->found) return 1;
    if (!read_mft_record(context, base_record) ||
        !find_loaded_attribute(context, type, 0, 0, 0, unnamed_only,
                               &direct)) return 0;
    return append_stream_extent(stream, &direct);
}

static UINT32 ntfs_collect_extension_records(NTFS_CONTEXT *context,
                                             UINT64 base_record,
                                             UINT32 type, UINT64 *records,
                                             UINT32 capacity)
{
    UINT8 *list = 0;
    UINT32 list_size = 0;
    UINT32 offset = 0;
    UINT32 count = 0;
    int status;
    if (!read_mft_record(context, base_record)) return 0;
    status = load_attribute_list(context, &list, &list_size);
    if (status <= 0) return 0;
    while (offset + sizeof(NTFS_ATTRIBUTE_LIST_ENTRY) <= list_size) {
        NTFS_ATTRIBUTE_LIST_ENTRY entry;
        UINT64 owner;
        UINT32 index;
        int duplicate = 0;
        ntfs_copy(&entry, list + offset, sizeof(entry));
        if (entry.length < sizeof(entry) || entry.length > list_size - offset)
            break;
        owner = entry.file_reference & 0x0000FFFFFFFFFFFFULL;
        if (entry.type == type && owner != base_record) {
            for (index = 0; index < count; index++)
                if (records[index] == owner) duplicate = 1;
            if (!duplicate) {
                if (count >= capacity) {
                    kfree(list);
                    return 0;
                }
                records[count++] = owner;
            }
        }
        offset += entry.length;
    }
    kfree(list);
    return count;
}

static int ntfs_stream_update_bits(NTFS_CONTEXT *context, UINT64 record_number,
                                   UINT32 type, int unnamed_only,
                                   UINT64 first_bit, UINT32 count,
                                   UINT64 *bits, int allocate)
{
    NTFS_STREAM *stream;
    UINT8 *chunk;
    UINT64 bit_count;
    UINT64 byte_offset;
    UINT32 found = 0;
    UINT32 index;
    if (count == 0 || bits == 0) return 0;
    stream = (NTFS_STREAM *)kmalloc(sizeof(*stream));
    chunk = (UINT8 *)kmalloc(4096);
    if (stream == 0 || chunk == 0) {
        if (stream != 0) kfree(stream);
        if (chunk != 0) kfree(chunk);
        return 0;
    }
    if (!collect_attribute_stream(context, record_number, type, unnamed_only,
                                  stream) || stream->data_size == 0 ||
        (stream->flags & (0x0001U | 0x4000U | 0x8000U)) != 0) {
        kfree(chunk);
        kfree(stream);
        return 0;
    }
    bit_count = stream->data_size * 8U;
    if (allocate) {
        if (first_bit >= bit_count) {
            kfree(chunk);
            kfree(stream);
            return 0;
        }
        byte_offset = first_bit >> 3;
        while (byte_offset < stream->data_size && found < count) {
            UINT64 bytes = stream->data_size - byte_offset;
            UINT64 local_bit;
            if (bytes > 4096U) bytes = 4096U;
            if (stream->resident) {
                ntfs_copy(chunk, stream->resident_value + byte_offset,
                          (UINT32)bytes);
            } else if (read_from_runs(context, stream->runs, stream->run_count,
                                      stream->data_size, byte_offset,
                                      chunk, bytes) != bytes) {
                kfree(chunk);
                kfree(stream);
                return 0;
            }
            local_bit = byte_offset * 8U;
            if (local_bit < first_bit) local_bit = first_bit;
            while (local_bit < (byte_offset + bytes) * 8U && found < count) {
                UINT64 local_byte = (local_bit >> 3) - byte_offset;
                if ((chunk[local_byte] & (1U << (local_bit & 7U))) == 0)
                    bits[found++] = local_bit;
                local_bit++;
            }
            byte_offset += bytes;
        }
        if (found != count) {
            kfree(chunk);
            kfree(stream);
            return 0;
        }
    } else {
        for (index = 0; index < count; index++) {
            if (bits[index] < first_bit || bits[index] >= bit_count) {
                kfree(chunk);
                kfree(stream);
                return 0;
            }
        }
    }
    if (stream->resident) {
        NTFS_ATTR_VIEW attribute;
        if (!read_mft_record(context, record_number) ||
            !find_loaded_attribute(context, type, 0, 0, 0, unnamed_only,
                                   &attribute) || attribute.non_resident) {
            kfree(chunk);
            kfree(stream);
            return 0;
        }
        for (index = 0; index < count; index++)
            ntfs_bitmap_set((UINT8 *)attribute.value, bits[index], allocate);
        if (!write_mft_record(context, record_number, context->mft_buf)) {
            kfree(chunk);
            kfree(stream);
            return 0;
        }
    } else {
        for (index = 0; index < count; index++) {
            UINT64 target_byte = bits[index] >> 3;
            UINT8 value;
            if (read_from_runs(context, stream->runs, stream->run_count,
                               stream->data_size, target_byte,
                               &value, 1) != 1) {
                kfree(chunk);
                kfree(stream);
                return 0;
            }
            ntfs_bitmap_set(&value, bits[index] & 7U, allocate);
            if (!write_to_runs_at(context, stream->runs, stream->run_count,
                                  stream->allocated_size, target_byte,
                                  &value, 1)) {
                kfree(chunk);
                kfree(stream);
                return 0;
            }
        }
    }
    kfree(chunk);
    kfree(stream);
    return ntfs_transaction_barrier(context);
}

static int ntfs_allocate_mft_records(NTFS_CONTEXT *context, UINT32 count,
                                     UINT64 *records)
{
    UINT32 index;
    UINT64 limit = context->mft_data_size / context->mft_record_size;
    if (!ntfs_stream_update_bits(context, 0, ATTR_BITMAP, 0, 24,
                                 count, records, 1)) return 0;
    for (index = 0; index < count; index++)
        if (records[index] >= limit) return 0;
    return 1;
}

static int ntfs_free_mft_records(NTFS_CONTEXT *context, UINT32 count,
                                 UINT64 *records)
{
    return ntfs_stream_update_bits(context, 0, ATTR_BITMAP, 0, 24,
                                   count, records, 0);
}

static int ntfs_allocate_clusters(NTFS_CONTEXT *context, UINT32 count,
                                  UINT64 *clusters)
{
    UINT32 index;
    UINT64 limit = context->boot.total_sectors /
                   context->boot.sectors_per_cluster;
    if (!ntfs_stream_update_bits(context, 6, ATTR_DATA, 1, 0,
                                 count, clusters, 1)) return 0;
    for (index = 0; index < count; index++)
        if (clusters[index] >= limit) return 0;
    return 1;
}

static int ntfs_free_clusters(NTFS_CONTEXT *context, UINT32 count,
                              UINT64 *clusters)
{
    return ntfs_stream_update_bits(context, 6, ATTR_DATA, 1, 0,
                                   count, clusters, 0);
}

static int ntfs_zero_cluster_list(NTFS_CONTEXT *context,
                                  const UINT64 *clusters, UINT32 count)
{
    UINT8 sector[NTFS_MAX_SECTOR_SIZE];
    UINT32 index;
    UINT32 sector_index;
    ntfs_zero(sector, context->bytes_per_sector);
    for (index = 0; index < count; index++) {
        for (sector_index = 0;
             sector_index < context->boot.sectors_per_cluster;
             sector_index++) {
            UINT64 lba = clusters[index] *
                         context->boot.sectors_per_cluster + sector_index;
            if (!ntfs_write_sector(context, lba, sector)) return 0;
        }
    }
    return ntfs_transaction_barrier(context);
}

static int ntfs_load_upcase_table(NTFS_CONTEXT *context)
{
    NTFS_STREAM *stream;
    UINT16 *table;
    UINT64 read;
    UINT64 table_size;
    stream = (NTFS_STREAM *)kmalloc(sizeof(*stream));
    if (stream == 0) return 0;
    if (!collect_attribute_stream(context, 10, ATTR_DATA, 1, stream) ||
        stream->data_size < 128U || stream->data_size > 131072U ||
        (stream->data_size & 1U) != 0 ||
        (stream->flags & (0x0001U | 0x4000U | 0x8000U)) != 0) {
        kfree(stream);
        return 0;
    }
    table = (UINT16 *)kmalloc((UINTN)stream->data_size);
    if (table == 0) {
        kfree(stream);
        return 0;
    }
    table_size = stream->data_size;
    if (stream->resident) {
        ntfs_copy(table, stream->resident_value, stream->resident_length);
        read = stream->resident_length;
    } else {
        read = read_from_runs(context, stream->runs, stream->run_count,
                              stream->data_size, 0, table, stream->data_size);
    }
    kfree(stream);
    if (read != table_size || read > 0xFFFFFFFFULL) {
        kfree(table);
        return 0;
    }
    context->upcase = table;
    context->upcase_count = (UINT32)(read / 2U);
    return 1;
}

static UINT16 ntfs_upcase(const NTFS_CONTEXT *context, UINT16 character)
{
    if (context->upcase != 0 && character < context->upcase_count)
        return context->upcase[character];
    if (character >= 'a' && character <= 'z')
        return (UINT16)(character - ('a' - 'A'));
    return character;
}

static int ntfs_compare_names(const NTFS_CONTEXT *context,
                              const UINT16 *left, UINT32 left_length,
                              const UINT16 *right, UINT32 right_length)
{
    UINT32 index;
    UINT32 common = left_length < right_length ? left_length : right_length;
    for (index = 0; index < common; index++) {
        UINT16 left_character = ntfs_upcase(context, left[index]);
        UINT16 right_character = ntfs_upcase(context, right[index]);
        if (left_character < right_character) return -1;
        if (left_character > right_character) return 1;
    }
    if (left_length < right_length) return -1;
    if (left_length > right_length) return 1;
    return 0;
}

static void ntfs_zero(void *buffer, UINT64 size)
{
    UINT8 *bytes = (UINT8 *)buffer;
    UINT64 index;
    for (index = 0; index < size; index++) bytes[index] = 0;
}

static int lznt1_decompress(const UINT8 *source, UINT32 source_size,
                            UINT8 *destination, UINT32 destination_size,
                            UINT32 *written)
{
    UINT32 source_offset = 0;
    UINT32 destination_offset = 0;
    while (source_offset + 2U <= source_size &&
           destination_offset < destination_size) {
        UINT16 header = (UINT16)source[source_offset] |
                        ((UINT16)source[source_offset + 1U] << 8);
        UINT32 payload_size;
        UINT32 payload_end;
        UINT32 chunk_start = destination_offset;
        source_offset += 2U;
        if (header == 0) break;
        if ((header & 0x7000U) != 0x3000U) return 0;
        payload_size = (header & 0x0FFFU) + 1U;
        if (payload_size > source_size - source_offset) return 0;
        payload_end = source_offset + payload_size;
        if ((header & 0x8000U) == 0) {
            UINT32 copy = payload_size;
            if (copy > 4096U || copy > destination_size - destination_offset)
                return 0;
            ntfs_copy(destination + destination_offset,
                      source + source_offset, copy);
            destination_offset += copy;
            source_offset = payload_end;
            continue;
        }
        while (source_offset < payload_end &&
               destination_offset - chunk_start < 4096U) {
            UINT8 tags = source[source_offset++];
            UINT32 bit;
            for (bit = 0; bit < 8U && source_offset < payload_end; bit++) {
                if ((tags & (1U << bit)) == 0) {
                    if (destination_offset >= destination_size ||
                        destination_offset - chunk_start >= 4096U) return 0;
                    destination[destination_offset++] = source[source_offset++];
                } else {
                    UINT16 token;
                    UINT32 length_mask = 0x0FFFU;
                    UINT32 displacement_shift = 12U;
                    UINT32 position = destination_offset - chunk_start;
                    UINT32 displacement;
                    UINT32 length;
                    UINT32 index;
                    if (source_offset + 2U > payload_end || position == 0)
                        return 0;
                    token = (UINT16)source[source_offset] |
                            ((UINT16)source[source_offset + 1U] << 8);
                    source_offset += 2U;
                    for (position--; position >= 0x10U; position >>= 1U) {
                        length_mask >>= 1U;
                        displacement_shift--;
                    }
                    length = (token & length_mask) + 3U;
                    displacement = (token >> displacement_shift) + 1U;
                    if (displacement > destination_offset - chunk_start ||
                        length > 4096U - (destination_offset - chunk_start) ||
                        length > destination_size - destination_offset)
                        return 0;
                    for (index = 0; index < length; index++) {
                        destination[destination_offset] =
                            destination[destination_offset - displacement];
                        destination_offset++;
                    }
                }
            }
        }
        if (source_offset != payload_end) return 0;
    }
    *written = destination_offset;
    return 1;
}

static int stream_cluster_sparse(const NTFS_STREAM *stream, UINT64 vcn,
                                 UINT8 *sparse)
{
    UINT64 start = 0;
    UINT32 index;
    for (index = 0; index < stream->run_count; index++) {
        if (vcn >= start && vcn - start < stream->runs[index].len) {
            *sparse = stream->runs[index].sparse;
            return 1;
        }
        start += stream->runs[index].len;
    }
    return 0;
}

static UINT64 read_compressed_stream(NTFS_CONTEXT *context,
                                     const NTFS_STREAM *stream,
                                     void *buffer, UINT64 capacity)
{
    UINT64 unit_clusters;
    UINT64 unit_bytes64;
    UINT32 unit_bytes;
    UINT8 *unit_buffer;
    UINT8 *compressed_buffer;
    UINT64 output_size = stream->data_size < capacity ?
                         stream->data_size : capacity;
    UINT64 output_offset = 0;
    UINT64 unit_vcn = 0;
    UINT64 logical_stream_size = stream->next_vcn *
                                 context->bytes_per_cluster;
    if (stream->compression_unit >= 31U) return 0;
    unit_clusters = 1ULL << stream->compression_unit;
    unit_bytes64 = unit_clusters * context->bytes_per_cluster;
    if (unit_bytes64 == 0 || unit_bytes64 > NTFS_MAX_COMPRESSION_UNIT_SIZE)
        return 0;
    unit_bytes = (UINT32)unit_bytes64;
    unit_buffer = (UINT8 *)kmalloc(unit_bytes);
    compressed_buffer = (UINT8 *)kmalloc(unit_bytes);
    if (unit_buffer == 0 || compressed_buffer == 0) {
        if (unit_buffer != 0) kfree(unit_buffer);
        if (compressed_buffer != 0) kfree(compressed_buffer);
        return 0;
    }
    while (output_offset < output_size && unit_vcn < stream->next_vcn) {
        UINT64 clusters = stream->next_vcn - unit_vcn;
        UINT64 cluster;
        UINT64 allocated_clusters = 0;
        UINT8 saw_sparse = 0;
        UINT32 decompressed = 0;
        UINT64 copy;
        int valid = 1;
        if (clusters > unit_clusters) clusters = unit_clusters;
        ntfs_zero(unit_buffer, unit_bytes);
        for (cluster = 0; cluster < clusters; cluster++) {
            UINT8 sparse;
            if (!stream_cluster_sparse(stream, unit_vcn + cluster, &sparse)) {
                valid = 0;
                break;
            }
            if (sparse) saw_sparse = 1;
            else {
                if (saw_sparse) {
                    valid = 0;
                    break;
                }
                allocated_clusters++;
            }
        }
        if (!valid) break;
        if (allocated_clusters == clusters) {
            UINT64 raw_size = clusters * context->bytes_per_cluster;
            if (read_from_runs(context, stream->runs, stream->run_count,
                               logical_stream_size,
                               unit_vcn * context->bytes_per_cluster,
                               unit_buffer, raw_size) != raw_size) break;
        } else if (allocated_clusters != 0) {
            UINT64 compressed_size = allocated_clusters *
                                     context->bytes_per_cluster;
            ntfs_zero(compressed_buffer, unit_bytes);
            if (read_from_runs(context, stream->runs, stream->run_count,
                               logical_stream_size,
                               unit_vcn * context->bytes_per_cluster,
                               compressed_buffer, compressed_size) !=
                compressed_size ||
                !lznt1_decompress(compressed_buffer,
                                  (UINT32)compressed_size,
                                  unit_buffer, unit_bytes, &decompressed))
                break;
        }
        copy = output_size - output_offset;
        if (copy > unit_bytes64) copy = unit_bytes64;
        ntfs_copy((UINT8 *)buffer + output_offset, unit_buffer, (UINT32)copy);
        output_offset += copy;
        unit_vcn += unit_clusters;
    }
    kfree(compressed_buffer);
    kfree(unit_buffer);
    return output_offset;
}

static UINT64 find_in_index_header(const UINT8 *ih_ptr, UINT32 available,
                                   const char *component)
{
    NTFS_INDEX_HEADER ih;
    UINT32 offset;
    if (available < sizeof(ih)) return 0;
    ntfs_copy(&ih, ih_ptr, sizeof(ih));
    if (ih.total_size > available || ih.first_entry_offset < sizeof(ih))
        return 0;
    offset = ih.first_entry_offset;
    while (offset + sizeof(NTFS_INDEX_ENTRY) <= ih.total_size) {
        NTFS_INDEX_ENTRY ie;
        ntfs_copy(&ie, ih_ptr + offset, sizeof(ie));
        if (ie.length < sizeof(NTFS_INDEX_ENTRY) ||
            ie.length > ih.total_size - offset) return 0;
        if (ie.flags & 0x02) break;
        if (ie.key_length >= (UINT16)sizeof(NTFS_FILE_NAME) &&
            ie.key_length <= ie.length - sizeof(NTFS_INDEX_ENTRY)) {
            const NTFS_FILE_NAME *fn = (const NTFS_FILE_NAME *)
                (ih_ptr + offset + sizeof(NTFS_INDEX_ENTRY));
            UINT32 required = sizeof(NTFS_FILE_NAME) +
                              (UINT32)fn->name_length * 2U;
            if (fn->name_space != 2 && required <= ie.key_length) {
                char name[256];
                utf16_to_utf8((const UINT8 *)fn + sizeof(NTFS_FILE_NAME),
                              fn->name_length, name, sizeof(name));
                if (ntfs_name_matches(name, component))
                    return ie.mft_ref & 0x0000FFFFFFFFFFFFull;
            }
        }
        offset += ie.length;
    }
    return 0;
}

/* List inline INDEX_ROOT entries into NTFS_FILE_INFO array */
static UINT64 list_index_root(
    const UINT8      *index_root_value,
    UINT32            value_len,
    NTFS_FILE_INFO   *out,
    UINT64            cap)
{
    /* INDEX_ROOT value layout:
       [NTFS_INDEX_ROOT: 16 bytes]
       [NTFS_INDEX_HEADER: 16 bytes]
       [inline index entries]  */
    const UINT8          *ih_ptr;
    if (value_len < 32) return 0;
    ih_ptr = index_root_value + 16; /* skip NTFS_INDEX_ROOT */
    return list_index_header(ih_ptr, value_len - 16U, out, cap);
}

static int index_bitmap_active(const UINT8 *bitmap, UINT64 bitmap_size,
                               UINT32 index)
{
    if (bitmap == 0) return 1;
    if (index / 8U >= bitmap_size) return 0;
    return (bitmap[index / 8U] & (1U << (index & 7U))) != 0;
}

static UINT8 *load_index_bitmap(NTFS_CONTEXT *context, UINT64 base_record,
                                UINT64 *bitmap_size)
{
    NTFS_STREAM *stream = (NTFS_STREAM *)kmalloc(sizeof(NTFS_STREAM));
    UINT8 *bitmap;
    UINT64 read;
    if (stream == 0) return 0;
    if (!collect_attribute_stream(context, base_record, ATTR_BITMAP, 0,
                                  stream) || stream->data_size == 0 ||
        stream->data_size > NTFS_MAX_ATTRIBUTE_LIST_SIZE ||
        (stream->flags & (0x0001U | 0x4000U)) != 0) {
        kfree(stream);
        return 0;
    }
    bitmap = (UINT8 *)kmalloc((UINTN)stream->data_size);
    if (bitmap == 0) {
        kfree(stream);
        return 0;
    }
    if (stream->resident) {
        ntfs_copy(bitmap, stream->resident_value, stream->resident_length);
        read = stream->resident_length;
    } else {
        read = read_from_runs(context, stream->runs, stream->run_count,
                              stream->data_size, 0, bitmap,
                              stream->data_size);
    }
    *bitmap_size = stream->data_size;
    kfree(stream);
    if (read != *bitmap_size) {
        kfree(bitmap);
        return 0;
    }
    return bitmap;
}

static UINT64 list_index_allocation(NTFS_CONTEXT *context, UINT64 base_record,
                                    NTFS_FILE_INFO *out, UINT64 cap)
{
    NTFS_STREAM *allocation;
    UINT8 *bitmap;
    UINT64 bitmap_size = 0;
    UINT32 record_count;
    UINT32 index;
    UINT64 count = 0;
    bitmap = load_index_bitmap(context, base_record, &bitmap_size);
    allocation = (NTFS_STREAM *)kmalloc(sizeof(NTFS_STREAM));
    if (allocation == 0) {
        if (bitmap != 0) kfree(bitmap);
        return 0;
    }
    if (!collect_attribute_stream(context, base_record, ATTR_INDEX_ALLOCATION,
                                  0, allocation) || allocation->resident ||
        (allocation->flags & (0x0001U | 0x4000U)) != 0 ||
        context->index_record_size == 0) {
        if (bitmap != 0) kfree(bitmap);
        kfree(allocation);
        return 0;
    }
    record_count = (UINT32)(allocation->data_size /
                            context->index_record_size);
    for (index = 0; index < record_count && count < cap; index++) {
        UINT8 *record = context->write_buf;
        if (!index_bitmap_active(bitmap, bitmap_size, index)) continue;
        if (read_from_runs(context, allocation->runs, allocation->run_count,
                           allocation->data_size,
                           (UINT64)index * context->index_record_size,
                           record, context->index_record_size) !=
            context->index_record_size) break;
        if (record[0] != 'I' || record[1] != 'N' ||
            record[2] != 'D' || record[3] != 'X' ||
            !apply_fixup(record, context->index_record_size,
                         context->bytes_per_sector)) continue;
        count += list_index_header(record + 24U,
                                   context->index_record_size - 24U,
                                   out + count, cap - count);
    }
    if (bitmap != 0) kfree(bitmap);
    kfree(allocation);
    return count;
}

static UINT64 find_in_index_allocation(NTFS_CONTEXT *context,
                                       UINT64 base_record,
                                       const char *component)
{
    NTFS_STREAM *allocation;
    UINT8 *bitmap;
    UINT64 bitmap_size = 0;
    UINT32 record_count;
    UINT32 index;
    bitmap = load_index_bitmap(context, base_record, &bitmap_size);
    allocation = (NTFS_STREAM *)kmalloc(sizeof(NTFS_STREAM));
    if (allocation == 0) {
        if (bitmap != 0) kfree(bitmap);
        return 0;
    }
    if (!collect_attribute_stream(context, base_record, ATTR_INDEX_ALLOCATION,
                                  0, allocation) || allocation->resident ||
        (allocation->flags & (0x0001U | 0x4000U)) != 0 ||
        context->index_record_size == 0) {
        if (bitmap != 0) kfree(bitmap);
        kfree(allocation);
        return 0;
    }
    record_count = (UINT32)(allocation->data_size /
                            context->index_record_size);
    for (index = 0; index < record_count; index++) {
        UINT64 found;
        UINT8 *record = context->write_buf;
        if (!index_bitmap_active(bitmap, bitmap_size, index)) continue;
        if (read_from_runs(context, allocation->runs, allocation->run_count,
                           allocation->data_size,
                           (UINT64)index * context->index_record_size,
                           record, context->index_record_size) !=
            context->index_record_size) break;
        if (record[0] != 'I' || record[1] != 'N' ||
            record[2] != 'D' || record[3] != 'X' ||
            !apply_fixup(record, context->index_record_size,
                         context->bytes_per_sector)) continue;
        found = find_in_index_header(record + 24U,
                                     context->index_record_size - 24U,
                                     component);
        if (found != 0) {
            if (bitmap != 0) kfree(bitmap);
            kfree(allocation);
            return found;
        }
    }
    if (bitmap != 0) kfree(bitmap);
    kfree(allocation);
    return 0;
}

/* Search INDEX_ROOT of current g_mft_buf for a path component.
   Returns MFT record number (0 = not found). */
static UINT64 find_in_index(NTFS_CONTEXT *context, UINT64 base_record,
                            const char *component)
{
    FIND_ATTR_CTX ctx;
    const UINT8  *ih_ptr;

    ctx.target_type = ATTR_INDEX_ROOT;
    ctx.found       = 0;
    iterate_attrs(context->mft_buf, find_attr_cb, &ctx);
    if (!ctx.found || ctx.non_resident) return 0;

    if (ctx.value_len < 32) return 0;
    ih_ptr = ctx.value + 16;
    {
        UINT64 found = find_in_index_header(ih_ptr, ctx.value_len - 16U,
                                            component);
        if (found != 0) return found;
    }
    return find_in_index_allocation(context, base_record, component);
}

/* Resolve an absolute path to its MFT record number.
   Leaves the record loaded in g_mft_buf on success.
   Returns 0 on failure. */
static UINT64 ntfs_resolve(NTFS_CONTEXT *context, const char *path)
{
    UINT64 current = MFT_RECORD_ROOT;
    const char *p  = path;
    char component[256];
    UINT32 ci;
    UINT64 found;

    while (*p == '/') p++;

    if (*p == '\0') {
        /* Root directory */
        if (!read_mft_record(context, current)) return 0;
        return current;
    }

    if (!read_mft_record(context, current)) return 0;

    for (;;) {
        ci = 0;
        while (*p && *p != '/' && ci < 255) component[ci++] = *p++;
        component[ci] = '\0';
        while (*p == '/') p++;

        found = find_in_index(context, current, component);
        if (found == 0) return 0;

        if (!read_mft_record(context, found)) return 0;

        if (*p == '\0') return found;

        /* If not the last component, must be a directory */
        if (!(((const NTFS_FILE_RECORD *)context->mft_buf)->flags & 0x02)) return 0;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */
static int ntfs_detect_context(NTFS_CONTEXT *context)
{
    UINT8 sec[NTFS_MAX_SECTOR_SIZE];
    if (!ntfs_read_sector(context, 0, sec)) return 0;
    /* OEM ID "NTFS    " at offset 3 */
    return sec[3]=='N' && sec[4]=='T' && sec[5]=='F' && sec[6]=='S' &&
           sec[7]==' ' && sec[8]==' ' && sec[9]==' ' && sec[10]==' ' ? 1 : 0;
}

static int ntfs_initialize_context(NTFS_CONTEXT *context)
{
    UINT8  boot_sec[NTFS_MAX_SECTOR_SIZE];
    /* Reuse the global g_mft_buf for the initial MFT record 0 read —
       avoids a large local that would require __chkstk (no CRT). */
    UINT8 *mft0_buf = context->mft_buf;
    NTFS_FILE_RECORD *hdr;
    FIND_ATTR_CTX  ctx;
    NTFS_RUN       temp_runs[MAX_RUNS];
    UINT32         i, sec_count;
    UINT64         mft_lba;

    ctx.target_type  = 0;
    ctx.value        = 0;
    ctx.value_len    = 0;
    ctx.non_resident = 0;
    ctx.runlist      = 0;
    ctx.runlist_len  = 0;
    ctx.found        = 0;

    if (!ntfs_read_sector(context, 0, boot_sec)) return 0;

    /* Extract boot sector fields manually (avoid struct alignment risk) */
    context->boot.bytes_per_sector =
        (UINT16)boot_sec[11] | ((UINT16)boot_sec[12] << 8);
    context->boot.sectors_per_cluster = boot_sec[13];
    context->boot.total_sectors = 0;
    for (i = 0; i < 8; i++)
        context->boot.total_sectors |= (UINT64)boot_sec[40 + i] << (i * 8);
    /* mft_lcn at offset 48 */
    context->boot.mft_lcn = 0;
    for (i = 0; i < 8; i++)
        context->boot.mft_lcn |= (UINT64)boot_sec[48 + i] << (i * 8);
    context->boot.mft_mirror_lcn = 0;
    for (i = 0; i < 8; i++)
        context->boot.mft_mirror_lcn |= (UINT64)boot_sec[56 + i] << (i * 8);
    /* clusters_per_file_record at offset 64 */
    context->boot.clusters_per_file_record = (INT8)boot_sec[64];
    context->boot.clusters_per_index_record = (INT8)boot_sec[68];

    if ((context->boot.bytes_per_sector != 512 &&
         context->boot.bytes_per_sector != 1024 &&
         context->boot.bytes_per_sector != 2048 &&
         context->boot.bytes_per_sector != 4096) ||
        (context->device != 0 && context->device->logical_block_size !=
                                context->boot.bytes_per_sector)) return 0;
    if (context->boot.sectors_per_cluster == 0) return 0;

    context->bytes_per_sector = context->boot.bytes_per_sector;
    context->bytes_per_cluster = (UINT32)context->boot.bytes_per_sector *
                                 context->boot.sectors_per_cluster;

    /* MFT record size */
    if (context->boot.clusters_per_file_record < 0) {
        context->mft_record_size =
            1u << (UINT32)(-context->boot.clusters_per_file_record);
    } else {
        context->mft_record_size =
            (UINT32)context->boot.clusters_per_file_record * context->bytes_per_cluster;
    }
    if (context->mft_record_size == 0 || context->mft_record_size > MFT_BUF_SIZE) return 0;
    if (context->boot.clusters_per_index_record < 0) {
        context->index_record_size =
            1U << (UINT32)(-context->boot.clusters_per_index_record);
    } else {
        context->index_record_size =
            (UINT32)context->boot.clusters_per_index_record *
            context->bytes_per_cluster;
    }
    if (context->index_record_size == 0 ||
        context->index_record_size > MFT_BUF_SIZE) return 0;

    /* Read MFT record 0 directly from mft_lcn */
    mft_lba = context->boot.mft_lcn * context->boot.sectors_per_cluster;
    sec_count = (context->mft_record_size + context->bytes_per_sector - 1U) /
                context->bytes_per_sector;

    for (i = 0; i < sec_count; i++) {
        UINT8  sec[NTFS_MAX_SECTOR_SIZE];
        UINT32 chunk = context->mft_record_size -
                       i * context->bytes_per_sector;
        if (chunk > context->bytes_per_sector) chunk = context->bytes_per_sector;
        if (!ntfs_read_sector(context, mft_lba + i, sec)) return 0;
        ntfs_copy(mft0_buf + i * context->bytes_per_sector, sec, chunk);
    }

    if (!apply_fixup(mft0_buf, context->mft_record_size,
                     context->bytes_per_sector) ||
        mft0_buf[0] != 'F' || mft0_buf[1] != 'I' ||
        mft0_buf[2] != 'L' || mft0_buf[3] != 'E') {
        mft_lba = context->boot.mft_mirror_lcn *
                  context->boot.sectors_per_cluster;
        for (i = 0; i < sec_count; i++) {
            UINT8 sec[NTFS_MAX_SECTOR_SIZE];
            UINT32 chunk = context->mft_record_size -
                           i * context->bytes_per_sector;
            if (chunk > context->bytes_per_sector)
                chunk = context->bytes_per_sector;
            if (!ntfs_read_sector(context, mft_lba + i, sec)) return 0;
            ntfs_copy(mft0_buf + i * context->bytes_per_sector, sec, chunk);
        }
        if (!apply_fixup(mft0_buf, context->mft_record_size,
                         context->bytes_per_sector) ||
            mft0_buf[0] != 'F' || mft0_buf[1] != 'I' ||
            mft0_buf[2] != 'L' || mft0_buf[3] != 'E') return 0;
        logger_write("NTFS", "primary MFT recovered from MFT mirror");
    }

    /* Find $DATA attribute of $MFT to get MFT's own runlist */
    ctx.target_type = ATTR_DATA;
    ctx.found       = 0;
    hdr = (NTFS_FILE_RECORD *)mft0_buf;
    {
        UINT32 off = hdr->attr_offset;
        while (off + sizeof(NTFS_ATTR_HEADER) <= hdr->used_size) {
            const NTFS_ATTR_HEADER *ah = (const NTFS_ATTR_HEADER *)(mft0_buf + off);
            if (ah->type == ATTR_END || ah->length == 0) break;
            if (ah->type == ATTR_DATA && ah->non_resident) {
                const NTFS_NONRESIDENT *nr =
                    (const NTFS_NONRESIDENT *)(mft0_buf + off + sizeof(NTFS_ATTR_HEADER));
                ctx.runlist     = mft0_buf + off + nr->runlist_offset;
                ctx.runlist_len = ah->length - nr->runlist_offset;
                ctx.value_len   = (UINT32)nr->data_size;
                context->mft_data_size = nr->data_size;
                ctx.found       = 1;
                break;
            }
            off += ah->length;
        }
    }

    if (!ctx.found) return 0;

    context->mft_run_count = decode_runlist(ctx.runlist, ctx.runlist_len,
                                            temp_runs, MAX_RUNS);
    if (context->mft_run_count == 0) return 0;

    for (i = 0; i < context->mft_run_count; i++) context->mft_runs[i] = temp_runs[i];

    context->initialized = 1;
    context->write_allowed = 0;
    context->read_only_reason = NTFS_READ_ONLY_REASON_UPCASE;
    context->volume_flags = 0;
    if (ntfs_load_upcase_table(context)) {
        context->read_only_reason = NTFS_READ_ONLY_REASON_DEVICE;
    }
    if (context->read_only_reason == NTFS_READ_ONLY_REASON_DEVICE &&
        (context->device == 0 ||
         !block_device_has_capability(context->device,
                                      BLOCK_DEVICE_FLAG_READ_ONLY))) {
        context->read_only_reason = NTFS_READ_ONLY_REASON_VOLUME_INFO;
    }
    if (context->read_only_reason == NTFS_READ_ONLY_REASON_VOLUME_INFO &&
        read_mft_record(context, 3)) {
        FIND_ATTR_CTX volume = { 0 };
        volume.target_type = ATTR_VOLUME_INFORMATION;
        iterate_attrs(context->mft_buf, find_attr_cb, &volume);
        if (volume.found && !volume.non_resident && volume.value_len >= 12) {
            UINT16 flags = (UINT16)volume.value[10] |
                           ((UINT16)volume.value[11] << 8);
            context->volume_flags = flags;
            context->write_allowed = flags == 0;
            context->read_only_reason = NTFS_READ_ONLY_REASON_NONE;
            if ((flags & 0x0001U) != 0)
                context->read_only_reason =
                    NTFS_READ_ONLY_REASON_DIRTY_LOG_REPLAY_REQUIRED;
            else if (flags != 0)
                context->read_only_reason =
                    NTFS_READ_ONLY_REASON_UNSUPPORTED_VOLUME_FLAGS;
            if (flags != 0) {
                context->write_allowed = 0;
                logger_write_hex("WARN", "NTFS volume forced read-only flags",
                                 flags);
                logger_write("WARN",
                             ntfs_context_read_only_reason_string(context));
            }
        }
    }
    logger_write("NTFS", "initialized");
    return 1;
}

UINT64 ntfs_context_file_size(NTFS_CONTEXT *context, const char *path)
{
    NTFS_STREAM *stream;
    UINT64 size;
    UINT64 record;
    if (context == 0 || !context->initialized ||
        (record = ntfs_resolve(context, path)) == 0) return 0;
    stream = (NTFS_STREAM *)kmalloc(sizeof(NTFS_STREAM));
    if (stream == 0) return 0;
    if (!collect_attribute_stream(context, record, ATTR_DATA, 1, stream)) {
        kfree(stream);
        return 0;
    }
    size = stream->data_size;
    kfree(stream);
    return size;
}

int ntfs_context_exists(NTFS_CONTEXT *context, const char *path)
{
    return context != 0 && context->initialized && ntfs_resolve(context, path) != 0;
}

int ntfs_context_is_directory(NTFS_CONTEXT *context, const char *path)
{
    const NTFS_FILE_RECORD *hdr;
    if (context == 0 || !context->initialized || !ntfs_resolve(context, path)) return 0;
    hdr = (const NTFS_FILE_RECORD *)context->mft_buf;
    return (hdr->flags & 0x02) ? 1 : 0;
}

UINT64 ntfs_context_read_file(NTFS_CONTEXT *context, const char *path,
                              void *buffer, UINT64 capacity)
{
    NTFS_STREAM *stream;
    UINT64 result;
    UINT64        record;

    if (context == 0 || !context->initialized || buffer == 0 || capacity == 0 ||
        (record = ntfs_resolve(context, path)) == 0) return 0;
    stream = (NTFS_STREAM *)kmalloc(sizeof(NTFS_STREAM));
    if (stream == 0) return 0;
    if (!collect_attribute_stream(context, record, ATTR_DATA, 1, stream) ||
        (stream->flags & 0x4000U) != 0) {
        kfree(stream);
        return 0;
    }
    if (stream->resident) {
        result = stream->resident_length < capacity ?
                 stream->resident_length : capacity;
        ntfs_copy(buffer, stream->resident_value, (UINT32)result);
    } else if ((stream->flags & 0x0001U) != 0) {
        result = read_compressed_stream(context, stream, buffer, capacity);
    } else {
        result = read_from_runs(context, stream->runs, stream->run_count,
                                stream->data_size, 0, buffer, capacity);
    }
    kfree(stream);
    return result;
}

static int ntfs_split_parent_path(const char *path, char *parent,
                                  UINT32 parent_capacity, const char **name)
{
    UINT32 length = 0;
    UINT32 slash = 0;
    UINT32 index;
    if (path == 0 || path[0] != '/') return 0;
    while (path[length] != '\0') {
        if (path[length] == '/') slash = length;
        length++;
    }
    if (length < 2 || slash + 1U >= length || slash >= parent_capacity)
        return 0;
    if (slash == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        for (index = 0; index < slash; index++) parent[index] = path[index];
        parent[slash] = '\0';
    }
    *name = path + slash + 1U;
    return 1;
}

static int ntfs_create_entry(NTFS_CONTEXT *context, const char *path,
                             int directory, const void *data, UINT32 data_size)
{
    NTFS_TRANSACTION transaction;
    char parent_path[512];
    const char *name_text;
    UINT16 name[255];
    UINT32 name_length;
    UINT64 parent_record;
    UINT64 new_record;
    UINT16 parent_sequence;
    UINT16 sequence = 1;
    UINT8 *new_record_bytes;
    UINT64 *clusters = 0;
    UINT8 *runlist = 0;
    NTFS_RUN *runs = 0;
    UINT32 cluster_count = 0;
    UINT32 run_count = 0;
    UINT32 runlist_length = 0;
    int record_built;
    int record_written = 0;
    UINT8 file_name_value[sizeof(NTFS_FILE_NAME) + 510U];
    UINT32 file_name_size;
    if (context == 0 || !context->initialized || !context->write_allowed ||
        (!directory && data_size != 0 && data == 0) ||
        !ntfs_split_parent_path(path, parent_path, sizeof(parent_path),
                                &name_text) ||
        !utf8_to_utf16(name_text, name, 255, &name_length)) {
        ntfs_set_error("ntfs: invalid create path or read-only context");
        return 0;
    }
    ntfs_set_error("ntfs: create started");
    if (ntfs_resolve(context, path) != 0) {
        ntfs_set_error("ntfs: target already exists");
        return 0;
    }
    if (!ntfs_mutation_begin(context, &transaction, 0)) {
        ntfs_set_error("ntfs: transaction begin failed");
        return 0;
    }
    parent_record = ntfs_resolve(context, parent_path);
    if (parent_record == 0 ||
        (((NTFS_FILE_RECORD *)context->mft_buf)->flags & 0x02U) == 0) {
        ntfs_set_error("ntfs: parent directory not found");
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    parent_sequence = ((NTFS_FILE_RECORD *)context->mft_buf)->sequence;
    if (!ntfs_allocate_mft_records(context, 1, &new_record)) {
        ntfs_set_error("ntfs: MFT record allocation failed");
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    if (read_mft_record(context, new_record)) {
        sequence = (UINT16)(((NTFS_FILE_RECORD *)context->mft_buf)->sequence + 1U);
        if (sequence == 0) sequence = 1;
    }
    new_record_bytes = (UINT8 *)kmalloc(context->mft_record_size);
    if (new_record_bytes == 0) {
        ntfs_set_error("ntfs: out of memory for MFT record");
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    record_built = ntfs_build_basic_record(
        context, new_record_bytes, new_record, sequence,
        parent_record | ((UINT64)parent_sequence << 48), name,
        (UINT8)name_length, directory, data, data_size);
    if (!record_built && !directory && data_size != 0) {
        cluster_count = (UINT32)(((UINT64)data_size +
                                  context->bytes_per_cluster - 1U) /
                                 context->bytes_per_cluster);
        if (cluster_count == 0 || cluster_count > 0x00100000U) {
            kfree(new_record_bytes);
            ntfs_set_error("ntfs: file cluster allocation too large");
            (void)ntfs_transaction_rollback(context, &transaction);
            return 0;
        }
        clusters = (UINT64 *)kmalloc((UINTN)cluster_count * sizeof(UINT64));
        runlist = (UINT8 *)kmalloc(context->mft_record_size);
        runs = (NTFS_RUN *)kmalloc(sizeof(NTFS_RUN) * NTFS_MAX_STREAM_RUNS);
        if (clusters != 0 && runlist != 0 && runs != 0 &&
            ntfs_allocate_clusters(context, cluster_count, clusters)) {
            runlist_length = ntfs_encode_cluster_runlist(
                clusters, cluster_count, runlist, context->mft_record_size,
                runs, &run_count);
            if (runlist_length != 0 &&
                ntfs_zero_cluster_list(context, clusters, cluster_count) &&
                write_to_runs(context, runs, run_count,
                              (UINT64)cluster_count *
                                  context->bytes_per_cluster,
                              data, data_size)) {
                record_built = ntfs_build_nonresident_file_record(
                    context, new_record_bytes, new_record, sequence,
                    parent_record | ((UINT64)parent_sequence << 48),
                    name, (UINT8)name_length, runlist, runlist_length,
                    cluster_count,
                     (UINT64)cluster_count * context->bytes_per_cluster,
                     data_size);
                if (!record_built) {
                    record_written = ntfs_write_split_nonresident_file(
                        context, new_record, sequence,
                        parent_record | ((UINT64)parent_sequence << 48),
                        name, (UINT8)name_length, runs, run_count,
                        (UINT64)cluster_count * context->bytes_per_cluster,
                        data_size);
                    record_built = record_written;
                }
            }
        }
    }
    if (clusters != 0) kfree(clusters);
    if (runlist != 0) kfree(runlist);
    if (runs != 0) kfree(runs);
    if (!record_built || (!record_written &&
        !write_mft_record(context, new_record, new_record_bytes))) {
        if (new_record_bytes != 0) kfree(new_record_bytes);
        ntfs_set_error("ntfs: new MFT record write failed");
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    file_name_size = ntfs_build_file_name_value(
        file_name_value, sizeof(file_name_value),
        parent_record | ((UINT64)parent_sequence << 48), name,
        (UINT8)name_length, directory, data_size);
    if (file_name_size == 0 ||
        !ntfs_index_root_insert(context, parent_record,
                                new_record | ((UINT64)sequence << 48),
                                file_name_value, (UINT16)file_name_size)) {
        kfree(new_record_bytes);
        ntfs_set_error("ntfs: parent directory index insert failed");
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    kfree(new_record_bytes);
    if (!ntfs_remount_verify_path(context, path, 1, !directory, data_size)) {
        ntfs_set_error("ntfs: remount verification failed");
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    if (!ntfs_mutation_commit(context, &transaction)) {
        ntfs_set_error("ntfs: transaction commit failed");
        return 0;
    }
    ntfs_set_error("ntfs: ok");
    return 1;
}

static int ntfs_rewrite_file_storage(NTFS_CONTEXT *context,
                                     UINT64 record_number,
                                     const void *buffer, UINT64 size)
{
    NTFS_STREAM *old_stream;
    UINT64 old_extensions[NTFS_MAX_STREAM_RUNS];
    UINT32 old_extension_count;
    NTFS_RESIDENT *file_name_resident;
    NTFS_FILE_NAME *file_name;
    UINT16 name[255];
    UINT8 name_length;
    UINT64 parent_reference;
    UINT64 parent_record;
    UINT16 sequence;
    UINT64 *clusters = 0;
    NTFS_RUN *runs = 0;
    UINT8 *runlist = 0;
    UINT8 *record = 0;
    UINT32 cluster_count = 0;
    UINT32 run_count = 0;
    UINT32 runlist_length = 0;
    UINT32 index;
    int written = 0;
    UINT8 index_key[sizeof(NTFS_FILE_NAME) + 510U];
    UINT32 index_key_size;
    if (!read_mft_record(context, record_number)) return 0;
    sequence = ((NTFS_FILE_RECORD *)context->mft_buf)->sequence;
    if (((NTFS_FILE_RECORD *)context->mft_buf)->hard_links != 1) return 0;
    file_name = ntfs_loaded_primary_file_name(context, &file_name_resident);
    if (file_name == 0 || file_name->name_length == 0 ||
        file_name->name_length > 255U) return 0;
    name_length = file_name->name_length;
    parent_reference = file_name->parent_ref;
    parent_record = parent_reference & 0x0000FFFFFFFFFFFFULL;
    ntfs_copy(name, (UINT8 *)file_name + sizeof(NTFS_FILE_NAME),
              (UINT32)name_length * 2U);
    old_stream = (NTFS_STREAM *)kmalloc(sizeof(*old_stream));
    if (old_stream == 0 ||
        !collect_attribute_stream(context, record_number, ATTR_DATA, 1,
                                  old_stream) ||
        (old_stream->flags & (0x0001U | 0x4000U | 0x8000U)) != 0) {
        if (old_stream != 0) kfree(old_stream);
        return 0;
    }
    old_extension_count = ntfs_collect_extension_records(
        context, record_number, ATTR_DATA, old_extensions,
        NTFS_MAX_STREAM_RUNS);
    record = (UINT8 *)kmalloc(context->mft_record_size);
    if (record == 0) goto failed;
    if (size == 0 || size <= context->mft_record_size / 4U) {
        written = ntfs_build_basic_record(
            context, record, record_number, sequence, parent_reference,
            name, name_length, 0, buffer, (UINT32)size) &&
            write_mft_record(context, record_number, record);
    } else {
        cluster_count = (UINT32)((size + context->bytes_per_cluster - 1U) /
                                 context->bytes_per_cluster);
        if (cluster_count == 0 || cluster_count > 0x00100000U) goto failed;
        clusters = (UINT64 *)kmalloc((UINTN)cluster_count * sizeof(UINT64));
        runs = (NTFS_RUN *)kmalloc(sizeof(NTFS_RUN) * NTFS_MAX_STREAM_RUNS);
        runlist = (UINT8 *)kmalloc(context->mft_record_size);
        if (clusters == 0 || runs == 0 || runlist == 0 ||
            !ntfs_allocate_clusters(context, cluster_count, clusters))
            goto failed;
        runlist_length = ntfs_encode_cluster_runlist(
            clusters, cluster_count, runlist, context->mft_record_size,
            runs, &run_count);
        if (runlist_length == 0 ||
            !ntfs_zero_cluster_list(context, clusters, cluster_count) ||
            !write_to_runs(context, runs, run_count,
                           (UINT64)cluster_count * context->bytes_per_cluster,
                           buffer, size)) goto failed;
        written = ntfs_build_nonresident_file_record(
            context, record, record_number, sequence, parent_reference,
            name, name_length, runlist, runlist_length, cluster_count,
            (UINT64)cluster_count * context->bytes_per_cluster, size) &&
            write_mft_record(context, record_number, record);
        if (!written)
            written = ntfs_write_split_nonresident_file(
                context, record_number, sequence, parent_reference,
                name, name_length, runs, run_count,
                (UINT64)cluster_count * context->bytes_per_cluster, size);
    }
    if (!written) goto failed;
    if (!old_stream->resident &&
        !ntfs_release_stream_clusters(context, old_stream)) goto failed;
    for (index = 0; index < old_extension_count; index++) {
        if (!read_mft_record(context, old_extensions[index])) goto failed;
        ((NTFS_FILE_RECORD *)context->mft_buf)->flags &= 0xFFFEU;
        ((NTFS_FILE_RECORD *)context->mft_buf)->sequence++;
        if (((NTFS_FILE_RECORD *)context->mft_buf)->sequence == 0)
            ((NTFS_FILE_RECORD *)context->mft_buf)->sequence = 1;
        if (!write_mft_record(context, old_extensions[index],
                              context->mft_buf)) goto failed;
    }
    if (old_extension_count != 0 &&
        !ntfs_free_mft_records(context, old_extension_count,
                               old_extensions)) goto failed;
    index_key_size = ntfs_build_file_name_value(
        index_key, sizeof(index_key), parent_reference, name, name_length,
        0, size);
    if (index_key_size == 0 ||
        !ntfs_index_root_remove(context, parent_record, record_number) ||
        !ntfs_index_root_insert(context, parent_record,
                                record_number | ((UINT64)sequence << 48),
                                index_key, (UINT16)index_key_size)) goto failed;
    if (runlist != 0) kfree(runlist);
    if (runs != 0) kfree(runs);
    if (clusters != 0) kfree(clusters);
    kfree(record);
    kfree(old_stream);
    return 1;
failed:
    if (runlist != 0) kfree(runlist);
    if (runs != 0) kfree(runs);
    if (clusters != 0) kfree(clusters);
    if (record != 0) kfree(record);
    if (old_stream != 0) kfree(old_stream);
    return 0;
}

static int ntfs_write_existing_file(NTFS_CONTEXT *context, const char *path,
                                    const void *buffer, UINT64 size)
{
    NTFS_FILE_RECORD *record;
    UINT64 record_number;
    UINT32 offset;
    NTFS_ATTR_HEADER *data_attr = 0;
    NTFS_RESIDENT *data_resident = 0;
    NTFS_NONRESIDENT *data_nonresident = 0;
    UINT32 resident_capacity = 0;
    if (context == 0 || !context->initialized || !context->write_allowed ||
        size > 0xFFFFFFFFULL ||
        (size != 0 && buffer == 0)) return 0;
    record_number = ntfs_resolve(context, path);
    if (record_number == 0) return 0;
    record = (NTFS_FILE_RECORD *)context->mft_buf;
    if ((record->flags & 0x02U) != 0 || record->used_size > context->mft_record_size)
        return 0;
    offset = record->attr_offset;
    while (offset + sizeof(NTFS_ATTR_HEADER) <= record->used_size) {
        NTFS_ATTR_HEADER *attribute =
            (NTFS_ATTR_HEADER *)(context->mft_buf + offset);
        if (attribute->type == ATTR_END) break;
        if (attribute->length < sizeof(NTFS_ATTR_HEADER) ||
            attribute->length > record->used_size - offset) return 0;
        if (attribute->type == ATTR_DATA && attribute->name_length == 0) {
            data_attr = attribute;
            if (!attribute->non_resident) {
                NTFS_RESIDENT *resident;
                if (attribute->length < sizeof(NTFS_ATTR_HEADER) +
                                        sizeof(NTFS_RESIDENT)) return 0;
                resident = (NTFS_RESIDENT *)((UINT8 *)attribute +
                                             sizeof(NTFS_ATTR_HEADER));
                if (resident->value_offset > attribute->length ||
                    resident->value_length >
                        attribute->length - resident->value_offset) return 0;
                resident_capacity = attribute->length - resident->value_offset;
                data_resident = resident;
            } else {
                if (attribute->flags != 0 ||
                    attribute->length < sizeof(NTFS_ATTR_HEADER) +
                                        sizeof(NTFS_NONRESIDENT)) return 0;
                data_nonresident = (NTFS_NONRESIDENT *)((UINT8 *)attribute +
                                             sizeof(NTFS_ATTR_HEADER));
                if (data_nonresident->lowest_vcn != 0 ||
                    data_nonresident->runlist_offset >= attribute->length)
                    return 0;
            }
            break;
        }
        offset += attribute->length;
    }
    if (data_attr == 0)
        return ntfs_rewrite_file_storage(context, record_number, buffer, size);
    if (data_resident != 0) {
        UINT8 *value = (UINT8 *)data_attr + data_resident->value_offset;
        UINT32 index;
        if (size > resident_capacity)
            return ntfs_rewrite_file_storage(context, record_number,
                                             buffer, size);
        for (index = 0; index < resident_capacity; index++) value[index] = 0;
        if (size != 0) ntfs_copy(value, buffer, (UINT32)size);
        data_resident->value_length = (UINT32)size;
    } else {
        NTFS_RUN runs[MAX_RUNS];
        UINT32 run_count = decode_writable_runlist(
            (UINT8 *)data_attr + data_nonresident->runlist_offset,
            data_attr->length - data_nonresident->runlist_offset,
            runs, MAX_RUNS);
        if (run_count == 0) return 0;
        if (size > data_nonresident->alloc_size)
            return ntfs_rewrite_file_storage(context, record_number,
                                             buffer, size);
        if (
            (size != 0 && !write_to_runs(context, runs, run_count,
                                         data_nonresident->alloc_size,
                                         buffer, size))) return 0;
        data_nonresident->data_size = size;
        data_nonresident->init_size = size;
    }
    offset = record->attr_offset;
    while (offset + sizeof(NTFS_ATTR_HEADER) <= record->used_size) {
        NTFS_ATTR_HEADER *attribute =
            (NTFS_ATTR_HEADER *)(context->mft_buf + offset);
        if (attribute->type == ATTR_END) break;
        if (attribute->length < sizeof(NTFS_ATTR_HEADER) ||
            attribute->length > record->used_size - offset) return 0;
        if (attribute->type == ATTR_FILE_NAME && !attribute->non_resident) {
            NTFS_RESIDENT *resident = (NTFS_RESIDENT *)((UINT8 *)attribute +
                                         sizeof(NTFS_ATTR_HEADER));
            if (resident->value_offset <= attribute->length &&
                resident->value_length >= sizeof(NTFS_FILE_NAME) &&
                resident->value_length <= attribute->length - resident->value_offset) {
                NTFS_FILE_NAME *file_name = (NTFS_FILE_NAME *)
                    ((UINT8 *)attribute + resident->value_offset);
                file_name->real_size = size;
            }
        }
        offset += attribute->length;
    }
    if (!write_mft_record(context, record_number, context->mft_buf)) {
        (void)read_mft_record(context, record_number);
        return 0;
    }
    return 1;
}

int ntfs_context_write_file(NTFS_CONTEXT *context, const char *path,
                            const void *buffer, UINT64 size)
{
    NTFS_TRANSACTION transaction;
    if (context == 0 || !context->initialized || !context->write_allowed)
        return 0;
    if (!ntfs_context_exists(context, path)) {
        if (size > 0xFFFFFFFFULL) return 0;
        return ntfs_create_entry(context, path, 0, buffer, (UINT32)size);
    }
    if (!ntfs_mutation_begin(context, &transaction, 0)) return 0;
    if (!ntfs_write_existing_file(context, path, buffer, size)) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    if (!ntfs_remount_verify_path(context, path, 1, 1, size)) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    return ntfs_mutation_commit(context, &transaction);
}

int ntfs_context_create_directory(NTFS_CONTEXT *context, const char *path)
{
    return ntfs_create_entry(context, path, 1, 0, 0);
}

static NTFS_FILE_NAME *ntfs_loaded_primary_file_name(NTFS_CONTEXT *context,
                                                     NTFS_RESIDENT **resident)
{
    NTFS_ATTR_HEADER *attribute =
        ntfs_find_mutable_resident_attribute(context, ATTR_FILE_NAME, resident);
    if (attribute == 0 || (*resident)->value_length < sizeof(NTFS_FILE_NAME))
        return 0;
    return (NTFS_FILE_NAME *)((UINT8 *)attribute + (*resident)->value_offset);
}

static int ntfs_loaded_directory_empty(NTFS_CONTEXT *context)
{
    static const UINT16 i30_name[] = { '$', 'I', '3', '0' };
    NTFS_RESIDENT *resident;
    NTFS_ATTR_HEADER *attribute =
        ntfs_find_mutable_resident_named_attribute(
            context, ATTR_INDEX_ROOT, i30_name, 4, &resident);
    NTFS_INDEX_ROOT *root;
    NTFS_INDEX_HEADER *header;
    NTFS_INDEX_ENTRY *entry;
    UINT8 *bitmap;
    UINT64 bitmap_size = 0;
    UINT64 index;
    if (attribute == 0 || resident->value_length <
        sizeof(NTFS_INDEX_ROOT) + sizeof(NTFS_INDEX_HEADER) +
        sizeof(NTFS_INDEX_ENTRY)) return 0;
    root = (NTFS_INDEX_ROOT *)((UINT8 *)attribute + resident->value_offset);
    header = (NTFS_INDEX_HEADER *)((UINT8 *)root + sizeof(NTFS_INDEX_ROOT));
    if (header->first_entry_offset + sizeof(NTFS_INDEX_ENTRY) >
        header->total_size) return 0;
    entry = (NTFS_INDEX_ENTRY *)((UINT8 *)header +
                                 header->first_entry_offset);
    if ((entry->flags & 0x02U) == 0) return 0;
    bitmap = load_index_bitmap(
        context, ((NTFS_FILE_RECORD *)context->mft_buf)->record_number,
        &bitmap_size);
    if (bitmap == 0) return 1;
    for (index = 0; index < bitmap_size; index++) {
        if (bitmap[index] != 0) {
            kfree(bitmap);
            return 0;
        }
    }
    kfree(bitmap);
    return 1;
}

static int ntfs_release_stream_clusters(NTFS_CONTEXT *context,
                                        const NTFS_STREAM *stream)
{
    UINT64 clusters[128];
    UINT32 count = 0;
    UINT32 run_index;
    for (run_index = 0; run_index < stream->run_count; run_index++) {
        UINT64 cluster;
        if (stream->runs[run_index].sparse) continue;
        for (cluster = 0; cluster < stream->runs[run_index].len; cluster++) {
            clusters[count++] = stream->runs[run_index].lcn + cluster;
            if (count == 128U) {
                if (!ntfs_free_clusters(context, count, clusters)) return 0;
                count = 0;
            }
        }
    }
    return count == 0 || ntfs_free_clusters(context, count, clusters);
}

static int ntfs_delete_entry(NTFS_CONTEXT *context, const char *path,
                             int directory)
{
    NTFS_TRANSACTION transaction;
    NTFS_FILE_RECORD *header;
    NTFS_RESIDENT *file_name_resident;
    NTFS_FILE_NAME *file_name;
    NTFS_STREAM *stream = 0;
    UINT64 record_number;
    UINT64 parent_record;
    UINT16 flags;
    if (context == 0 || !context->initialized || !context->write_allowed ||
        !ntfs_mutation_begin(context, &transaction, 0)) return 0;
    record_number = ntfs_resolve(context, path);
    if (record_number < 24U) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    header = (NTFS_FILE_RECORD *)context->mft_buf;
    if (((header->flags & 0x02U) != 0) != (directory != 0) ||
        header->hard_links != 1 ||
        (directory && !ntfs_loaded_directory_empty(context))) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    file_name = ntfs_loaded_primary_file_name(context, &file_name_resident);
    if (file_name == 0 || (file_name->file_flags &
        (0x00000001U | 0x00000400U | 0x00000800U | 0x00004000U)) != 0) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    parent_record = file_name->parent_ref & 0x0000FFFFFFFFFFFFULL;
    flags = header->flags;
    if (!directory) {
        stream = (NTFS_STREAM *)kmalloc(sizeof(*stream));
        if (stream == 0 ||
            !collect_attribute_stream(context, record_number, ATTR_DATA, 1,
                                      stream) ||
            (stream->flags & (0x0001U | 0x4000U)) != 0 ||
            (!stream->resident && !ntfs_release_stream_clusters(context,
                                                                 stream))) {
            if (stream != 0) kfree(stream);
            (void)ntfs_transaction_rollback(context, &transaction);
            return 0;
        }
        kfree(stream);
    }
    if (!ntfs_index_root_remove(context, parent_record, record_number) ||
        !read_mft_record(context, record_number)) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    header = (NTFS_FILE_RECORD *)context->mft_buf;
    header->flags = (UINT16)(flags & 0xFFFEU);
    header->sequence++;
    if (header->sequence == 0) header->sequence = 1;
    if (!write_mft_record(context, record_number, context->mft_buf) ||
        !ntfs_free_mft_records(context, 1, &record_number)) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    if (!ntfs_remount_verify_path(context, path, 0, 0, 0)) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    return ntfs_mutation_commit(context, &transaction);
}

int ntfs_context_delete_file(NTFS_CONTEXT *context, const char *path)
{
    return ntfs_delete_entry(context, path, 0);
}

int ntfs_context_delete_directory(NTFS_CONTEXT *context, const char *path)
{
    return ntfs_delete_entry(context, path, 1);
}

static int ntfs_move_would_cycle(NTFS_CONTEXT *context, UINT64 source_record,
                                 UINT64 destination_parent)
{
    UINT32 depth;
    UINT64 current = destination_parent;
    for (depth = 0; depth < 1024U; depth++) {
        NTFS_RESIDENT *resident;
        NTFS_FILE_NAME *file_name;
        if (current == source_record) return 1;
        if (current == MFT_RECORD_ROOT) return 0;
        if (!read_mft_record(context, current)) return 1;
        file_name = ntfs_loaded_primary_file_name(context, &resident);
        if (file_name == 0) return 1;
        current = file_name->parent_ref & 0x0000FFFFFFFFFFFFULL;
    }
    return 1;
}

int ntfs_context_rename(NTFS_CONTEXT *context, const char *source,
                        const char *destination)
{
    NTFS_TRANSACTION transaction;
    char destination_parent_path[512];
    const char *destination_name_text;
    UINT16 destination_name[255];
    UINT32 destination_name_length;
    UINT64 source_record;
    UINT64 old_parent;
    UINT64 new_parent;
    UINT16 new_parent_sequence;
    UINT16 source_sequence;
    UINT16 hard_links;
    UINT16 source_flags;
    NTFS_RESIDENT *resident;
    NTFS_ATTR_HEADER *attribute;
    NTFS_FILE_NAME old_file_name;
    UINT32 new_value_length;
    UINT32 old_attribute_length;
    UINT32 new_attribute_length;
    UINT8 file_name_value[sizeof(NTFS_FILE_NAME) + 510U];
    if (context == 0 || !context->initialized || !context->write_allowed ||
        !ntfs_split_parent_path(destination, destination_parent_path,
                                sizeof(destination_parent_path),
                                &destination_name_text) ||
        !utf8_to_utf16(destination_name_text, destination_name, 255,
                       &destination_name_length) ||
        ntfs_resolve(context, destination) != 0 ||
        !ntfs_mutation_begin(context, &transaction, 0)) return 0;
    source_record = ntfs_resolve(context, source);
    if (source_record < 24U) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    source_sequence = ((NTFS_FILE_RECORD *)context->mft_buf)->sequence;
    hard_links = ((NTFS_FILE_RECORD *)context->mft_buf)->hard_links;
    source_flags = ((NTFS_FILE_RECORD *)context->mft_buf)->flags;
    attribute = ntfs_find_mutable_resident_attribute(context, ATTR_FILE_NAME,
                                                      &resident);
    if (attribute == 0 || resident->value_length < sizeof(NTFS_FILE_NAME) ||
        hard_links != 1) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    ntfs_copy(&old_file_name, (UINT8 *)attribute + resident->value_offset,
              sizeof(old_file_name));
    if ((old_file_name.file_flags &
        (0x00000001U | 0x00000400U | 0x00000800U | 0x00004000U)) != 0) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    old_parent = old_file_name.parent_ref & 0x0000FFFFFFFFFFFFULL;
    new_parent = ntfs_resolve(context, destination_parent_path);
    if (new_parent == 0 ||
        (((NTFS_FILE_RECORD *)context->mft_buf)->flags & 0x02U) == 0 ||
        ((source_flags & 0x02U) != 0 &&
         ntfs_move_would_cycle(context, source_record, new_parent))) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    if (!read_mft_record(context, new_parent)) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    new_parent_sequence = ((NTFS_FILE_RECORD *)context->mft_buf)->sequence;
    if (!read_mft_record(context, source_record)) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    attribute = ntfs_find_mutable_resident_attribute(context, ATTR_FILE_NAME,
                                                      &resident);
    if (attribute == 0) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    old_attribute_length = attribute->length;
    new_value_length = sizeof(NTFS_FILE_NAME) + destination_name_length * 2U;
    new_attribute_length = ntfs_align8(resident->value_offset +
                                       new_value_length);
    if (!ntfs_resize_resident_attribute(
            context, attribute,
            (INT64)new_attribute_length - old_attribute_length)) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    attribute = ntfs_find_mutable_resident_attribute(context, ATTR_FILE_NAME,
                                                      &resident);
    if (attribute == 0) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    old_file_name.parent_ref = new_parent |
        ((UINT64)new_parent_sequence << 48);
    old_file_name.name_length = (UINT8)destination_name_length;
    resident->value_length = new_value_length;
    ntfs_zero((UINT8 *)attribute + resident->value_offset,
              attribute->length - resident->value_offset);
    ntfs_copy((UINT8 *)attribute + resident->value_offset, &old_file_name,
              sizeof(old_file_name));
    ntfs_copy((UINT8 *)attribute + resident->value_offset +
              sizeof(NTFS_FILE_NAME), destination_name,
              destination_name_length * 2U);
    ntfs_copy(file_name_value,
              (UINT8 *)attribute + resident->value_offset,
              new_value_length);
    if (!write_mft_record(context, source_record, context->mft_buf) ||
        !ntfs_index_root_remove(context, old_parent, source_record) ||
        !ntfs_index_root_insert(context, new_parent,
                                source_record |
                                ((UINT64)source_sequence << 48),
                                file_name_value,
                                (UINT16)new_value_length)) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    if (!ntfs_remount_verify_path(context, source, 0, 0, 0) ||
        !ntfs_remount_verify_path(context, destination, 1, 0, 0)) {
        (void)ntfs_transaction_rollback(context, &transaction);
        return 0;
    }
    return ntfs_mutation_commit(context, &transaction);
}

UINT64 ntfs_context_list_directory(NTFS_CONTEXT *context, const char *path,
                                   NTFS_FILE_INFO *entries, UINT64 capacity)
{
    FIND_ATTR_CTX ctx;
    const NTFS_FILE_RECORD *hdr;
    UINT64 record_number;

    if (context == 0 || !context->initialized ||
        (record_number = ntfs_resolve(context, path)) == 0) return 0;

    hdr = (const NTFS_FILE_RECORD *)context->mft_buf;
    if (!(hdr->flags & 0x02)) return 0; /* not a directory */

    ctx.target_type = ATTR_INDEX_ROOT;
    ctx.found       = 0;
    iterate_attrs(context->mft_buf, find_attr_cb, &ctx);
    if (!ctx.found || ctx.non_resident) return 0;

    {
        UINT64 count = list_index_root(ctx.value, ctx.value_len,
                                       entries, capacity);
        if (count < capacity)
            count += list_index_allocation(context, record_number,
                                           entries + count,
                                           capacity - count);
        return count;
    }
}

int ntfs_detect(void) { return ntfs_detect_context(&legacy_context); }
int ntfs_initialize(void) { return ntfs_initialize_context(&legacy_context); }
UINT64 ntfs_file_size(const char *path) { return ntfs_context_file_size(&legacy_context, path); }
int ntfs_exists(const char *path) { return ntfs_context_exists(&legacy_context, path); }
int ntfs_is_directory(const char *path) { return ntfs_context_is_directory(&legacy_context, path); }
UINT64 ntfs_read_file(const char *path, void *buffer, UINT64 capacity)
{
    return ntfs_context_read_file(&legacy_context, path, buffer, capacity);
}
UINT64 ntfs_list_directory(const char *path, NTFS_FILE_INFO *entries, UINT64 capacity)
{
    return ntfs_context_list_directory(&legacy_context, path, entries, capacity);
}

/* ---- Multi-device mount ---- */
static UINT8 ntfs_mounted_target = 0xFFU;
static UINT8 ntfs_mounted_lun    = 0xFFU;

int ntfs_mount_device(UINT8 target, UINT8 lun)
{
    legacy_context.device = 0;
    if (target == ntfs_mounted_target && lun == ntfs_mounted_lun) {
        return legacy_context.initialized;
    }
    virtio_block_select_device(target, lun);
    if (!ntfs_initialize()) {
        return 0;
    }
    ntfs_mounted_target = target;
    ntfs_mounted_lun    = lun;
    return 1;
}

NTFS_CONTEXT *ntfs_context_create(ASAS_BLOCK_DEVICE *device)
{
    NTFS_CONTEXT *context;
    UINT8 *bytes;
    UINT32 index;
    if (device == 0 || (device->logical_block_size != 512 &&
                        device->logical_block_size != 1024 &&
                        device->logical_block_size != 2048 &&
                        device->logical_block_size != 4096)) return 0;
    context = (NTFS_CONTEXT *)kmalloc(sizeof(NTFS_CONTEXT));
    if (context == 0) return 0;
    bytes = (UINT8 *)context;
    for (index = 0; index < sizeof(NTFS_CONTEXT); index++) bytes[index] = 0;
    context->device = device;
    if (!ntfs_initialize_context(context)) {
        kfree(context);
        return 0;
    }
    return context;
}

void ntfs_context_destroy(NTFS_CONTEXT *context)
{
    if (context == 0 || context == &legacy_context) return;
    if (context->upcase != 0) kfree(context->upcase);
    kfree(context);
}

int ntfs_context_is_writable(const NTFS_CONTEXT *context)
{
    return context != 0 && context->initialized && context->write_allowed;
}

NTFS_READ_ONLY_REASON ntfs_context_read_only_reason(const NTFS_CONTEXT *context)
{
    if (context == 0 || !context->initialized)
        return NTFS_READ_ONLY_REASON_VOLUME_INFO;
    return context->write_allowed ? NTFS_READ_ONLY_REASON_NONE :
        context->read_only_reason;
}

const char *ntfs_context_read_only_reason_string(const NTFS_CONTEXT *context)
{
    switch (ntfs_context_read_only_reason(context)) {
    case NTFS_READ_ONLY_REASON_NONE:
        return "full read-write";
    case NTFS_READ_ONLY_REASON_DEVICE:
        return "temporary read-only: device is write-protected";
    case NTFS_READ_ONLY_REASON_UPCASE:
        return "temporary read-only: NTFS upcase table unavailable";
    case NTFS_READ_ONLY_REASON_VOLUME_INFO:
        return "temporary read-only: NTFS volume information unavailable";
    case NTFS_READ_ONLY_REASON_DIRTY_LOG_REPLAY_REQUIRED:
        return "temporary read-only: dirty NTFS requires Windows LogFile replay";
    case NTFS_READ_ONLY_REASON_UNSUPPORTED_VOLUME_FLAGS:
        return "temporary read-only: unsupported NTFS volume flags";
    default:
        return "temporary read-only: unknown NTFS safety gate";
    }
}

const char *ntfs_context_last_error_string(const NTFS_CONTEXT *context)
{
    (void)context;
    return ntfs_last_error;
}

int ntfs_self_test(void)
{
    UINT8 record[1024];
    UINT8 index_record[1024];
    UINT8 sector_record[4096];
    UINT8 extension_record[1024];
    UINT8 attribute_list[160];
    const UINT8 writable_runlist[] = { 0x11, 0x02, 0x05, 0x00 };
    const UINT8 sparse_runlist[] = { 0x01, 0x02, 0x00 };
    const UINT8 extent_one_runlist[] = { 0x11, 0x02, 0x05, 0x00 };
    const UINT8 extent_two_runlist[] = { 0x11, 0x03, 0x09, 0x00 };
    const UINT8 lznt1_raw[] = { 0x02, 0x30, 'A', 'B', 'C' };
    const UINT8 lznt1_literals[] = { 0x03, 0xB0, 0x00, 'A', 'B', 'C' };
    const UINT8 lznt1_phrase[] = {
        0x05, 0xB0, 0x08, 'A', 'B', 'C', 0x00, 0x20
    };
    const UINT8 lznt1_invalid[] = { 0x02, 0xB0, 0x01, 0x00, 0x00 };
    UINT8 decompressed[16];
    UINT32 decompressed_size;
    NTFS_ATTR_VIEW extent;
    NTFS_CONTEXT transaction_context;
    NTFS_TRANSACTION transaction;
    ASAS_BLOCK_DEVICE transaction_device = { 0 };
    UINT8 sector_before[512];
    UINT8 sector_after[512];
    UINT8 bitmap[2] = { 0xEFU, 0xFFU };
    UINT64 bitmap_results[2];
    UINT16 test_name[] = { 'T', 'e', 's', 't' };
    FIND_ATTR_CTX built_attribute = { 0 };
    NTFS_ATTR_VIEW built_extent;
    UINT32 attribute_list_size;
    NTFS_RUN runs[2];
    NTFS_FILE_RECORD *header = (NTFS_FILE_RECORD *)record;
    UINT16 *usa;
    UINT32 index;
    for (index = 0; index < sizeof(record); index++) record[index] = 0;
    record[0] = 'F'; record[1] = 'I'; record[2] = 'L'; record[3] = 'E';
    header->fixup_offset = sizeof(NTFS_FILE_RECORD);
    header->fixup_count = 3;
    usa = (UINT16 *)(record + header->fixup_offset);
    usa[0] = 0xA55AU;
    usa[1] = 0x1111U;
    usa[2] = 0x2222U;
    *(UINT16 *)(record + 510) = usa[0];
    *(UINT16 *)(record + 1022) = usa[0];
    if (!apply_fixup(record, sizeof(record), 512) ||
        *(UINT16 *)(record + 510) != usa[1] ||
        *(UINT16 *)(record + 1022) != usa[2]) return 0;

    if (!protect_fixup(record, sizeof(record), 512) ||
        *(UINT16 *)(record + 510) != usa[0] ||
        *(UINT16 *)(record + 1022) != usa[0] ||
        !apply_fixup(record, sizeof(record), 512) ||
        *(UINT16 *)(record + 510) != usa[1] ||
        *(UINT16 *)(record + 1022) != usa[2]) return 0;

    ntfs_zero(index_record, sizeof(index_record));
    index_record[0] = 'I'; index_record[1] = 'N';
    index_record[2] = 'D'; index_record[3] = 'X';
    ((NTFS_INDEX_BLOCK *)index_record)->fixup_offset = 40;
    ((NTFS_INDEX_BLOCK *)index_record)->fixup_count = 3;
    if (!protect_fixup(index_record, sizeof(index_record), 512) ||
        !apply_fixup(index_record, sizeof(index_record), 512)) return 0;

    for (index = 1024; index <= 2048; index *= 2U) {
        NTFS_FILE_RECORD *sector_header;
        ntfs_zero(sector_record, sizeof(sector_record));
        sector_header = (NTFS_FILE_RECORD *)sector_record;
        sector_record[0] = 'F'; sector_record[1] = 'I';
        sector_record[2] = 'L'; sector_record[3] = 'E';
        sector_header->fixup_offset = sizeof(NTFS_FILE_RECORD);
        sector_header->fixup_count = 3;
        if (!protect_fixup(sector_record, index * 2U, index) ||
            !apply_fixup(sector_record, index * 2U, index)) return 0;
    }

    for (index = 0; index < sizeof(ntfs_test_4k_record); index++)
        ntfs_test_4k_record[index] = 0;
    header = (NTFS_FILE_RECORD *)ntfs_test_4k_record;
    ntfs_test_4k_record[0] = 'F'; ntfs_test_4k_record[1] = 'I';
    ntfs_test_4k_record[2] = 'L'; ntfs_test_4k_record[3] = 'E';
    header->fixup_offset = sizeof(NTFS_FILE_RECORD);
    header->fixup_count = 2;
    usa = (UINT16 *)(ntfs_test_4k_record + header->fixup_offset);
    usa[0] = 0x4B4EU;
    usa[1] = 0x1357U;
    *(UINT16 *)(ntfs_test_4k_record + 4094) = usa[0];
    if (!apply_fixup(ntfs_test_4k_record, sizeof(ntfs_test_4k_record), 4096) ||
        *(UINT16 *)(ntfs_test_4k_record + 4094) != usa[1] ||
        !protect_fixup(ntfs_test_4k_record, sizeof(ntfs_test_4k_record), 4096) ||
        !apply_fixup(ntfs_test_4k_record, sizeof(ntfs_test_4k_record), 4096) ||
        *(UINT16 *)(ntfs_test_4k_record + 4094) != usa[1]) return 0;

    header = (NTFS_FILE_RECORD *)record;
    usa = (UINT16 *)(record + sizeof(NTFS_FILE_RECORD));
    *(UINT16 *)(record + 510) = usa[0];
    *(UINT16 *)(record + 1022) = 0xBAD0U;
    if (apply_fixup(record, sizeof(record), 512) ||
        *(UINT16 *)(record + 510) != usa[0]) return 0;

    *(UINT16 *)(record + 1022) = usa[0];
    header->fixup_offset = 1020;
    header->fixup_count = 3;
    if (apply_fixup(record, sizeof(record), 512) ||
        decode_runlist(sparse_runlist, sizeof(sparse_runlist),
                       runs, 2) != 1 || !runs[0].sparse ||
        runs[0].len != 2 ||
        decode_writable_runlist(writable_runlist,
                                sizeof(writable_runlist), runs, 2) != 1 ||
        runs[0].lcn != 5 || runs[0].len != 2 ||
        decode_writable_runlist(sparse_runlist,
                                sizeof(sparse_runlist), runs, 2) != 0)
        return 0;

    ntfs_zero(&ntfs_test_stream, sizeof(ntfs_test_stream));
    ntfs_zero(&extent, sizeof(extent));
    extent.found = 1;
    extent.non_resident = 1;
    extent.lowest_vcn = 0;
    extent.highest_vcn = 1;
    extent.flags = 1;
    extent.compression_unit = 4;
    extent.data_size = 5U * 4096U;
    extent.runlist = extent_one_runlist;
    extent.runlist_length = sizeof(extent_one_runlist);
    if (!append_stream_extent(&ntfs_test_stream, &extent)) return 0;
    extent.lowest_vcn = 2;
    extent.highest_vcn = 4;
    extent.runlist = extent_two_runlist;
    extent.runlist_length = sizeof(extent_two_runlist);
    if (!append_stream_extent(&ntfs_test_stream, &extent) ||
        ntfs_test_stream.run_count != 2 ||
        ntfs_test_stream.next_vcn != 5 ||
        ntfs_test_stream.runs[1].lcn != 9) return 0;
    extent.lowest_vcn = 6;
    extent.highest_vcn = 8;
    if (append_stream_extent(&ntfs_test_stream, &extent)) return 0;

    ntfs_zero(decompressed, sizeof(decompressed));
    if (!lznt1_decompress(lznt1_raw, sizeof(lznt1_raw), decompressed,
                          sizeof(decompressed), &decompressed_size) ||
        decompressed_size != 3 || decompressed[0] != 'A' ||
        decompressed[1] != 'B' || decompressed[2] != 'C') return 0;
    ntfs_zero(decompressed, sizeof(decompressed));
    if (!lznt1_decompress(lznt1_literals, sizeof(lznt1_literals),
                          decompressed, sizeof(decompressed),
                          &decompressed_size) || decompressed_size != 3 ||
        decompressed[0] != 'A' || decompressed[1] != 'B' ||
        decompressed[2] != 'C') return 0;
    ntfs_zero(decompressed, sizeof(decompressed));
    if (!lznt1_decompress(lznt1_phrase, sizeof(lznt1_phrase), decompressed,
                          sizeof(decompressed), &decompressed_size) ||
        decompressed_size != 6 || decompressed[0] != 'A' ||
        decompressed[3] != 'A' || decompressed[4] != 'B' ||
        decompressed[5] != 'C' ||
        lznt1_decompress(lznt1_invalid, sizeof(lznt1_invalid), decompressed,
                         sizeof(decompressed), &decompressed_size)) return 0;

    ntfs_zero(&transaction_context, sizeof(transaction_context));
    ntfs_zero(ntfs_test_disk, sizeof(ntfs_test_disk));
    for (index = 0; index < sizeof(sector_before); index++) {
        sector_before[index] = (UINT8)(index ^ 0x5AU);
        sector_after[index] = (UINT8)(index ^ 0xA5U);
        ntfs_test_disk[index] = sector_before[index];
    }
    transaction_device.logical_block_size = 512;
    transaction_device.physical_block_size = 512;
    transaction_device.block_count = 8;
    transaction_device.ops = &ntfs_test_device_ops;
    transaction_context.device = &transaction_device;
    transaction_context.bytes_per_sector = 512;
    transaction_context.write_allowed = 1;
    if (!ntfs_transaction_begin(&transaction_context, &transaction, 1) ||
        !ntfs_write_sector(&transaction_context, 0, sector_after) ||
        ntfs_transaction_commit(&transaction_context, &transaction)) return 0;
    for (index = 0; index < sizeof(sector_before); index++)
        if (ntfs_test_disk[index] != sector_before[index]) return 0;
    if (!ntfs_transaction_begin(&transaction_context, &transaction, 0) ||
        !ntfs_write_sector(&transaction_context, 0, sector_after) ||
        !ntfs_transaction_commit(&transaction_context, &transaction)) return 0;
    for (index = 0; index < sizeof(sector_after); index++)
        if (ntfs_test_disk[index] != sector_after[index]) return 0;

    if (!ntfs_bitmap_find_clear(bitmap, 16, 0, bitmap_results, 1) ||
        bitmap_results[0] != 4 ||
        ntfs_bitmap_find_clear(bitmap, 16, 0, bitmap_results, 2)) return 0;
    ntfs_bitmap_set(bitmap, bitmap_results[0], 1);
    if ((bitmap[0] & 0x10U) == 0) return 0;
    ntfs_bitmap_set(bitmap, bitmap_results[0], 0);
    if ((bitmap[0] & 0x10U) != 0) return 0;

    transaction_context.mft_record_size = sizeof(record);
    transaction_context.bytes_per_sector = 512;
    if (!ntfs_build_basic_record(&transaction_context, record, 24, 7,
                                 MFT_RECORD_ROOT, test_name, 4, 0,
                                 "data", 4)) return 0;
    built_attribute.target_type = ATTR_DATA;
    iterate_attrs(record, find_attr_cb, &built_attribute);
    if (!built_attribute.found || built_attribute.non_resident ||
        built_attribute.value_len != 4 || built_attribute.value[0] != 'd' ||
        ((NTFS_FILE_RECORD *)record)->record_number != 24 ||
        ((NTFS_FILE_RECORD *)record)->sequence != 7 ||
        !protect_fixup(record, sizeof(record), 512) ||
        !apply_fixup(record, sizeof(record), 512)) return 0;

    transaction_context.mft_record_size = sizeof(extension_record);
    if (!ntfs_initialize_file_record(&transaction_context, extension_record,
                                     25, 3, 0)) return 0;
    ((NTFS_FILE_RECORD *)extension_record)->base_record =
        24U | (7ULL << 48);
    if (!ntfs_append_nonresident_extent(
            extension_record, sizeof(extension_record), ATTR_DATA, 0, 0,
            extent_two_runlist, sizeof(extent_two_runlist), 2, 3,
            5U * 4096U, 5U * 4096U, 5U * 4096U, 0, 0, 0)) return 0;
    ntfs_copy(transaction_context.mft_buf, extension_record,
              sizeof(extension_record));
    if (!find_loaded_attribute(&transaction_context, ATTR_DATA, 0, 2, 1, 1,
                               &built_extent) ||
        built_extent.lowest_vcn != 2 || built_extent.highest_vcn != 4)
        return 0;
    attribute_list_size = ntfs_attribute_list_add(
        attribute_list, sizeof(attribute_list), 0, ATTR_STANDARD_INFORMATION,
        0, 24U | (7ULL << 48), 0);
    attribute_list_size = ntfs_attribute_list_add(
        attribute_list, sizeof(attribute_list), attribute_list_size,
        ATTR_ATTRIBUTE_LIST, 0, 24U | (7ULL << 48), 2);
    attribute_list_size = ntfs_attribute_list_add(
        attribute_list, sizeof(attribute_list), attribute_list_size,
        ATTR_FILE_NAME, 0, 24U | (7ULL << 48), 1);
    attribute_list_size = ntfs_attribute_list_add(
        attribute_list, sizeof(attribute_list), attribute_list_size,
        ATTR_DATA, 2, 25U | (3ULL << 48), 0);
    if (attribute_list_size == 0 ||
        ((NTFS_ATTRIBUTE_LIST_ENTRY *)(attribute_list +
          3U * ntfs_align8(sizeof(NTFS_ATTRIBUTE_LIST_ENTRY))))->lowest_vcn != 2)
        return 0;
    return 1;
}
