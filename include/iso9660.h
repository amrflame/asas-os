#ifndef ASAS_ISO9660_H
#define ASAS_ISO9660_H

#include "uefi.h"
#include "block_device.h"

typedef struct ISO9660_CONTEXT ISO9660_CONTEXT;

typedef struct {
    char name[256];
    UINT64 size;
    UINT8 is_directory;
    UINT8 read_only;
} ISO9660_FILE_INFO;

int iso9660_probe(ASAS_BLOCK_DEVICE *device);
ISO9660_CONTEXT *iso9660_context_create(ASAS_BLOCK_DEVICE *device);
void iso9660_context_destroy(ISO9660_CONTEXT *context);
int iso9660_context_exists(ISO9660_CONTEXT *context, const char *path);
int iso9660_context_is_directory(ISO9660_CONTEXT *context, const char *path);
int iso9660_context_is_read_only(ISO9660_CONTEXT *context, const char *path);
UINT64 iso9660_context_file_size(ISO9660_CONTEXT *context, const char *path);
UINT64 iso9660_context_read_file(ISO9660_CONTEXT *context, const char *path,
                                 void *buffer, UINT64 capacity);
UINT64 iso9660_context_list_directory(ISO9660_CONTEXT *context, const char *path,
                                      ISO9660_FILE_INFO *entries,
                                      UINT64 capacity);
int iso9660_context_write_file(ISO9660_CONTEXT *context, const char *path,
                               const void *buffer, UINT64 size);
int iso9660_context_delete_file(ISO9660_CONTEXT *context, const char *path);
int iso9660_context_create_directory(ISO9660_CONTEXT *context, const char *path);
int iso9660_context_delete_directory(ISO9660_CONTEXT *context, const char *path);
int iso9660_context_rename(ISO9660_CONTEXT *context, const char *source,
                           const char *destination);
int iso9660_context_sync(ISO9660_CONTEXT *context);

#endif
