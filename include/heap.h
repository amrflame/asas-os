#ifndef ASAS_HEAP_H
#define ASAS_HEAP_H

#include "memory.h"
#include "paging.h"

int heap_initialize(ASAS_PAGE_TABLES *tables, ASAS_FRAME_ALLOCATOR *allocator);
void *kmalloc(UINTN size);
void kfree(void *pointer);
UINTN heap_free_bytes(void);
int heap_fragmentation_self_test(void);

#endif
