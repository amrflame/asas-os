#ifndef ASAS_LAPTOP_H
#define ASAS_LAPTOP_H

#include "pci.h"

int laptop_initialize(UINT64 rsdp_address, const ASAS_PCI_REGISTRY *pci_registry);
int laptop_touchpad_present(void);
int laptop_wifi_present(void);
int laptop_self_test(void);

#endif
