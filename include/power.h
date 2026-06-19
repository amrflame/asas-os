#ifndef ASAS_POWER_H
#define ASAS_POWER_H

#include "uefi.h"

typedef enum {
    ASAS_BATTERY_UNAVAILABLE = 0,
    ASAS_BATTERY_STATUS_UNSUPPORTED = 1
} ASAS_BATTERY_STATUS;

int power_initialize(UINT64 rsdp_address);
int power_can_shutdown(void);
int power_can_sleep(void);
int power_can_reboot(void);
int power_shutdown(void);
int power_reboot(void);
int power_sleep(void);
int power_battery_namespace_present(void);
ASAS_BATTERY_STATUS power_battery_status(void);
int power_self_test(void);

#endif
