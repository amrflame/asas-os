#include "pci.h"

#pragma intrinsic(__indword)
unsigned long __indword(unsigned short port);
#pragma intrinsic(__outdword)
void __outdword(unsigned short port, unsigned long value);

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static UINT32 pci_read_config(UINT8 bus, UINT8 device, UINT8 function, UINT8 offset)
{
    UINT32 address =
        0x80000000U |
        ((UINT32)bus << 16) |
        ((UINT32)device << 11) |
        ((UINT32)function << 8) |
        (offset & 0xFCU);

    __outdword(PCI_CONFIG_ADDRESS, address);
    return (UINT32)__indword(PCI_CONFIG_DATA);
}

static void pci_write_config(
    UINT8 bus,
    UINT8 device,
    UINT8 function,
    UINT8 offset,
    UINT32 value
)
{
    UINT32 address =
        0x80000000U |
        ((UINT32)bus << 16) |
        ((UINT32)device << 11) |
        ((UINT32)function << 8) |
        (offset & 0xFCU);

    __outdword(PCI_CONFIG_ADDRESS, address);
    __outdword(PCI_CONFIG_DATA, value);
}

void pci_discover_devices(ASAS_PCI_REGISTRY *registry)
{
    UINT32 bus;
    UINT32 device;
    UINT32 function;

    registry->count = 0;

    for (bus = 0; bus < ASAS_PCI_MAX_BUSES; bus++) {
        for (device = 0; device < 32; device++) {
            UINT32 header = pci_read_config((UINT8)bus, (UINT8)device, 0, 0x0C);
            UINT32 function_count = (header & 0x00800000U) != 0 ? 8 : 1;

            for (function = 0; function < function_count; function++) {
                UINT32 identity = pci_read_config(
                    (UINT8)bus,
                    (UINT8)device,
                    (UINT8)function,
                    0
                );

                if ((identity & 0xFFFFU) != 0xFFFFU) {
                    ASAS_PCI_DEVICE *found;
                    UINT32 class_data;
                    UINT32 interrupt_data;
                    UINT32 bar;

                    if (registry->count >= ASAS_MAX_PCI_DEVICES) {
                        return;
                    }

                    found = &registry->devices[registry->count++];
                    found->bus = (UINT8)bus;
                    found->device = (UINT8)device;
                    found->function = (UINT8)function;
                    found->vendor_id = (UINT16)(identity & 0xFFFFU);
                    found->device_id = (UINT16)(identity >> 16);

                    class_data = pci_read_config(found->bus, found->device, found->function, 0x08);
                    found->programming_interface = (UINT8)(class_data >> 8);
                    found->subclass = (UINT8)(class_data >> 16);
                    found->class_code = (UINT8)(class_data >> 24);
                    interrupt_data = pci_read_config(found->bus, found->device, found->function, 0x3C);
                    found->interrupt_line = (UINT8)interrupt_data;

                    for (bar = 0; bar < 6; bar++) {
                        found->bars[bar] = pci_read_config(
                            found->bus,
                            found->device,
                            found->function,
                            (UINT8)(0x10 + bar * 4)
                        );
                    }
                }
            }
        }
    }

}

const ASAS_PCI_DEVICE *pci_find_device(
    const ASAS_PCI_REGISTRY *registry,
    UINT16 vendor_id,
    UINT16 device_id
)
{
    UINT32 index;

    for (index = 0; index < registry->count; index++) {
        const ASAS_PCI_DEVICE *device = &registry->devices[index];

        if (device->vendor_id == vendor_id && device->device_id == device_id) {
            return device;
        }
    }

    return 0;
}

const ASAS_PCI_DEVICE *pci_find_class(
    const ASAS_PCI_REGISTRY *registry,
    UINT8 class_code,
    UINT8 subclass,
    UINT8 programming_interface
)
{
    UINT32 index;

    for (index = 0; index < registry->count; index++) {
        const ASAS_PCI_DEVICE *device = &registry->devices[index];

        if (
            device->class_code == class_code &&
            device->subclass == subclass &&
            device->programming_interface == programming_interface
        ) {
            return device;
        }
    }

    return 0;
}

void pci_enable_bus_mastering(const ASAS_PCI_DEVICE *device)
{
    UINT32 command = pci_read_config(device->bus, device->device, device->function, 0x04);
    command |= 0x00000007U;
    pci_write_config(device->bus, device->device, device->function, 0x04, command);
}

void pci_write_bar(const ASAS_PCI_DEVICE *device, UINT32 bar_index, UINT32 value)
{
    if (bar_index < 6) {
        pci_write_config(
            device->bus,
            device->device,
            device->function,
            (UINT8)(0x10 + bar_index * 4),
            value
        );
    }
}
