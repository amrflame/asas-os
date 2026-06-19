#ifndef ASAS_XHCI_H
#define ASAS_XHCI_H

#include "pci.h"
#include "paging.h"

typedef struct {
    UINT64 mmio_base;
    UINT16 version;
    UINT8 max_slots;
    UINT8 max_ports;
    UINT8 connected_ports;
    UINT8 running;
    UINT8 enabled_slot;
    UINT8 addressed_port;
    UINT8 port_speed;
    UINT8 device_addressed;
    UINT8 descriptor_read;
    UINT16 vendor_id;
    UINT16 product_id;
    UINT8 interface_class;
    UINT8 interface_protocol;
    UINT8 hid_keyboard_detected;
    UINT8 interrupt_endpoint_address;
    UINT8 interrupt_endpoint_interval;
    UINT16 interrupt_endpoint_packet_size;
    UINT8 interrupt_endpoint_configured;
    UINT8 configuration_set;
    UINT8 hid_report_queued;
    UINT64 hid_report_count;
    UINT8 mouse_enabled_slot;
    UINT8 mouse_addressed_port;
    UINT8 hid_mouse_detected;
    UINT8 mouse_interrupt_endpoint_address;
    UINT16 mouse_interrupt_endpoint_packet_size;
    UINT8 mouse_interrupt_endpoint_configured;
    UINT8 mouse_report_queued;
    UINT64 mouse_report_count;
    UINT8 storage_enabled_slot;
    UINT8 storage_addressed_port;
    UINT8 usb_storage_detected;
    UINT8 storage_bulk_in_endpoint_address;
    UINT8 storage_bulk_out_endpoint_address;
    UINT16 storage_bulk_in_packet_size;
    UINT16 storage_bulk_out_packet_size;
    UINT8 storage_bulk_endpoints_configured;
    UINT8 storage_inquiry_completed;
    UINT8 storage_capacity_read;
    UINT8 storage_sector_read;
    UINT32 storage_block_size;
    UINT32 storage_sector_count;
    UINT8 storage_read_only;
    UINT8 supported_device_capacity;
    UINT32 static_dma_bytes;
} ASAS_XHCI_CONTROLLER;

int xhci_initialize(
    const ASAS_PCI_DEVICE *device,
    ASAS_XHCI_CONTROLLER *controller,
    ASAS_PAGE_TABLES *tables
);
int xhci_poll_keyboard(ASAS_XHCI_CONTROLLER *controller);
int xhci_poll_devices(ASAS_XHCI_CONTROLLER *controller);
int xhci_poll_active_keyboard(void);
int xhci_storage_read_blocks(UINT64 lba, UINT32 count, void *buffer);
int xhci_storage_write_blocks(UINT64 lba, UINT32 count, const void *buffer);
int xhci_storage_flush(void);
int xhci_register_storage_block_device(void);

#endif
