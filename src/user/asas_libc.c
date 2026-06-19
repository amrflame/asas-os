#include "asas_libc.h"
#include "asas_syscall.h"

typedef struct USER_HEAP_BLOCK {
    UINTN size;
    UINT32 free;
    UINT32 reserved;
    struct USER_HEAP_BLOCK *next;
} USER_HEAP_BLOCK;

static USER_HEAP_BLOCK *heap_head;

UINTN asas_strlen(const char *text)
{
    UINTN length = 0;

    while (text[length] != '\0') {
        length++;
    }

    return length;
}

int asas_memcmp(const void *left, const void *right, UINTN count)
{
    const UINT8 *left_bytes = (const UINT8 *)left;
    const UINT8 *right_bytes = (const UINT8 *)right;
    UINTN index;

    for (index = 0; index < count; index++) {
        if (left_bytes[index] != right_bytes[index]) {
            return left_bytes[index] < right_bytes[index] ? -1 : 1;
        }
    }

    return 0;
}

void asas_heap_initialize(void *memory, UINTN size)
{
    heap_head = (USER_HEAP_BLOCK *)memory;
    heap_head->size = size - sizeof(USER_HEAP_BLOCK);
    heap_head->free = 1;
    heap_head->reserved = 0;
    heap_head->next = 0;
}

void *asas_malloc(UINTN size)
{
    USER_HEAP_BLOCK *block = heap_head;
    UINTN aligned_size = (size + 15U) & ~15ULL;

    if (aligned_size == 0) {
        return 0;
    }

    while (block != 0) {
        if (block->free && block->size >= aligned_size) {
            if (block->size >= aligned_size + sizeof(USER_HEAP_BLOCK) + 16) {
                USER_HEAP_BLOCK *next = (USER_HEAP_BLOCK *)(
                    (UINT8 *)(block + 1) + aligned_size
                );
                next->size = block->size - aligned_size - sizeof(USER_HEAP_BLOCK);
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

void asas_free(void *pointer)
{
    USER_HEAP_BLOCK *current;

    if (pointer == 0) {
        return;
    }

    (((USER_HEAP_BLOCK *)pointer) - 1)->free = 1;

    current = heap_head;
    while (current != 0 && current->next != 0) {
        if (current->free && current->next->free) {
            current->size += sizeof(USER_HEAP_BLOCK) + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

UINT64 asas_getpid(void)
{
    return asas_system_call(ASAS_SYSCALL_GETPID, 0, 0, 0);
}

UINT64 asas_write(const char *text)
{
    return asas_system_call(ASAS_SYSCALL_WRITE, (UINT64)(UINTN)text, asas_strlen(text), 0);
}

UINT64 asas_open(const char *path)
{
    return asas_system_call(ASAS_SYSCALL_OPEN, (UINT64)(UINTN)path, 0, 0);
}

UINT64 asas_read(UINT64 handle, void *buffer, UINT64 size)
{
    return asas_system_call(ASAS_SYSCALL_READ, handle, (UINT64)(UINTN)buffer, size);
}

void asas_exit(UINT64 status)
{
    (void)asas_system_call(ASAS_SYSCALL_EXIT, status, 0, 0);

    for (;;) {
    }
}
