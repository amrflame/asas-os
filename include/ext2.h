#ifndef ASAS_EXT2_H
#define ASAS_EXT2_H

#include "uefi.h"
#include "block_device.h"

typedef struct EXT2_CONTEXT EXT2_CONTEXT;

typedef struct {
    char name[256];
    UINT64 size;
    UINT8 is_directory;
    UINT8 read_only;
} EXT2_FILE_INFO;

int ext2_probe(ASAS_BLOCK_DEVICE *device);
EXT2_CONTEXT *ext2_context_create(ASAS_BLOCK_DEVICE *device);
void ext2_context_destroy(EXT2_CONTEXT *context);
int ext2_context_exists(EXT2_CONTEXT *context, const char *path);
int ext2_context_is_directory(EXT2_CONTEXT *context, const char *path);
int ext2_context_is_read_only(EXT2_CONTEXT *context);
UINT64 ext2_context_file_size(EXT2_CONTEXT *context, const char *path);
UINT64 ext2_context_read_file(EXT2_CONTEXT *context, const char *path,
                              void *buffer, UINT64 capacity);
UINT64 ext2_context_list_directory(EXT2_CONTEXT *context, const char *path,
                                   EXT2_FILE_INFO *entries, UINT64 capacity);
int ext2_context_write_file(EXT2_CONTEXT *context, const char *path,
                            const void *buffer, UINT64 size);
int ext2_context_delete_file(EXT2_CONTEXT *context, const char *path);
int ext2_context_create_directory(EXT2_CONTEXT *context, const char *path);
int ext2_context_delete_directory(EXT2_CONTEXT *context, const char *path);
int ext2_context_rename(EXT2_CONTEXT *context, const char *source,
                        const char *destination);
int ext2_context_sync(EXT2_CONTEXT *context);

#endif
