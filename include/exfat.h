#ifndef ASAS_EXFAT_H
#define ASAS_EXFAT_H

#include "uefi.h"
#include "block_device.h"

typedef struct EXFAT_CONTEXT EXFAT_CONTEXT;

typedef struct {
    char name[256];
    UINT64 size;
    UINT8 is_directory;
    UINT8 read_only;
} EXFAT_FILE_INFO;

int exfat_probe(ASAS_BLOCK_DEVICE *device);
EXFAT_CONTEXT *exfat_context_create(ASAS_BLOCK_DEVICE *device);
void exfat_context_destroy(EXFAT_CONTEXT *context);
int exfat_context_exists(EXFAT_CONTEXT *context, const char *path);
int exfat_context_is_directory(EXFAT_CONTEXT *context, const char *path);
UINT64 exfat_context_file_size(EXFAT_CONTEXT *context, const char *path);
UINT64 exfat_context_read_file(EXFAT_CONTEXT *context, const char *path,
                               void *buffer, UINT64 capacity);
UINT64 exfat_context_list_directory(EXFAT_CONTEXT *context, const char *path,
                                    EXFAT_FILE_INFO *entries, UINT64 capacity);
int exfat_context_write_file(EXFAT_CONTEXT *context, const char *path,
                             const void *buffer, UINT64 size);
int exfat_context_delete_file(EXFAT_CONTEXT *context, const char *path);
int exfat_context_create_directory(EXFAT_CONTEXT *context, const char *path);
int exfat_context_delete_directory(EXFAT_CONTEXT *context, const char *path);
int exfat_context_rename(EXFAT_CONTEXT *context, const char *source,
                         const char *destination);
int exfat_context_sync(EXFAT_CONTEXT *context);
int exfat_self_test(void);

#endif
