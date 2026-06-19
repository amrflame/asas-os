#include "ipc.h"

#pragma intrinsic(_InterlockedExchange)
long _InterlockedExchange(long volatile *target, long value);

#define IPC_QUEUE_CAPACITY 32

static ASAS_IPC_MESSAGE queue[IPC_QUEUE_CAPACITY];
static UINT8 queue_used[IPC_QUEUE_CAPACITY];
static UINT32 queue_head;
static UINT32 queue_tail;
static UINT32 queue_count;
static volatile long ipc_lock_value;

static void ipc_lock(void)
{
    while (_InterlockedExchange(&ipc_lock_value, 1) != 0) {
    }
}

static void ipc_unlock(void)
{
    (void)_InterlockedExchange(&ipc_lock_value, 0);
}

void ipc_initialize(void)
{
    UINT32 index;

    queue_head = 0;
    queue_tail = 0;
    queue_count = 0;
    for (index = 0; index < IPC_QUEUE_CAPACITY; index++) {
        queue_used[index] = 0;
    }
}

int ipc_send(UINT32 sender_pid, UINT32 receiver_pid, const void *data, UINT32 length)
{
    ASAS_IPC_MESSAGE *message;
    const UINT8 *bytes = (const UINT8 *)data;
    UINT32 index;
    if (length > ASAS_IPC_MESSAGE_SIZE) {
        return 0;
    }

    ipc_lock();
    if (queue_count >= IPC_QUEUE_CAPACITY) {
        ipc_unlock();
        return 0;
    }

    while (queue_used[queue_tail]) {
        queue_tail = (queue_tail + 1) % IPC_QUEUE_CAPACITY;
    }
    message = &queue[queue_tail];
    queue_used[queue_tail] = 1;
    queue_tail = (queue_tail + 1) % IPC_QUEUE_CAPACITY;
    queue_count++;
    message->sender_pid = sender_pid;
    message->receiver_pid = receiver_pid;
    message->length = length;

    for (index = 0; index < length; index++) {
        message->data[index] = bytes[index];
    }
    ipc_unlock();
    return 1;
}

int ipc_receive(UINT32 receiver_pid, ASAS_IPC_MESSAGE *message)
{
    UINT32 index;

    ipc_lock();
    for (index = 0; index < IPC_QUEUE_CAPACITY; index++) {
        UINT32 slot = (queue_head + index) % IPC_QUEUE_CAPACITY;

        if (!queue_used[slot] || queue[slot].receiver_pid != receiver_pid) {
            continue;
        }

        *message = queue[slot];
        queue_used[slot] = 0;
        queue_count--;
        if (queue_count == 0) {
            queue_head = 0;
            queue_tail = 0;
        } else {
            while (!queue_used[queue_head]) {
                queue_head = (queue_head + 1) % IPC_QUEUE_CAPACITY;
            }
        }
        ipc_unlock();
        return 1;
    }

    ipc_unlock();
    return 0;
}

int ipc_self_test(void)
{
    static const UINT8 payload[] = { 'A', 'S', 'A', 'S' };
    ASAS_IPC_MESSAGE message;

    ipc_initialize();
    if (!ipc_send(1, 2, payload, sizeof(payload)) || !ipc_receive(2, &message)) {
        return 0;
    }

    return
        message.sender_pid == 1 &&
        message.receiver_pid == 2 &&
        message.length == sizeof(payload) &&
        message.data[0] == 'A' &&
        message.data[3] == 'S';
}
