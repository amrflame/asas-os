#include "heap.h"

#define HEAP_BASE 0xFFFF900000000000ULL
#define HEAP_PAGE_COUNT 2048
#define HEAP_SIZE (HEAP_PAGE_COUNT * 4096ULL)

typedef struct HEAP_BLOCK {
    UINTN size;
    UINT32 free;
    UINT32 reserved;
    struct HEAP_BLOCK *next;
} HEAP_BLOCK;

static HEAP_BLOCK *heap_head;

static UINTN align_size(UINTN size)
{
    return (size + 15U) & ~15ULL;
}

int heap_initialize(ASAS_PAGE_TABLES *tables, ASAS_FRAME_ALLOCATOR *allocator)
{
    UINT32 page;

    for (page = 0; page < HEAP_PAGE_COUNT; page++) {
        UINT64 frame = frame_allocate(allocator);
        UINT64 virtual_address = HEAP_BASE + (UINT64)page * 4096ULL;

        if (
            frame == 0 ||
            !map_page(tables, virtual_address, frame, ASAS_PAGE_WRITABLE | ASAS_PAGE_NO_EXECUTE)
        ) {
            return 0;
        }
    }

    heap_head = (HEAP_BLOCK *)(UINTN)HEAP_BASE;
    heap_head->size = HEAP_SIZE - sizeof(HEAP_BLOCK);
    heap_head->free = 1;
    heap_head->reserved = 0;
    heap_head->next = 0;
    return 1;
}

void *kmalloc(UINTN size)
{
    HEAP_BLOCK *block = heap_head;
    UINTN aligned_size = align_size(size);

    if (aligned_size == 0) {
        return 0;
    }

    while (block != 0) {
        if (block->free && block->size >= aligned_size) {
            if (block->size >= aligned_size + sizeof(HEAP_BLOCK) + 16) {
                HEAP_BLOCK *next = (HEAP_BLOCK *)(
                    (UINT8 *)(block + 1) + aligned_size
                );
                next->size = block->size - aligned_size - sizeof(HEAP_BLOCK);
                next->free = 1;
                next->reserved = 0;
                next->next = block->next;
                block->next = next;
                block->size = aligned_size;
            }

            block->free = 0;
            return block + 1;
        }

        block = block->next;
    }

    return 0;
}

void kfree(void *pointer)
{
    HEAP_BLOCK *block;
    HEAP_BLOCK *current;

    if (pointer == 0) {
        return;
    }

    block = ((HEAP_BLOCK *)pointer) - 1;
    block->free = 1;

    current = heap_head;
    while (current != 0 && current->next != 0) {
        if (current->free && current->next->free) {
            current->size += sizeof(HEAP_BLOCK) + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

UINTN heap_free_bytes(void)
{
    HEAP_BLOCK *block = heap_head;
    UINTN total = 0;

    while (block != 0) {
        if (block->free) {
            total += block->size;
        }
        block = block->next;
    }

    return total;
}

int heap_fragmentation_self_test(void)
{
    void *first;
    void *second;
    void *third;
    void *merged;

    first = kmalloc(1024);
    second = kmalloc(1024);
    third = kmalloc(1024);
    if (first == 0 || second == 0 || third == 0) {
        return 0;
    }

    kfree(second);
    kfree(first);
    merged = kmalloc(1800);
    if (merged == 0) {
        kfree(third);
        return 0;
    }

    kfree(merged);
    kfree(third);
    return 1;
}
