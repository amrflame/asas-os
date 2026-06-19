#ifndef ASAS_PE_LOADER_H
#define ASAS_PE_LOADER_H

#include "memory.h"
#include "paging.h"

int pe_load_and_enter_user_program(
    const char fat_name[11],
    ASAS_PAGE_TABLES *tables,
    ASAS_FRAME_ALLOCATOR *allocator
);

#endif

