#ifndef ASAS_NTFS_H
#define ASAS_NTFS_H

#include "uefi.h"
#include "block_device.h"

typedef struct NTFS_CONTEXT NTFS_CONTEXT;

typedef enum {
    NTFS_READ_ONLY_REASON_NONE = 0,
    NTFS_READ_ONLY_REASON_DEVICE,
    NTFS_READ_ONLY_REASON_UPCASE,
    NTFS_READ_ONLY_REASON_VOLUME_INFO,
    NTFS_READ_ONLY_REASON_DIRTY_LOG_REPLAY_REQUIRED,
    NTFS_READ_ONLY_REASON_UNSUPPORTED_VOLUME_FLAGS
} NTFS_READ_ONLY_REASON;

typedef struct {
    char   name[256];
    UINT64 size;
    UINT8  is_directory;
    UINT8  read_only;
    UINT8  reparse_point;
    UINT8  compressed;
    UINT8  sparse;
    UINT8  encrypted;
} NTFS_FILE_INFO;

int    ntfs_detect(void);
int    ntfs_initialize(void);
UINT64 ntfs_file_size(const char *path);
int    ntfs_is_directory(const char *path);
int    ntfs_exists(const char *path);
UINT64 ntfs_read_file(const char *path, void *buffer, UINT64 capacity);
UINT64 ntfs_list_directory(const char *path, NTFS_FILE_INFO *entries, UINT64 capacity);
int    ntfs_mount_device(UINT8 target, UINT8 lun);
NTFS_CONTEXT *ntfs_context_create(ASAS_BLOCK_DEVICE *device);
void          ntfs_context_destroy(NTFS_CONTEXT *context);
int           ntfs_context_is_writable(const NTFS_CONTEXT *context);
NTFS_READ_ONLY_REASON ntfs_context_read_only_reason(const NTFS_CONTEXT *context);
const char   *ntfs_context_read_only_reason_string(const NTFS_CONTEXT *context);
const char   *ntfs_context_last_error_string(const NTFS_CONTEXT *context);
int           ntfs_self_test(void);
UINT64        ntfs_context_file_size(NTFS_CONTEXT *context, const char *path);
int           ntfs_context_exists(NTFS_CONTEXT *context, const char *path);
int           ntfs_context_is_directory(NTFS_CONTEXT *context, const char *path);
UINT64        ntfs_context_read_file(NTFS_CONTEXT *context, const char *path,
                                     void *buffer, UINT64 capacity);
int           ntfs_context_write_file(NTFS_CONTEXT *context, const char *path,
                                      const void *buffer, UINT64 size);
int           ntfs_context_delete_file(NTFS_CONTEXT *context, const char *path);
int           ntfs_context_create_directory(NTFS_CONTEXT *context,
                                            const char *path);
int           ntfs_context_delete_directory(NTFS_CONTEXT *context,
                                            const char *path);
int           ntfs_context_rename(NTFS_CONTEXT *context, const char *source,
                                  const char *destination);
UINT64        ntfs_context_list_directory(NTFS_CONTEXT *context, const char *path,
                                          NTFS_FILE_INFO *entries, UINT64 capacity);

#endif
