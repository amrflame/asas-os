#ifndef ASAS_FAT16_H
#define ASAS_FAT16_H

#include "uefi.h"

typedef struct {
    char name[13];
    UINT64 size;
    UINT8 is_directory;
    UINT8 read_only;
} FAT16_FILE_INFO;

int fat16_initialize(void);
UINT64 fat16_root_file_size(const char name[11]);
UINT64 fat16_read_root_file(const char name[11], void *buffer, UINT64 capacity);
UINT64 fat16_list_root(FAT16_FILE_INFO *entries, UINT64 capacity);
UINT64 fat16_file_size(const char *path);
int fat16_is_directory(const char *path);
UINT64 fat16_read_file(const char *path, void *buffer, UINT64 capacity);
UINT64 fat16_list_directory(const char *path, FAT16_FILE_INFO *entries, UINT64 capacity);
int fat16_write_root_file(const char *path, const void *buffer, UINT64 size);
int fat16_delete_root_file(const char *path);
int fat16_create_root_directory(const char *path);
int fat16_delete_root_directory(const char *path);
int fat16_self_test(void);
int fat16_mount_device(UINT8 target, UINT8 lun);

#endif
