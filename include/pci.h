#ifndef ASAS_PCI_H
#define ASAS_PCI_H

#include "uefi.h"

#define ASAS_MAX_PCI_DEVICES 128
#define ASAS_PCI_MAX_BUSES 32
#define PCI_VENDOR_VIRTIO 0x1AF4

typedef struct {
    UINT8 bus;
    UINT8 device;
    UINT8 function;
    UINT8 class_code;
    UINT8 subclass;
    UINT8 programming_interface;
    UINT8 interrupt_line;
    UINT16 vendor_id;
    UINT16 device_id;
    UINT32 bars[6];
} ASAS_PCI_DEVICE;

typedef struct {
    UINT32 count;
    ASAS_PCI_DEVICE devices[ASAS_MAX_PCI_DEVICES];
} ASAS_PCI_REGISTRY;

void pci_discover_devices(ASAS_PCI_REGISTRY *registry);
const ASAS_PCI_DEVICE *pci_find_device(
    const ASAS_PCI_REGISTRY *registry,
    UINT16 vendor_id,
    UINT16 device_id
);
const ASAS_PCI_DEVICE *pci_find_class(
    const ASAS_PCI_REGISTRY *registry,
    UINT8 class_code,
    UINT8 subclass,
    UINT8 programming_interface
);
void pci_enable_bus_mastering(const ASAS_PCI_DEVICE *device);
void pci_write_bar(const ASAS_PCI_DEVICE *device, UINT32 bar_index, UINT32 value);

#endif
