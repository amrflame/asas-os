#ifndef ASAS_FAT32_H
#define ASAS_FAT32_H

#include "uefi.h"
#include "block_device.h"

typedef struct FAT32_CONTEXT FAT32_CONTEXT;

typedef struct {
    char   name[256];
    UINT64 size;
    UINT8  is_directory;
    UINT8  read_only;
} FAT32_FILE_INFO;

int    fat32_detect(void);
int    fat32_initialize(void);
UINT64 fat32_file_size(const char *path);
int    fat32_is_directory(const char *path);
int    fat32_exists(const char *path);
UINT64 fat32_read_file(const char *path, void *buffer, UINT64 capacity);
UINT64 fat32_list_directory(const char *path, FAT32_FILE_INFO *entries, UINT64 capacity);
int    fat32_write_file(const char *path, const void *buffer, UINT64 size);
int    fat32_delete_file(const char *path);
int    fat32_create_directory(const char *path);
int    fat32_delete_directory(const char *path);
int    fat32_mount_device(UINT8 target, UINT8 lun);
FAT32_CONTEXT *fat32_context_create(ASAS_BLOCK_DEVICE *device);
void           fat32_context_destroy(FAT32_CONTEXT *context);
UINT64         fat32_context_file_size(FAT32_CONTEXT *context, const char *path);
int            fat32_context_exists(FAT32_CONTEXT *context, const char *path);
int            fat32_context_is_directory(FAT32_CONTEXT *context, const char *path);
UINT64         fat32_context_read_file(FAT32_CONTEXT *context, const char *path,
                                       void *buffer, UINT64 capacity);
UINT64         fat32_context_list_directory(FAT32_CONTEXT *context, const char *path,
                                            FAT32_FILE_INFO *entries, UINT64 capacity);
int            fat32_context_write_file(FAT32_CONTEXT *context, const char *path,
                                        const void *buffer, UINT64 size);
int            fat32_context_delete_file(FAT32_CONTEXT *context, const char *path);
int            fat32_context_create_directory(FAT32_CONTEXT *context, const char *path);
int            fat32_context_delete_directory(FAT32_CONTEXT *context, const char *path);
int            fat32_context_rename(FAT32_CONTEXT *context, const char *source,
                                    const char *destination);
int            fat32_context_sync(FAT32_CONTEXT *context);
const char    *fat32_context_last_error_string(FAT32_CONTEXT *context);

#endif
