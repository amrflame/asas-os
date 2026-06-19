#ifndef ASAS_HYPERV_STORAGE_H
#define ASAS_HYPERV_STORAGE_H

#include "memory.h"
#include "uefi.h"

typedef struct {
    UINT8 detected;
    UINT8 hypercall_ready;
    UINT8 synic_ready;
    UINT8 contact_posted;
    UINT8 version_response_received;
    UINT8 version_supported;
    UINT8 vmbus_connected;
    UINT8 offers_requested;
    UINT8 all_offers_delivered;
    UINT8 storvsc_offered;
    UINT8 storvsc_gpadl_posted;
    UINT8 storvsc_gpadl_created;
    UINT8 storvsc_open_posted;
    UINT8 storvsc_opened;
    UINT8 keyboard_offered;
    UINT8 keyboard_gpadl_posted;
    UINT8 keyboard_gpadl_created;
    UINT8 keyboard_open_posted;
    UINT8 keyboard_opened;
    UINT8 keyboard_protocol_ready;
    UINT8 hid_input_offered;
    UINT8 hid_input_gpadl_posted;
    UINT8 hid_input_gpadl_created;
    UINT8 hid_input_open_posted;
    UINT8 hid_input_opened;
    UINT8 hid_input_protocol_ready;
    UINT8 hid_input_device_ready;
    UINT64 hypercall_page;
    UINT64 post_message_page;
    UINT64 message_page;
    UINT64 event_flags_page;
    UINT64 monitor_page1;
    UINT64 monitor_page2;
    UINT64 storvsc_ring_page1;
    UINT64 storvsc_ring_page2;
    UINT64 storvsc_ring_page3;
    UINT64 storvsc_ring_page4;
    UINT64 keyboard_ring_page1;
    UINT64 keyboard_ring_page2;
    UINT64 keyboard_ring_page3;
    UINT64 keyboard_ring_page4;
    UINT64 hid_input_ring_page1;
    UINT64 hid_input_ring_page2;
    UINT64 hid_input_ring_page3;
    UINT64 hid_input_ring_page4;
    UINT64 post_status;
    UINT32 message_connection_id;
    UINT32 response_message_type;
    UINT32 response_channel_type;
    UINT32 offer_count;
    UINT32 storvsc_relid;
    UINT32 storvsc_connection_id;
    UINT32 storvsc_gpadl;
    UINT32 storvsc_open_status;
    UINT32 keyboard_relid;
    UINT32 keyboard_connection_id;
    UINT32 keyboard_gpadl;
    UINT32 keyboard_open_status;
    UINT32 keyboard_protocol_status;
    UINT32 keyboard_event_count;
    UINT32 keyboard_packet_count;
    UINT32 keyboard_bad_packet_count;
    UINT32 keyboard_last_make_code;
    UINT32 keyboard_last_info;
    UINT32 hid_input_relid;
    UINT32 hid_input_connection_id;
    UINT32 hid_input_gpadl;
    UINT32 hid_input_open_status;
    UINT32 hid_input_report_count;
    UINT32 hid_input_packet_count;
    UINT32 hid_input_last_report_size;
    UINT32 hid_input_vendor;
    UINT32 hid_input_product;
    UINT32 hid_input_report_desc_size;
    UINT32 hid_input_report_desc0;
    UINT32 hid_input_report_desc1;
    UINT32 hid_input_report_desc2;
    UINT32 hid_input_report_desc3;
    UINT32 hid_input_last_report0;
    UINT32 hid_input_last_report1;
    UINT32 hid_input_last_report2;
    UINT32 hid_input_last_report3;
    UINT32 hid_input_last_report4;
    UINT32 hid_input_last_report5;
    UINT32 hid_input_last_report6;
    UINT32 hid_input_last_report7;
    UINT8 vsp_initialized;
    UINT8 raw_read_ready;
    UINT8 raw_write_supported;
    UINT32 last_srb_status;
    UINT32 last_scsi_status;
    UINT32 last_vstor_status;
} ASAS_HYPERV_STORAGE_STATUS;

int hyperv_storage_initialize(ASAS_FRAME_ALLOCATOR *allocator);
int hyperv_storage_detected(void);
const ASAS_HYPERV_STORAGE_STATUS *hyperv_storage_status(void);
void hyperv_storage_interrupt_handler(void);
int hyperv_vsp_initialize(void);
int hyperv_storage_read_sector(UINT64 lba, void *buffer);
int hyperv_storage_write_sector(UINT64 lba, const void *buffer);
int hyperv_keyboard_ready(void);
void hyperv_keyboard_poll(void);

/* ---- Multi-device probing ---- */
#define ASAS_MAX_STORAGE_DEVICES 8

typedef struct {
    UINT8  valid;
    UINT8  target;
    UINT8  lun;
    UINT8  is_cdrom;
    UINT32 sector_count;
    UINT32 sector_size;
} ASAS_STORAGE_DEVICE;

int                          hyperv_storage_probe_devices(void);
int                          hyperv_storage_rescan_devices(void);
int                          hyperv_storage_get_device_count(void);
const ASAS_STORAGE_DEVICE   *hyperv_storage_get_devices(void);
void                         hyperv_storage_note_device(UINT8 target, UINT8 lun,
                                                        UINT8 is_cdrom,
                                                        UINT32 sector_count,
                                                        UINT32 sector_size);
int                          hyperv_storage_read_sector_ex(UINT8 target, UINT8 lun,
                                                            UINT64 lba, void *buffer);
int                          hyperv_storage_write_sector_ex(UINT8 target, UINT8 lun,
                                                             UINT64 lba, const void *buffer);
int                          hyperv_storage_register_block_device(UINT8 target, UINT8 lun);

#endif
