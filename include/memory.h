#ifndef ASAS_MEMORY_H
#define ASAS_MEMORY_H

#include "uefi.h"

#define FRAME_FREE_LIST_CAPACITY 256U

typedef struct {
    UINT8 *memory_map;
    UINTN memory_map_size;
    UINTN descriptor_size;
    UINTN descriptor_index;
    UINT64 next_frame;
    UINT64 remaining_pages;
    UINT64 allocated_pages;
    UINT64 free_list[FRAME_FREE_LIST_CAPACITY];
    UINT32 free_count;
} ASAS_FRAME_ALLOCATOR;

void frame_allocator_initialize(
    ASAS_FRAME_ALLOCATOR *allocator,
    void *memory_map,
    UINTN memory_map_size,
    UINTN descriptor_size
);

UINT64 frame_allocate(ASAS_FRAME_ALLOCATOR *allocator);
void   frame_free(ASAS_FRAME_ALLOCATOR *allocator, UINT64 frame);
UINT64 frame_allocator_allocated_pages(const ASAS_FRAME_ALLOCATOR *allocator);

#endif

