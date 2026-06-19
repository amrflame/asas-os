#include "stability.h"
#include "heap.h"
#include "logger.h"

#define STABILITY_HEAP_BLOCKS 32
#define STABILITY_FRAME_BLOCKS 8

static int heap_stress_test(void)
{
    void *blocks[STABILITY_HEAP_BLOCKS];
    UINTN before = heap_free_bytes();
    UINT32 index;

    for (index = 0; index < STABILITY_HEAP_BLOCKS; index++) {
        UINT8 *block = (UINT8 *)kmalloc(32 + index * 7);

        if (block == 0) {
            return 0;
        }
        block[0] = (UINT8)index;
        block[31] = (UINT8)(index ^ 0x5A);
        blocks[index] = block;
    }

    for (index = 0; index < STABILITY_HEAP_BLOCKS; index += 2) {
        kfree(blocks[index]);
    }
    for (index = 1; index < STABILITY_HEAP_BLOCKS; index += 2) {
        kfree(blocks[index]);
    }

    return heap_free_bytes() == before;
}

static int frame_allocator_leak_test(ASAS_FRAME_ALLOCATOR *allocator)
{
    UINT64 frames[STABILITY_FRAME_BLOCKS];
    UINT64 before = frame_allocator_allocated_pages(allocator);
    UINT32 index;

    for (index = 0; index < STABILITY_FRAME_BLOCKS; index++) {
        frames[index] = frame_allocate(allocator);
        if (frames[index] == 0) {
            return 0;
        }
    }

    for (index = 0; index < STABILITY_FRAME_BLOCKS; index++) {
        frame_free(allocator, frames[index]);
    }

    return frame_allocator_allocated_pages(allocator) == before;
}

int stability_self_test(ASAS_FRAME_ALLOCATOR *allocator)
{
    if (!heap_stress_test()) {
        return 0;
    }
    logger_write("INFO", "kernel heap stress leak check verified");

    if (!frame_allocator_leak_test(allocator)) {
        return 0;
    }
    logger_write("INFO", "frame allocator stress leak check verified");
    logger_write("INFO", "memory stress and leak tests verified");
    return 1;
}
