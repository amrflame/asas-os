#include "syscall.h"
#include "logger.h"
#include "asas_syscall.h"
#include "vfs.h"

#pragma intrinsic(__halt)
void __halt(void);

/* Validate that an address range lies entirely within canonical user space.
   User space: 0x0000000000001000 .. 0x00007FFFFFFFFFFF (excludes NULL page). */
static int is_user_pointer(UINT64 address, UINT64 length)
{
    if (address < 0x1000ULL) {
        return 0;
    }
    if (address >= 0x0000800000000000ULL) {
        return 0;
    }
    if (length > 0 && (address + length) > 0x0000800000000000ULL) {
        return 0;
    }
    return 1;
}

UINT64 syscall_dispatch(UINT64 number, UINT64 argument0, UINT64 argument1, UINT64 argument2)
{
    (void)argument1;
    (void)argument2;

    if (number == ASAS_SYSCALL_VERIFY && argument0 == 0x41534153ULL) {
        logger_write("INFO", "user mode system call verified");
        return 1;
    }

    if (number == ASAS_SYSCALL_GETPID) {
        return 1;
    }

    if (number == ASAS_SYSCALL_WRITE) {
        if (!is_user_pointer(argument0, argument1)) {
            return (UINT64)-1;
        }
        logger_write("USER", (const char *)(UINTN)argument0);
        return argument1;
    }

    if (number == ASAS_SYSCALL_EXIT) {
        logger_write("INFO", "user program exited");
        for (;;) {
            __halt();
        }
    }

    if (number == ASAS_SYSCALL_OPEN) {
        if (!is_user_pointer(argument0, 1)) {
            return 0;
        }
        return vfs_open((const char *)(UINTN)argument0);
    }

    if (number == ASAS_SYSCALL_READ) {
        if (!is_user_pointer(argument1, argument2)) {
            return 0;
        }
        return vfs_read(argument0, (void *)(UINTN)argument1, argument2);
    }

    return (UINT64)-1;
}
