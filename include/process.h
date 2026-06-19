#ifndef ASAS_PROCESS_H
#define ASAS_PROCESS_H

#include "paging.h"

#define ASAS_MAX_PROCESSES 16

typedef enum {
    PROCESS_UNUSED = 0,
    PROCESS_READY = 1,
    PROCESS_RUNNING = 2,
    PROCESS_BLOCKED = 3,
    PROCESS_EXITED = 4
} ASAS_PROCESS_STATE;

typedef struct {
    UINT32 pid;
    ASAS_PROCESS_STATE state;
    ASAS_PAGE_TABLES address_space;
    UINT64 user_entry;
    UINT64 user_stack;
} ASAS_PROCESS;

void process_initialize(void);
ASAS_PROCESS *process_create(
    const ASAS_PAGE_TABLES *kernel_tables,
    ASAS_FRAME_ALLOCATOR *allocator
);
UINT32 process_active_count(void);
int process_kill(UINT32 pid);
int process_self_test(
    const ASAS_PAGE_TABLES *kernel_tables,
    ASAS_FRAME_ALLOCATOR *allocator
);

#endif
