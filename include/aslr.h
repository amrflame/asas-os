#ifndef ASAS_ASLR_H
#define ASAS_ASLR_H

#include "uefi.h"

void aslr_initialize(UINT64 seed);
UINT64 aslr_user_stack_address(void);
int aslr_self_test(void);

#endif
