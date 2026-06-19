#include "scheduler.h"
#include "uefi.h"

#pragma intrinsic(__halt)
void __halt(void);
#pragma intrinsic(_InterlockedExchange)
long _InterlockedExchange(long volatile *target, long value);

#define MAX_THREADS 16
#define THREAD_STACK_SIZE 16384

typedef struct {
    UINT64 stack_pointer;
    ASAS_THREAD_ENTRY entry;
    UINT32 active;
} ASAS_THREAD;

extern void context_switch(UINT64 *old_stack_pointer, UINT64 new_stack_pointer);
extern void interrupts_enable(void);

static ASAS_THREAD threads[MAX_THREADS];
static UINT8 thread_stacks[MAX_THREADS][THREAD_STACK_SIZE];
static UINT32 current_thread;
static UINT32 thread_count;
static volatile UINT32 self_test_counter;
static volatile UINT32 preemption_enabled;
static volatile UINT32 preemption_test_complete;
static volatile long scheduler_lock_value;

static void scheduler_lock(void)
{
    while (_InterlockedExchange(&scheduler_lock_value, 1) != 0) {
    }
}

static void scheduler_unlock(void)
{
    (void)_InterlockedExchange(&scheduler_lock_value, 0);
}

static void thread_bootstrap(void)
{
    ASAS_THREAD_ENTRY entry = threads[current_thread].entry;

    interrupts_enable();
    entry();
    threads[current_thread].active = 0;

    for (;;) {
        scheduler_yield();
        __halt();
    }
}

void scheduler_initialize(void)
{
    UINT32 index;

    for (index = 0; index < MAX_THREADS; index++) {
        threads[index].stack_pointer = 0;
        threads[index].entry = 0;
        threads[index].active = 0;
    }

    current_thread = 0;
    thread_count = 1;
    threads[0].active = 1;
    preemption_enabled = 0;
}

int scheduler_create_thread(ASAS_THREAD_ENTRY entry)
{
    UINT32 index;

    scheduler_lock();
    for (index = 1; index < MAX_THREADS; index++) {
        if (!threads[index].active) {
            UINT64 *stack = (UINT64 *)(UINTN)&thread_stacks[index][THREAD_STACK_SIZE];
            UINT32 register_index;

            *--stack = (UINT64)(UINTN)thread_bootstrap;
            for (register_index = 0; register_index < 8; register_index++) {
                *--stack = 0;
            }

            threads[index].stack_pointer = (UINT64)(UINTN)stack;
            threads[index].entry = entry;
            threads[index].active = 1;
            if (index >= thread_count) {
                thread_count = index + 1;
            }
            scheduler_unlock();
            return 1;
        }
    }

    scheduler_unlock();
    return 0;
}

void scheduler_yield(void)
{
    UINT32 next = current_thread;
    UINT32 checked;

    scheduler_lock();
    for (checked = 0; checked < thread_count; checked++) {
        next = (next + 1) % thread_count;
        if (threads[next].active) {
            UINT32 previous = current_thread;
            if (next == previous) {
                scheduler_unlock();
                return;
            }
            current_thread = next;
            scheduler_unlock();
            context_switch(&threads[previous].stack_pointer, threads[next].stack_pointer);
            return;
        }
    }
    scheduler_unlock();
}

static void scheduler_test_worker(void)
{
    self_test_counter = 0x41534153U;
    scheduler_yield();
}

int scheduler_self_test(void)
{
    self_test_counter = 0;

    if (!scheduler_create_thread(scheduler_test_worker)) {
        return 0;
    }

    scheduler_yield();
    return self_test_counter == 0x41534153U;
}

void scheduler_on_timer_tick(void)
{
    if (preemption_enabled) {
        scheduler_yield();
    }
}

void scheduler_enable_preemption(void)
{
    preemption_enabled = 1;
}

void scheduler_disable_preemption(void)
{
    preemption_enabled = 0;
}

static void preemption_busy_worker(void)
{
    while (!preemption_test_complete) {
    }
}

static void preemption_signal_worker(void)
{
    preemption_test_complete = 1;
}

int scheduler_preemption_self_test(void)
{
    preemption_test_complete = 0;

    if (
        !scheduler_create_thread(preemption_busy_worker) ||
        !scheduler_create_thread(preemption_signal_worker)
    ) {
        return 0;
    }

    scheduler_enable_preemption();
    scheduler_yield();
    return preemption_test_complete != 0;
}
