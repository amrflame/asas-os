#ifndef ASAS_IPC_H
#define ASAS_IPC_H

#include "uefi.h"

#define ASAS_IPC_MESSAGE_SIZE 64

typedef struct {
    UINT32 sender_pid;
    UINT32 receiver_pid;
    UINT32 length;
    UINT8 data[ASAS_IPC_MESSAGE_SIZE];
} ASAS_IPC_MESSAGE;

void ipc_initialize(void);
int ipc_send(UINT32 sender_pid, UINT32 receiver_pid, const void *data, UINT32 length);
int ipc_receive(UINT32 receiver_pid, ASAS_IPC_MESSAGE *message);
int ipc_self_test(void);

#endif

