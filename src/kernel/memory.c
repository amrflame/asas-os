#include "memory.h"

#define EFI_CONVENTIONAL_MEMORY 7
#define PAGE_SIZE 4096ULL
#define LOW_MEMORY_RESERVE 0x100000ULL

static int select_next_region(ASAS_FRAME_ALLOCATOR *allocator)
{
    UINTN descriptor_count = allocator->memory_map_size / allocator->descriptor_size;

    while (allocator->descriptor_index < descriptor_count) {
        EFI_MEMORY_DESCRIPTOR *descriptor = (EFI_MEMORY_DESCRIPTOR *)(
            allocator->memory_map + allocator->descriptor_index * allocator->descriptor_size
        );
        allocator->descriptor_index++;

        if (descriptor->type == EFI_CONVENTIONAL_MEMORY && descriptor->number_of_pages != 0) {
            UINT64 region_start = descriptor->physical_start;
            UINT64 region_end = region_start + descriptor->number_of_pages * PAGE_SIZE;

            if (region_end <= LOW_MEMORY_RESERVE) {
                continue;
            }

            if (region_start < LOW_MEMORY_RESERVE) {
                region_start = LOW_MEMORY_RESERVE;
            }

            allocator->next_frame = region_start;
            allocator->remaining_pages = (region_end - region_start) / PAGE_SIZE;
            return 1;
        }
    }

    return 0;
}

void frame_allocator_initialize(
    ASAS_FRAME_ALLOCATOR *allocator,
    void *memory_map,
    UINTN memory_map_size,
    UINTN descriptor_size
)
{
    allocator->memory_map = (UINT8 *)memory_map;
    allocator->memory_map_size = memory_map_size;
    allocator->descriptor_size = descriptor_size;
    allocator->descriptor_index = 0;
    allocator->next_frame = 0;
    allocator->remaining_pages = 0;
    allocator->allocated_pages = 0;
    allocator->free_count = 0;
}

UINT64 frame_allocate(ASAS_FRAME_ALLOCATOR *allocator)
{
    UINT64 frame;

    /* Prefer recycled frames from the free list. */
    if (allocator->free_count > 0) {
        allocator->free_count--;
        allocator->allocated_pages++;
        return allocator->free_list[allocator->free_count];
    }

    if (allocator->remaining_pages == 0 && !select_next_region(allocator)) {
        return 0;
    }

    frame = allocator->next_frame;
    allocator->next_frame += PAGE_SIZE;
    allocator->remaining_pages--;
    allocator->allocated_pages++;
    return frame;
}

void frame_free(ASAS_FRAME_ALLOCATOR *allocator, UINT64 frame)
{
    if (frame == 0 || (frame & (PAGE_SIZE - 1)) != 0) {
        return;
    }
    if (allocator->free_count < FRAME_FREE_LIST_CAPACITY) {
        allocator->free_list[allocator->free_count] = frame;
        allocator->free_count++;
        allocator->allocated_pages--;
    }
    /* If the free list is full the frame is silently discarded;
       a bitmap allocator would be needed to handle that case. */
}

UINT64 frame_allocator_allocated_pages(const ASAS_FRAME_ALLOCATOR *allocator)
{
    return allocator->allocated_pages;
}
