#include "process.h"

#define USER_TEST_PAGE 0x0000600001000000ULL
#define PAGE_ADDRESS_MASK 0x000FFFFFFFFFF000ULL

static ASAS_PROCESS processes[ASAS_MAX_PROCESSES];
static UINT32 next_pid;

void process_initialize(void)
{
    UINT32 index;

    for (index = 0; index < ASAS_MAX_PROCESSES; index++) {
        processes[index].pid = 0;
        processes[index].state = PROCESS_UNUSED;
        processes[index].address_space.pml4 = 0;
        processes[index].address_space.frame_allocator = 0;
        processes[index].user_entry = 0;
        processes[index].user_stack = 0;
    }
    next_pid = 1;
}

ASAS_PROCESS *process_create(
    const ASAS_PAGE_TABLES *kernel_tables,
    ASAS_FRAME_ALLOCATOR *allocator
)
{
    UINT32 index;

    for (index = 0; index < ASAS_MAX_PROCESSES; index++) {
        ASAS_PROCESS *process = &processes[index];

        if (process->state == PROCESS_UNUSED) {
            if (!paging_create_address_space(&process->address_space, kernel_tables, allocator)) {
                return 0;
            }

            process->pid = next_pid++;
            process->state = PROCESS_READY;
            return process;
        }
    }

    return 0;
}

UINT32 process_active_count(void)
{
    UINT32 count = 0;
    UINT32 index;

    for (index = 0; index < ASAS_MAX_PROCESSES; index++) {
        if (processes[index].state != PROCESS_UNUSED && processes[index].state != PROCESS_EXITED) {
            count++;
        }
    }
    return count;
}

int process_kill(UINT32 pid)
{
    UINT32 index;

    for (index = 0; index < ASAS_MAX_PROCESSES; index++) {
        if (processes[index].pid == pid && processes[index].state != PROCESS_UNUSED) {
            paging_destroy_address_space(&processes[index].address_space);
            processes[index].state = PROCESS_EXITED;
            processes[index].user_entry = 0;
            processes[index].user_stack = 0;
            return 1;
        }
    }
    return 0;
}

int process_self_test(
    const ASAS_PAGE_TABLES *kernel_tables,
    ASAS_FRAME_ALLOCATOR *allocator
)
{
    ASAS_PROCESS *first = process_create(kernel_tables, allocator);
    ASAS_PROCESS *second = process_create(kernel_tables, allocator);
    ASAS_PROCESS *cleanup;
    UINT64 first_frame;
    UINT64 second_frame;
    UINT64 cleanup_frame;
    UINT64 first_mapping;
    UINT64 second_mapping;
    UINT64 allocated_before_kill;
    UINT64 allocated_after_kill;

    if (first == 0 || second == 0 || first->pid == second->pid) {
        return 0;
    }

    first_frame = frame_allocate(allocator);
    second_frame = frame_allocate(allocator);
    if (first_frame == 0 || second_frame == 0 || first_frame == second_frame) {
        return 0;
    }

    if (
        !map_page(&first->address_space, USER_TEST_PAGE, first_frame, ASAS_PAGE_USER | ASAS_PAGE_WRITABLE) ||
        !map_page(&second->address_space, USER_TEST_PAGE, second_frame, ASAS_PAGE_USER | ASAS_PAGE_WRITABLE)
    ) {
        return 0;
    }

    first_mapping = paging_page_flags(&first->address_space, USER_TEST_PAGE);
    second_mapping = paging_page_flags(&second->address_space, USER_TEST_PAGE);
    if (
        (first_mapping & ASAS_PAGE_USER) == 0 ||
        (second_mapping & ASAS_PAGE_USER) == 0 ||
        (first_mapping & PAGE_ADDRESS_MASK) == (second_mapping & PAGE_ADDRESS_MASK)
    ) {
        return 0;
    }

    cleanup = process_create(kernel_tables, allocator);
    if (cleanup == 0) {
        return 0;
    }

    cleanup_frame = frame_allocate(allocator);
    if (cleanup_frame == 0) {
        return 0;
    }
    if (!map_page(&cleanup->address_space, USER_TEST_PAGE, cleanup_frame, ASAS_PAGE_USER | ASAS_PAGE_WRITABLE)) {
        return 0;
    }

    allocated_before_kill = frame_allocator_allocated_pages(allocator);
    if (!process_kill(cleanup->pid)) {
        return 0;
    }
    allocated_after_kill = frame_allocator_allocated_pages(allocator);

    return allocated_after_kill < allocated_before_kill;
}
