#ifndef ASAS_UDF_H
#define ASAS_UDF_H

#include "uefi.h"
#include "block_device.h"

typedef struct UDF_CONTEXT UDF_CONTEXT;

typedef struct {
    char name[256];
    UINT64 size;
    UINT8 is_directory;
    UINT8 read_only;
} UDF_FILE_INFO;

int udf_probe(ASAS_BLOCK_DEVICE *device);
UDF_CONTEXT *udf_context_create(ASAS_BLOCK_DEVICE *device);
void udf_context_destroy(UDF_CONTEXT *context);
int udf_context_exists(UDF_CONTEXT *context, const char *path);
int udf_context_is_directory(UDF_CONTEXT *context, const char *path);
UINT64 udf_context_file_size(UDF_CONTEXT *context, const char *path);
UINT64 udf_context_read_file(UDF_CONTEXT *context, const char *path,
                             void *buffer, UINT64 capacity);
UINT64 udf_context_list_directory(UDF_CONTEXT *context, const char *path,
                                  UDF_FILE_INFO *entries, UINT64 capacity);
int udf_context_write_file(UDF_CONTEXT *context, const char *path,
                           const void *buffer, UINT64 size);
int udf_context_delete_file(UDF_CONTEXT *context, const char *path);
int udf_context_create_directory(UDF_CONTEXT *context, const char *path);
int udf_context_delete_directory(UDF_CONTEXT *context, const char *path);
int udf_context_rename(UDF_CONTEXT *context, const char *source,
                       const char *destination);
int udf_context_sync(UDF_CONTEXT *context);

#endif
