#ifndef ASAS_FILESYSTEM_H
#define ASAS_FILESYSTEM_H

#include "block_device.h"

#define FILESYSTEM_MAX_DRIVERS 8
#define FILESYSTEM_MAX_MOUNTS 16
#define FILESYSTEM_NAME_CAPACITY 16

#define FILESYSTEM_FLAG_READ_ONLY 0x01U
#define FILESYSTEM_FLAG_NO_EXEC   0x02U

typedef struct {
    UINT64 size;
    UINT8 exists;
    UINT8 is_directory;
    UINT8 read_only;
} ASAS_FILE_STAT;

typedef struct {
    char name[256];
    UINT64 size;
    UINT8 is_directory;
    UINT8 read_only;
} ASAS_FILESYSTEM_ENTRY;

typedef struct ASAS_FILESYSTEM_DRIVER ASAS_FILESYSTEM_DRIVER;
typedef struct ASAS_FILESYSTEM_MOUNT ASAS_FILESYSTEM_MOUNT;

struct ASAS_FILESYSTEM_DRIVER {
    const char *name;
    UINT32 flags;
    int (*probe)(ASAS_BLOCK_DEVICE *device);
    int (*mount)(ASAS_FILESYSTEM_MOUNT *mount);
    int (*stat)(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                ASAS_FILE_STAT *stat);
    UINT64 (*read)(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                   void *buffer, UINT64 capacity);
    UINT64 (*list)(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                   ASAS_FILESYSTEM_ENTRY *entries, UINT64 capacity);
    int (*write)(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                 const void *buffer, UINT64 size);
    int (*unlink)(ASAS_FILESYSTEM_MOUNT *mount, const char *path);
    int (*mkdir)(ASAS_FILESYSTEM_MOUNT *mount, const char *path);
    int (*rmdir)(ASAS_FILESYSTEM_MOUNT *mount, const char *path);
    int (*rename)(ASAS_FILESYSTEM_MOUNT *mount, const char *source,
                  const char *destination);
    int (*sync)(ASAS_FILESYSTEM_MOUNT *mount);
};

struct ASAS_FILESYSTEM_MOUNT {
    UINT32 id;
    UINT32 generation;
    UINT32 reference_count;
    ASAS_BLOCK_DEVICE *device;
    const ASAS_FILESYSTEM_DRIVER *driver;
    UINT32 flags;
    void *context;
    volatile long lock_value;
    UINT8 active;
};

void filesystem_initialize(void);
int filesystem_register(const ASAS_FILESYSTEM_DRIVER *driver);
const ASAS_FILESYSTEM_DRIVER *filesystem_probe(ASAS_BLOCK_DEVICE *device);
ASAS_FILESYSTEM_MOUNT *filesystem_mount(ASAS_BLOCK_DEVICE *device,
                                       const ASAS_FILESYSTEM_DRIVER *driver,
                                       UINT32 flags);
UINT32 filesystem_mount_count(void);
ASAS_FILESYSTEM_MOUNT *filesystem_get_mount(UINT32 index);
int filesystem_unmount(ASAS_FILESYSTEM_MOUNT *mount);
int filesystem_mount_acquire(ASAS_FILESYSTEM_MOUNT *mount);
void filesystem_mount_release(ASAS_FILESYSTEM_MOUNT *mount);
int filesystem_device_is_mounted(const ASAS_BLOCK_DEVICE *device);
void filesystem_register_builtin_drivers(void);

int filesystem_stat(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                    ASAS_FILE_STAT *stat);
UINT64 filesystem_read(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                       void *buffer, UINT64 capacity);
UINT64 filesystem_list(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                       ASAS_FILESYSTEM_ENTRY *entries, UINT64 capacity);
int filesystem_write(ASAS_FILESYSTEM_MOUNT *mount, const char *path,
                     const void *buffer, UINT64 size);
int filesystem_unlink(ASAS_FILESYSTEM_MOUNT *mount, const char *path);
int filesystem_mkdir(ASAS_FILESYSTEM_MOUNT *mount, const char *path);
int filesystem_rmdir(ASAS_FILESYSTEM_MOUNT *mount, const char *path);
int filesystem_rename(ASAS_FILESYSTEM_MOUNT *mount, const char *source,
                      const char *destination);
int filesystem_sync(ASAS_FILESYSTEM_MOUNT *mount);

#endif
