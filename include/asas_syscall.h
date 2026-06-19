#ifndef ASAS_SYSCALL_API_H
#define ASAS_SYSCALL_API_H

#include "uefi.h"

typedef enum {
    ASAS_SYSCALL_VERIFY = 1,
    ASAS_SYSCALL_GETPID = 2,
    ASAS_SYSCALL_WRITE = 3,
    ASAS_SYSCALL_EXIT = 4,
    ASAS_SYSCALL_OPEN = 5,
    ASAS_SYSCALL_READ = 6
} ASAS_SYSCALL_NUMBER;

#ifdef __cplusplus
extern "C" {
#endif

UINT64 asas_system_call(UINT64 number, UINT64 argument0, UINT64 argument1, UINT64 argument2);

#ifdef __cplusplus
}
#endif

#endif
