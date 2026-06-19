#ifndef ASAS_LIBC_H
#define ASAS_LIBC_H

#include "uefi.h"

#ifdef __cplusplus
extern "C" {
#endif

UINTN asas_strlen(const char *text);
int asas_memcmp(const void *left, const void *right, UINTN count);
void asas_heap_initialize(void *memory, UINTN size);
void *asas_malloc(UINTN size);
void asas_free(void *pointer);
UINT64 asas_getpid(void);
UINT64 asas_write(const char *text);
UINT64 asas_open(const char *path);
UINT64 asas_read(UINT64 handle, void *buffer, UINT64 size);
void asas_exit(UINT64 status);

#ifdef __cplusplus
}
#endif

#endif
