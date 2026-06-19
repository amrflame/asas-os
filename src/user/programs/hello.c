#include "asas_libc.h"
#include "asas_syscall.h"

void user_main(void)
{
    UINT8 heap[1024];
    UINT64 *value;

    asas_heap_initialize(heap, sizeof(heap));
    value = (UINT64 *)asas_malloc(sizeof(UINT64));

    if (value == 0) {
        asas_exit(1);
    }

    *value = 0x41534153ULL;
    if (
        *value != 0x41534153ULL ||
        asas_getpid() == 0 ||
        asas_system_call(ASAS_SYSCALL_VERIFY, 0x41534153ULL, 0, 0) == 0
    ) {
        asas_exit(2);
    }

    asas_write("Hello from an Asas OS C user program");
    asas_free(value);
    asas_exit(0);
}
