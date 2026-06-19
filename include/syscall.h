#ifndef ASAS_SYSCALL_H
#define ASAS_SYSCALL_H

#include "paging.h"

UINT64 syscall_dispatch(UINT64 number, UINT64 argument0, UINT64 argument1, UINT64 argument2);

#endif
