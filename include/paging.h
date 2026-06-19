#ifndef ASAS_PAGING_H
#define ASAS_PAGING_H

#include "memory.h"

typedef struct {
    UINT64 *pml4;
    ASAS_FRAME_ALLOCATOR *frame_allocator;
} ASAS_PAGE_TABLES;

#define ASAS_PAGE_WRITABLE 0x002ULL
#define ASAS_PAGE_USER 0x004ULL
#define ASAS_PAGE_NO_EXECUTE (1ULL << 63)

void paging_initialize(ASAS_PAGE_TABLES *tables, ASAS_FRAME_ALLOCATOR *allocator);
void paging_enable_nx(void);
int map_page(ASAS_PAGE_TABLES *tables, UINT64 virtual_address, UINT64 physical_address, UINT64 flags);
UINT64 paging_page_flags(ASAS_PAGE_TABLES *tables, UINT64 virtual_address);
int paging_create_address_space(
    ASAS_PAGE_TABLES *destination,
    const ASAS_PAGE_TABLES *kernel_tables,
    ASAS_FRAME_ALLOCATOR *allocator
);
void paging_destroy_address_space(ASAS_PAGE_TABLES *tables);

#endif
