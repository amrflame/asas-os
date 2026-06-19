#ifndef ASAS_CRASH_H
#define ASAS_CRASH_H

#include "uefi.h"

void crash_initialize(void);
void crash_record(const char *category, const char *message, UINT64 code);
UINT32 crash_record_count(void);
const char *crash_last_category(void);
const char *crash_last_message(void);
int crash_self_test(void);

#endif
