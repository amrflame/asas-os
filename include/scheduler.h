#ifndef ASAS_SCHEDULER_H
#define ASAS_SCHEDULER_H

typedef void (*ASAS_THREAD_ENTRY)(void);

void scheduler_initialize(void);
int scheduler_create_thread(ASAS_THREAD_ENTRY entry);
void scheduler_yield(void);
int scheduler_self_test(void);
void scheduler_on_timer_tick(void);
void scheduler_enable_preemption(void);
void scheduler_disable_preemption(void);
int scheduler_preemption_self_test(void);

#endif
