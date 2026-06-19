#include "xhci.h"
#include "block_device.h"
#include "keyboard.h"
#include "logger.h"
#include "mouse.h"

#define XHCI_PORT_REGISTER_BASE 0x400
#define XHCI_PORT_REGISTER_STRIDE 0x10
#define XHCI_PORT_CONNECTED 1U
#define XHCI_PORT_ENABLED (1U << 1)
#define XHCI_PORT_RESET (1U << 4)
#define XHCI_PORT_CHANGE_BITS (0x7FU << 17)
#define XHCI_MMIO_VIRTUAL_BASE 0x0000700000000000ULL
#define XHCI_MMIO_PAGE_COUNT 16
#define XHCI_COMMAND_RUN 1U
#define XHCI_COMMAND_RESET 2U
#define XHCI_STATUS_HALTED 1U
#define XHCI_STATUS_NOT_READY (1U << 11)
#define XHCI_TRB_CYCLE 1U
#define XHCI_TRB_TOGGLE_CYCLE (1U << 1)
#define XHCI_TRB_TYPE_LINK (6U << 10)
#define XHCI_TRB_TYPE_NORMAL (1U << 10)
#define XHCI_TRB_TYPE_ENABLE_SLOT (9U << 10)
#define XHCI_TRB_TYPE_DISABLE_SLOT (10U << 10)
#define XHCI_TRB_TYPE_ADDRESS_DEVICE (11U << 10)
#define XHCI_TRB_TYPE_CONFIGURE_ENDPOINT (12U << 10)
#define XHCI_TRB_TYPE_SETUP_STAGE (2U << 10)
#define XHCI_TRB_TYPE_DATA_STAGE (3U << 10)
#define XHCI_TRB_TYPE_STATUS_STAGE (4U << 10)
#define XHCI_TRB_TYPE_TRANSFER_EVENT 32U
#define XHCI_TRB_TYPE_COMMAND_COMPLETION 33U
#define XHCI_COMPLETION_SUCCESS 1U
#define XHCI_COMPLETION_SHORT_PACKET 13U
#define USB_BOT_CBW_SIGNATURE 0x43425355U
#define USB_BOT_CSW_SIGNATURE 0x53425355U
#define USB_BOT_DIRECTION_IN 0x80U
#define USB_BOT_TAG 0x41534153U
#define XHCI_SUPPORTED_DEVICE_CAPACITY 3U

#pragma pack(push, 1)
typedef struct {
    UINT64 parameter;
    UINT32 status;
    UINT32 control;
} XHCI_TRB;

typedef struct {
    UINT64 ring_segment_base;
    UINT32 ring_segment_size;
    UINT32 reserved;
} XHCI_EVENT_RING_SEGMENT;

typedef struct {
    UINT32 signature;
    UINT32 tag;
    UINT32 transfer_length;
    UINT8 flags;
    UINT8 lun;
    UINT8 command_length;
    UINT8 command[16];
} USB_BOT_CBW;

typedef struct {
    UINT32 signature;
    UINT32 tag;
    UINT32 residue;
    UINT8 status;
} USB_BOT_CSW;
#pragma pack(pop)

__declspec(align(64)) static UINT64 device_context_base_array[256];
__declspec(align(4096)) static XHCI_TRB command_ring[256];
__declspec(align(4096)) static XHCI_TRB event_ring[256];
__declspec(align(4096)) static XHCI_TRB endpoint_zero_ring[256];
__declspec(align(4096)) static XHCI_TRB interrupt_in_ring[256];
__declspec(align(64)) static UINT8 hid_keyboard_report[64];
__declspec(align(4096)) static UINT8 input_context[4096];
__declspec(align(4096)) static UINT8 device_context[4096];
__declspec(align(64)) static UINT8 device_descriptor[64];
__declspec(align(64)) static UINT8 configuration_descriptor[256];
__declspec(align(4096)) static XHCI_TRB mouse_endpoint_zero_ring[256];
__declspec(align(4096)) static XHCI_TRB mouse_interrupt_in_ring[256];
__declspec(align(64)) static UINT8 hid_mouse_report[64];
__declspec(align(4096)) static UINT8 mouse_input_context[4096];
__declspec(align(4096)) static UINT8 mouse_device_context[4096];
__declspec(align(64)) static UINT8 mouse_device_descriptor[64];
__declspec(align(64)) static UINT8 mouse_configuration_descriptor[256];
__declspec(align(4096)) static XHCI_TRB storage_endpoint_zero_ring[256];
__declspec(align(4096)) static XHCI_TRB storage_bulk_in_ring[256];
__declspec(align(4096)) static XHCI_TRB storage_bulk_out_ring[256];
__declspec(align(4096)) static UINT8 storage_input_context[4096];
__declspec(align(4096)) static UINT8 storage_device_context[4096];
__declspec(align(64)) static UINT8 storage_device_descriptor[64];
__declspec(align(64)) static UINT8 storage_configuration_descriptor[256];
__declspec(align(64)) static USB_BOT_CBW storage_cbw;
__declspec(align(64)) static USB_BOT_CSW storage_csw;
__declspec(align(64)) static UINT8 storage_inquiry_data[36];
__declspec(align(64)) static UINT8 storage_capacity_data[8];
#define USB_STORAGE_MAX_TRANSFER (64U * 1024U)
#define USB_STORAGE_MIN_BLOCK_SIZE 512U
#define USB_STORAGE_MAX_BLOCK_SIZE 4096U
__declspec(align(4096)) static UINT8 storage_sector_data[USB_STORAGE_MAX_TRANSFER];
__declspec(align(64)) static XHCI_EVENT_RING_SEGMENT event_ring_segment_table[1];
static UINT32 event_dequeue_index;
static UINT32 interrupt_ring_enqueue_index;
static UINT32 mouse_interrupt_ring_enqueue_index;
static UINT32 storage_bulk_in_ring_enqueue_index;
static UINT32 storage_bulk_out_ring_enqueue_index;
static UINT8 storage_bulk_in_cycle = 1;
static UINT8 storage_bulk_out_cycle = 1;
static UINT32 command_ring_enqueue_index;
static ASAS_XHCI_CONTROLLER *active_controller;

static UINT32 xhci_static_dma_bytes(void)
{
    return
        (UINT32)sizeof(device_context_base_array) +
        (UINT32)sizeof(command_ring) +
        (UINT32)sizeof(event_ring) +
        (UINT32)sizeof(endpoint_zero_ring) +
        (UINT32)sizeof(interrupt_in_ring) +
        (UINT32)sizeof(hid_keyboard_report) +
        (UINT32)sizeof(input_context) +
        (UINT32)sizeof(device_context) +
        (UINT32)sizeof(device_descriptor) +
        (UINT32)sizeof(configuration_descriptor) +
        (UINT32)sizeof(mouse_endpoint_zero_ring) +
        (UINT32)sizeof(mouse_interrupt_in_ring) +
        (UINT32)sizeof(hid_mouse_report) +
        (UINT32)sizeof(mouse_input_context) +
        (UINT32)sizeof(mouse_device_context) +
        (UINT32)sizeof(mouse_device_descriptor) +
        (UINT32)sizeof(mouse_configuration_descriptor) +
        (UINT32)sizeof(storage_endpoint_zero_ring) +
        (UINT32)sizeof(storage_bulk_in_ring) +
        (UINT32)sizeof(storage_bulk_out_ring) +
        (UINT32)sizeof(storage_input_context) +
        (UINT32)sizeof(storage_device_context) +
        (UINT32)sizeof(storage_device_descriptor) +
        (UINT32)sizeof(storage_configuration_descriptor) +
        (UINT32)sizeof(storage_cbw) +
        (UINT32)sizeof(storage_csw) +
        (UINT32)sizeof(storage_inquiry_data) +
        (UINT32)sizeof(storage_capacity_data) +
        (UINT32)sizeof(storage_sector_data) +
        (UINT32)sizeof(event_ring_segment_table);
}

static void clear_bytes(void *buffer, UINT32 size)
{
    UINT8 *bytes = (UINT8 *)buffer;
    UINT32 index;

    for (index = 0; index < size; index++) {
        bytes[index] = 0;
    }
}

static void copy_bytes(void *destination, const void *source, UINT32 size)
{
    UINT8 *destination_bytes = (UINT8 *)destination;
    const UINT8 *source_bytes = (const UINT8 *)source;
    UINT32 index;

    for (index = 0; index < size; index++) {
        destination_bytes[index] = source_bytes[index];
    }
}

static UINT32 read_be32(const UINT8 *bytes)
{
    return
        ((UINT32)bytes[0] << 24) |
        ((UINT32)bytes[1] << 16) |
        ((UINT32)bytes[2] << 8) |
        (UINT32)bytes[3];
}

static void write_be32(UINT8 *bytes, UINT32 value)
{
    bytes[0] = (UINT8)(value >> 24);
    bytes[1] = (UINT8)(value >> 16);
    bytes[2] = (UINT8)(value >> 8);
    bytes[3] = (UINT8)value;
}

static int wait_for_clear(volatile UINT32 *register_address, UINT32 mask)
{
    UINT32 timeout;

    for (timeout = 0; timeout < 10000000U; timeout++) {
        if ((*register_address & mask) == 0) {
            return 1;
        }
    }
    return 0;
}

static int wait_for_set(volatile UINT32 *register_address, UINT32 mask)
{
    UINT32 timeout;

    for (timeout = 0; timeout < 10000000U; timeout++) {
        if ((*register_address & mask) != 0) {
            return 1;
        }
    }
    return 0;
}

static int configure_rings(
    volatile UINT8 *capabilities,
    volatile UINT8 *operational,
    UINT8 max_slots
)
{
    UINT32 runtime_offset = *(volatile UINT32 *)(capabilities + 0x18) & ~0x1FU;
    volatile UINT8 *runtime = capabilities + runtime_offset;
    volatile UINT8 *interrupter = runtime + 0x20;

    clear_bytes(device_context_base_array, sizeof(device_context_base_array));
    clear_bytes(command_ring, sizeof(command_ring));
    clear_bytes(event_ring, sizeof(event_ring));
    clear_bytes(event_ring_segment_table, sizeof(event_ring_segment_table));
    event_dequeue_index = 0;
    command_ring_enqueue_index = 0;

    command_ring[255].parameter = (UINT64)(UINTN)&command_ring[0];
    command_ring[255].control = XHCI_TRB_TYPE_LINK | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    event_ring_segment_table[0].ring_segment_base = (UINT64)(UINTN)&event_ring[0];
    event_ring_segment_table[0].ring_segment_size = 256;

    *(volatile UINT64 *)(operational + 0x30) = (UINT64)(UINTN)&device_context_base_array[0];
    *(volatile UINT64 *)(operational + 0x18) = (UINT64)(UINTN)&command_ring[0] | XHCI_TRB_CYCLE;
    *(volatile UINT32 *)(interrupter + 0x08) = 1;
    *(volatile UINT64 *)(interrupter + 0x10) = (UINT64)(UINTN)&event_ring_segment_table[0];
    *(volatile UINT64 *)(interrupter + 0x18) = (UINT64)(UINTN)&event_ring[0];
    *(volatile UINT32 *)(operational + 0x38) = max_slots;
    return 1;
}

static int wait_for_command_completion(volatile UINT8 *capabilities, UINT8 *slot_id)
{
    UINT32 runtime_offset = *(volatile UINT32 *)(capabilities + 0x18) & ~0x1FU;
    volatile UINT8 *interrupter = capabilities + runtime_offset + 0x20;
    UINT32 timeout;

    for (timeout = 0; timeout < 10000000U; timeout++) {
        UINT32 control = event_ring[event_dequeue_index].control;

        if ((control & XHCI_TRB_CYCLE) != 0) {
            UINT32 event_type = (control >> 10) & 0x3F;
            UINT32 completion_code = event_ring[event_dequeue_index].status >> 24;

            event_dequeue_index++;
            *(volatile UINT64 *)(interrupter + 0x18) =
                ((UINT64)(UINTN)&event_ring[event_dequeue_index]) | (1ULL << 3);
            if (event_type == XHCI_TRB_TYPE_COMMAND_COMPLETION) {
                if (completion_code != XHCI_COMPLETION_SUCCESS) {
                    logger_write_hex("ERROR", "xHCI command completion code", completion_code);
                    return 0;
                }
                *slot_id = (UINT8)(control >> 24);
                return 1;
            }
        }
    }
    return 0;
}

static int wait_for_transfer_completion(volatile UINT8 *capabilities)
{
    UINT32 runtime_offset = *(volatile UINT32 *)(capabilities + 0x18) & ~0x1FU;
    volatile UINT8 *interrupter = capabilities + runtime_offset + 0x20;
    UINT32 timeout;

    for (timeout = 0; timeout < 10000000U; timeout++) {
        UINT32 control = event_ring[event_dequeue_index].control;

        if ((control & XHCI_TRB_CYCLE) != 0) {
            UINT32 event_type = (control >> 10) & 0x3F;
            UINT32 completion_code = event_ring[event_dequeue_index].status >> 24;

            event_dequeue_index++;
            *(volatile UINT64 *)(interrupter + 0x18) =
                ((UINT64)(UINTN)&event_ring[event_dequeue_index]) | (1ULL << 3);
            if (event_type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
                if (
                    completion_code != XHCI_COMPLETION_SUCCESS &&
                    completion_code != XHCI_COMPLETION_SHORT_PACKET
                ) {
                    logger_write_hex("ERROR", "xHCI transfer completion code", completion_code);
                    return 0;
                }
                return 1;
            }
        }
    }
    return 0;
}

static int wait_for_storage_transfer_completion(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller)
{
    UINT32 runtime_offset = *(volatile UINT32 *)(capabilities + 0x18) & ~0x1FU;
    volatile UINT8 *interrupter = capabilities + runtime_offset + 0x20;
    UINT32 timeout;
    for (timeout = 0; timeout < 10000000U; timeout++) {
        UINT32 control = event_ring[event_dequeue_index].control;
        if ((control & XHCI_TRB_CYCLE) != 0) {
            UINT32 event_type = (control >> 10) & 0x3F;
            UINT8 slot_id = (UINT8)(control >> 24);
            if (event_type == XHCI_TRB_TYPE_TRANSFER_EVENT &&
                slot_id != controller->storage_enabled_slot &&
                (slot_id == controller->enabled_slot ||
                 slot_id == controller->mouse_enabled_slot)) {
                (void)xhci_poll_devices(controller);
                continue;
            }
            {
                UINT32 completion_code = event_ring[event_dequeue_index].status >> 24;
                event_dequeue_index++;
                *(volatile UINT64 *)(interrupter + 0x18) =
                    ((UINT64)(UINTN)&event_ring[event_dequeue_index]) | (1ULL << 3);
                if (event_type == XHCI_TRB_TYPE_TRANSFER_EVENT &&
                    slot_id == controller->storage_enabled_slot) {
                    if (completion_code != XHCI_COMPLETION_SUCCESS &&
                        completion_code != XHCI_COMPLETION_SHORT_PACKET) {
                        logger_write_hex("ERROR", "xHCI storage completion code",
                                         completion_code);
                        return 0;
                    }
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int enable_slot(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *command_doorbell = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT8 slot_id;
    XHCI_TRB *command = &command_ring[command_ring_enqueue_index++];

    command->parameter = 0;
    command->status = 0;
    command->control = XHCI_TRB_TYPE_ENABLE_SLOT | XHCI_TRB_CYCLE;
    *command_doorbell = 0;

    if (!wait_for_command_completion(capabilities, &slot_id)) {
        return 0;
    }
    controller->enabled_slot = slot_id;
    return controller->enabled_slot != 0;
}

static int disable_slot_id(volatile UINT8 *capabilities, UINT8 slot_id)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *command_doorbell = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT8 completed_slot;
    XHCI_TRB *command;

    if (slot_id == 0) {
        return 1;
    }
    command = &command_ring[command_ring_enqueue_index++];
    command->parameter = 0;
    command->status = 0;
    command->control =
        XHCI_TRB_TYPE_DISABLE_SLOT |
        XHCI_TRB_CYCLE |
        ((UINT32)slot_id << 24);
    *command_doorbell = 0;
    if (!wait_for_command_completion(capabilities, &completed_slot)) {
        return 0;
    }
    device_context_base_array[slot_id] = 0;
    return 1;
}

static int reset_port_number(
    volatile UINT8 *operational,
    UINT8 port_number,
    UINT8 *port_speed
)
{
    volatile UINT32 *port_status;
    UINT32 value;

    if (port_number == 0) {
        return 0;
    }
    port_status = (volatile UINT32 *)(
        operational + XHCI_PORT_REGISTER_BASE +
        (UINT32)(port_number - 1) * XHCI_PORT_REGISTER_STRIDE
    );
    value = *port_status;
    if ((value & XHCI_PORT_CONNECTED) == 0) {
        return 0;
    }
    logger_write_hex("INFO", "xHCI connected port status", value);
    *port_status = (value & ~XHCI_PORT_CHANGE_BITS) | XHCI_PORT_RESET;
    if (
        !wait_for_clear(port_status, XHCI_PORT_RESET) ||
        !wait_for_set(port_status, XHCI_PORT_ENABLED)
    ) {
        logger_write_hex("ERROR", "xHCI port reset status", *port_status);
        return 0;
    }
    value = *port_status;
    *port_speed = (UINT8)((value >> 10) & 0xF);
    return *port_speed != 0;
}

static UINT32 endpoint_zero_max_packet_size(UINT8 speed)
{
    if (speed == 4) {
        return 512;
    }
    if (speed == 3) {
        return 64;
    }
    return 8;
}

static int address_device(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 context_size = (*(volatile UINT32 *)(capabilities + 0x10) & (1U << 2)) != 0 ? 64 : 32;
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *command_doorbell = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT32 *input_control;
    UINT32 *slot_context;
    UINT32 *endpoint_context;
    UINT8 slot_id;

    clear_bytes(input_context, sizeof(input_context));
    clear_bytes(device_context, sizeof(device_context));
    clear_bytes(endpoint_zero_ring, sizeof(endpoint_zero_ring));

    endpoint_zero_ring[255].parameter = (UINT64)(UINTN)&endpoint_zero_ring[0];
    endpoint_zero_ring[255].control = XHCI_TRB_TYPE_LINK | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    input_control = (UINT32 *)&input_context[0];
    slot_context = (UINT32 *)&input_context[context_size];
    endpoint_context = (UINT32 *)&input_context[context_size * 2];

    input_control[1] = 3;
    slot_context[0] =
        ((UINT32)controller->port_speed << 20) |
        (1U << 27);
    slot_context[1] = (UINT32)controller->addressed_port << 16;
    endpoint_context[1] =
        (3U << 1) |
        (4U << 3) |
        (endpoint_zero_max_packet_size(controller->port_speed) << 16);
    endpoint_context[2] = (UINT32)((UINT64)(UINTN)&endpoint_zero_ring[0] | XHCI_TRB_CYCLE);
    endpoint_context[3] = (UINT32)((UINT64)(UINTN)&endpoint_zero_ring[0] >> 32);
    endpoint_context[4] = 8;

    device_context_base_array[controller->enabled_slot] = (UINT64)(UINTN)&device_context[0];
    {
        XHCI_TRB *command_trb = &command_ring[command_ring_enqueue_index++];
        command_trb->parameter = (UINT64)(UINTN)&input_context[0];
        command_trb->status = 0;
        command_trb->control =
        XHCI_TRB_TYPE_ADDRESS_DEVICE |
        XHCI_TRB_CYCLE |
        ((UINT32)controller->enabled_slot << 24);
    }
    *command_doorbell = 0;

    if (!wait_for_command_completion(capabilities, &slot_id)) {
        return 0;
    }
    controller->device_addressed = slot_id == controller->enabled_slot;
    return controller->device_addressed;
}

static int read_device_descriptor(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *doorbells = (volatile UINT32 *)(capabilities + doorbell_offset);

    clear_bytes(device_descriptor, sizeof(device_descriptor));
    endpoint_zero_ring[0].parameter = 0x0012000001000680ULL;
    endpoint_zero_ring[0].status = 8;
    endpoint_zero_ring[0].control =
        XHCI_TRB_TYPE_SETUP_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 6) |
        (3U << 16);
    endpoint_zero_ring[1].parameter = (UINT64)(UINTN)&device_descriptor[0];
    endpoint_zero_ring[1].status = 18;
    endpoint_zero_ring[1].control =
        XHCI_TRB_TYPE_DATA_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 16);
    endpoint_zero_ring[2].parameter = 0;
    endpoint_zero_ring[2].status = 0;
    endpoint_zero_ring[2].control =
        XHCI_TRB_TYPE_STATUS_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 5);
    doorbells[controller->enabled_slot] = 1;

    if (!wait_for_transfer_completion(capabilities)) {
        return 0;
    }
    if (device_descriptor[0] != 18 || device_descriptor[1] != 1) {
        return 0;
    }
    controller->vendor_id = (UINT16)(device_descriptor[8] | ((UINT16)device_descriptor[9] << 8));
    controller->product_id = (UINT16)(device_descriptor[10] | ((UINT16)device_descriptor[11] << 8));
    controller->descriptor_read = 1;
    return 1;
}

static int read_configuration_descriptor(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *doorbells = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT32 index;

    clear_bytes(configuration_descriptor, sizeof(configuration_descriptor));
    endpoint_zero_ring[3].parameter = 0x0100000002000680ULL;
    endpoint_zero_ring[3].status = 8;
    endpoint_zero_ring[3].control =
        XHCI_TRB_TYPE_SETUP_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 6) |
        (3U << 16);
    endpoint_zero_ring[4].parameter = (UINT64)(UINTN)&configuration_descriptor[0];
    endpoint_zero_ring[4].status = sizeof(configuration_descriptor);
    endpoint_zero_ring[4].control =
        XHCI_TRB_TYPE_DATA_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 16);
    endpoint_zero_ring[5].parameter = 0;
    endpoint_zero_ring[5].status = 0;
    endpoint_zero_ring[5].control =
        XHCI_TRB_TYPE_STATUS_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 5);
    doorbells[controller->enabled_slot] = 1;

    if (!wait_for_transfer_completion(capabilities)) {
        return 0;
    }
    if (configuration_descriptor[0] < 9 || configuration_descriptor[1] != 2) {
        return 0;
    }

    controller->interface_class = 0;
    controller->interface_protocol = 0;
    controller->hid_keyboard_detected = 0;
    controller->interrupt_endpoint_address = 0;
    controller->interrupt_endpoint_interval = 0;
    controller->interrupt_endpoint_packet_size = 0;
    index = configuration_descriptor[0];
    {
        int keyboard_interface = 0;

    while (index + 2 <= sizeof(configuration_descriptor) && configuration_descriptor[index] != 0) {
        UINT8 length = configuration_descriptor[index];
        UINT8 type = configuration_descriptor[index + 1];

        if (length < 2 || index + length > sizeof(configuration_descriptor)) {
            break;
        }
        if (type == 4 && length >= 9) {
            controller->interface_class = configuration_descriptor[index + 5];
            controller->interface_protocol = configuration_descriptor[index + 7];
            keyboard_interface =
                controller->interface_class == 3 &&
                controller->interface_protocol == 1;
            if (keyboard_interface) {
                controller->hid_keyboard_detected = 1;
            }
        } else if (
            type == 5 &&
            length >= 7 &&
            keyboard_interface &&
            (configuration_descriptor[index + 2] & 0x80) != 0 &&
            (configuration_descriptor[index + 3] & 3) == 3
        ) {
            controller->interrupt_endpoint_address = configuration_descriptor[index + 2];
            controller->interrupt_endpoint_packet_size = (UINT16)(
                configuration_descriptor[index + 4] |
                ((UINT16)configuration_descriptor[index + 5] << 8)
            );
            controller->interrupt_endpoint_interval = configuration_descriptor[index + 6];
            return 1;
        }
        index += length;
    }
    }
    return 0;
}

static int configure_interrupt_endpoint(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 context_size = (*(volatile UINT32 *)(capabilities + 0x10) & (1U << 2)) != 0 ? 64 : 32;
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *command_doorbell = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT32 endpoint_number = controller->interrupt_endpoint_address & 0x0F;
    UINT32 endpoint_context_index = endpoint_number * 2 + 1;
    UINT32 *input_control;
    UINT32 *slot_context;
    UINT32 *endpoint_context;
    UINT8 slot_id;

    if (
        endpoint_number == 0 ||
        controller->interrupt_endpoint_packet_size == 0 ||
        endpoint_context_index >= 32
    ) {
        return 0;
    }

    clear_bytes(input_context, sizeof(input_context));
    clear_bytes(interrupt_in_ring, sizeof(interrupt_in_ring));
    interrupt_in_ring[255].parameter = (UINT64)(UINTN)&interrupt_in_ring[0];
    interrupt_in_ring[255].control = XHCI_TRB_TYPE_LINK | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;

    input_control = (UINT32 *)&input_context[0];
    slot_context = (UINT32 *)&input_context[context_size];
    endpoint_context = (UINT32 *)&input_context[context_size * (endpoint_context_index + 1)];
    copy_bytes(slot_context, device_context, context_size);
    input_control[1] = 1U | (1U << endpoint_context_index);
    slot_context[0] &= ~(0x1FU << 27);
    slot_context[0] |= endpoint_context_index << 27;
    endpoint_context[0] = (UINT32)controller->interrupt_endpoint_interval << 16;
    endpoint_context[1] =
        (3U << 1) |
        (7U << 3) |
        ((UINT32)controller->interrupt_endpoint_packet_size << 16);
    endpoint_context[2] = (UINT32)((UINT64)(UINTN)&interrupt_in_ring[0] | XHCI_TRB_CYCLE);
    endpoint_context[3] = (UINT32)((UINT64)(UINTN)&interrupt_in_ring[0] >> 32);
    endpoint_context[4] = controller->interrupt_endpoint_packet_size;

    {
        XHCI_TRB *command_trb = &command_ring[command_ring_enqueue_index++];
        command_trb->parameter = (UINT64)(UINTN)&input_context[0];
        command_trb->status = 0;
        command_trb->control =
        XHCI_TRB_TYPE_CONFIGURE_ENDPOINT |
        XHCI_TRB_CYCLE |
        ((UINT32)controller->enabled_slot << 24);
    }
    *command_doorbell = 0;

    if (!wait_for_command_completion(capabilities, &slot_id)) {
        return 0;
    }
    controller->interrupt_endpoint_configured = slot_id == controller->enabled_slot;
    return controller->interrupt_endpoint_configured;
}

static int set_usb_configuration(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *doorbells = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT8 configuration_value = configuration_descriptor[5];

    endpoint_zero_ring[6].parameter = 0x0000000000000900ULL | ((UINT64)configuration_value << 16);
    endpoint_zero_ring[6].status = 8;
    endpoint_zero_ring[6].control =
        XHCI_TRB_TYPE_SETUP_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 6);
    endpoint_zero_ring[7].parameter = 0;
    endpoint_zero_ring[7].status = 0;
    endpoint_zero_ring[7].control =
        XHCI_TRB_TYPE_STATUS_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 5) |
        (1U << 16);
    doorbells[controller->enabled_slot] = 1;

    if (!wait_for_transfer_completion(capabilities)) {
        return 0;
    }
    controller->configuration_set = 1;
    return 1;
}

static int queue_hid_keyboard_report(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *doorbells = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT32 endpoint_number = controller->interrupt_endpoint_address & 0x0F;
    UINT32 endpoint_context_index = endpoint_number * 2 + 1;

    clear_bytes(hid_keyboard_report, sizeof(hid_keyboard_report));
    interrupt_ring_enqueue_index = 0;
    interrupt_in_ring[interrupt_ring_enqueue_index].parameter = (UINT64)(UINTN)&hid_keyboard_report[0];
    interrupt_in_ring[interrupt_ring_enqueue_index].status = controller->interrupt_endpoint_packet_size;
    interrupt_in_ring[interrupt_ring_enqueue_index].control =
        XHCI_TRB_TYPE_NORMAL |
        XHCI_TRB_CYCLE |
        (1U << 5);
    interrupt_ring_enqueue_index++;
    doorbells[controller->enabled_slot] = endpoint_context_index;
    controller->hid_report_queued = 1;
    return 1;
}

static int initialize_keyboard_device(
    volatile UINT8 *capabilities,
    volatile UINT8 *operational,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT8 port_number;

    for (port_number = 1; port_number <= controller->max_ports; port_number++) {
        UINT8 port_speed;

        controller->addressed_port = port_number;
        if (!reset_port_number(operational, port_number, &port_speed)) {
            continue;
        }
        controller->port_speed = port_speed;
        controller->enabled_slot = 0;
        controller->device_addressed = 0;
        controller->descriptor_read = 0;
        controller->hid_keyboard_detected = 0;
        controller->interrupt_endpoint_address = 0;
        controller->interrupt_endpoint_packet_size = 0;
        controller->interrupt_endpoint_configured = 0;
        controller->configuration_set = 0;
        controller->hid_report_queued = 0;

        if (
            enable_slot(capabilities, controller) &&
            address_device(capabilities, controller) &&
            read_device_descriptor(capabilities, controller) &&
            read_configuration_descriptor(capabilities, controller) &&
            set_usb_configuration(capabilities, controller) &&
            configure_interrupt_endpoint(capabilities, controller) &&
            queue_hid_keyboard_report(capabilities, controller)
        ) {
            return 1;
        }
        (void)disable_slot_id(capabilities, controller->enabled_slot);
    }
    controller->addressed_port = 0;
    return 0;
}

static int enable_mouse_slot(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *command_doorbell = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT8 slot_id;
    XHCI_TRB *command = &command_ring[command_ring_enqueue_index++];

    command->parameter = 0;
    command->status = 0;
    command->control = XHCI_TRB_TYPE_ENABLE_SLOT | XHCI_TRB_CYCLE;
    *command_doorbell = 0;
    if (!wait_for_command_completion(capabilities, &slot_id)) {
        return 0;
    }
    controller->mouse_enabled_slot = slot_id;
    return slot_id != 0;
}

static int address_mouse_device(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller,
    UINT8 port_speed
)
{
    UINT32 context_size = (*(volatile UINT32 *)(capabilities + 0x10) & (1U << 2)) != 0 ? 64 : 32;
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *command_doorbell = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT32 *input_control;
    UINT32 *slot_context;
    UINT32 *endpoint_context;
    UINT8 slot_id;
    XHCI_TRB *command = &command_ring[command_ring_enqueue_index++];

    clear_bytes(mouse_input_context, sizeof(mouse_input_context));
    clear_bytes(mouse_device_context, sizeof(mouse_device_context));
    clear_bytes(mouse_endpoint_zero_ring, sizeof(mouse_endpoint_zero_ring));
    mouse_endpoint_zero_ring[255].parameter = (UINT64)(UINTN)&mouse_endpoint_zero_ring[0];
    mouse_endpoint_zero_ring[255].control =
        XHCI_TRB_TYPE_LINK | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    input_control = (UINT32 *)&mouse_input_context[0];
    slot_context = (UINT32 *)&mouse_input_context[context_size];
    endpoint_context = (UINT32 *)&mouse_input_context[context_size * 2];
    input_control[1] = 3;
    slot_context[0] = ((UINT32)port_speed << 20) | (1U << 27);
    slot_context[1] = (UINT32)controller->mouse_addressed_port << 16;
    endpoint_context[1] =
        (3U << 1) | (4U << 3) | (endpoint_zero_max_packet_size(port_speed) << 16);
    endpoint_context[2] =
        (UINT32)((UINT64)(UINTN)&mouse_endpoint_zero_ring[0] | XHCI_TRB_CYCLE);
    endpoint_context[3] = (UINT32)((UINT64)(UINTN)&mouse_endpoint_zero_ring[0] >> 32);
    endpoint_context[4] = 8;
    device_context_base_array[controller->mouse_enabled_slot] =
        (UINT64)(UINTN)&mouse_device_context[0];
    command->parameter = (UINT64)(UINTN)&mouse_input_context[0];
    command->control =
        XHCI_TRB_TYPE_ADDRESS_DEVICE |
        XHCI_TRB_CYCLE |
        ((UINT32)controller->mouse_enabled_slot << 24);
    *command_doorbell = 0;
    return
        wait_for_command_completion(capabilities, &slot_id) &&
        slot_id == controller->mouse_enabled_slot;
}

static int mouse_control_transfer(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller,
    UINT32 ring_index,
    UINT64 setup,
    void *buffer,
    UINT32 length,
    int data_in
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *doorbells = (volatile UINT32 *)(capabilities + doorbell_offset);

    mouse_endpoint_zero_ring[ring_index].parameter = setup;
    mouse_endpoint_zero_ring[ring_index].status = 8;
    mouse_endpoint_zero_ring[ring_index].control =
        XHCI_TRB_TYPE_SETUP_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 6) |
        (buffer != 0 ? (3U << 16) : 0);
    if (buffer != 0) {
        mouse_endpoint_zero_ring[ring_index + 1].parameter = (UINT64)(UINTN)buffer;
        mouse_endpoint_zero_ring[ring_index + 1].status = length;
        mouse_endpoint_zero_ring[ring_index + 1].control =
            XHCI_TRB_TYPE_DATA_STAGE | XHCI_TRB_CYCLE | (data_in ? (1U << 16) : 0);
        ring_index++;
    }
    mouse_endpoint_zero_ring[ring_index + 1].control =
        XHCI_TRB_TYPE_STATUS_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 5) |
        (data_in ? 0 : (1U << 16));
    doorbells[controller->mouse_enabled_slot] = 1;
    return wait_for_transfer_completion(capabilities);
}

static int read_mouse_descriptors(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 index;
    int mouse_interface = 0;

    clear_bytes(mouse_device_descriptor, sizeof(mouse_device_descriptor));
    if (!mouse_control_transfer(
        capabilities, controller, 0, 0x0012000001000680ULL,
        mouse_device_descriptor, 18, 1
    )) {
        return 0;
    }
    clear_bytes(mouse_configuration_descriptor, sizeof(mouse_configuration_descriptor));
    if (!mouse_control_transfer(
        capabilities, controller, 3, 0x0100000002000680ULL,
        mouse_configuration_descriptor, sizeof(mouse_configuration_descriptor), 1
    )) {
        return 0;
    }
    index = mouse_configuration_descriptor[0];
    while (
        index + 2 <= sizeof(mouse_configuration_descriptor) &&
        mouse_configuration_descriptor[index] != 0
    ) {
        UINT8 length = mouse_configuration_descriptor[index];
        UINT8 type = mouse_configuration_descriptor[index + 1];

        if (length < 2 || index + length > sizeof(mouse_configuration_descriptor)) {
            break;
        }
        if (type == 4 && length >= 9) {
            mouse_interface =
                mouse_configuration_descriptor[index + 5] == 3 &&
                mouse_configuration_descriptor[index + 7] == 2;
            if (mouse_interface) {
                controller->hid_mouse_detected = 1;
            }
        } else if (
            type == 5 && length >= 7 && mouse_interface &&
            (mouse_configuration_descriptor[index + 2] & 0x80) != 0 &&
            (mouse_configuration_descriptor[index + 3] & 3) == 3
        ) {
            controller->mouse_interrupt_endpoint_address =
                mouse_configuration_descriptor[index + 2];
            controller->mouse_interrupt_endpoint_packet_size = (UINT16)(
                mouse_configuration_descriptor[index + 4] |
                ((UINT16)mouse_configuration_descriptor[index + 5] << 8)
            );
            return 1;
        }
        index += length;
    }
    return 0;
}

static int configure_mouse_endpoint(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 context_size = (*(volatile UINT32 *)(capabilities + 0x10) & (1U << 2)) != 0 ? 64 : 32;
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *command_doorbell = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT32 endpoint_number = controller->mouse_interrupt_endpoint_address & 0x0F;
    UINT32 endpoint_context_index = endpoint_number * 2 + 1;
    UINT32 *input_control;
    UINT32 *slot_context;
    UINT32 *endpoint_context;
    UINT8 slot_id;
    XHCI_TRB *command = &command_ring[command_ring_enqueue_index++];

    clear_bytes(mouse_input_context, sizeof(mouse_input_context));
    clear_bytes(mouse_interrupt_in_ring, sizeof(mouse_interrupt_in_ring));
    mouse_interrupt_in_ring[255].parameter = (UINT64)(UINTN)&mouse_interrupt_in_ring[0];
    mouse_interrupt_in_ring[255].control =
        XHCI_TRB_TYPE_LINK | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    input_control = (UINT32 *)&mouse_input_context[0];
    slot_context = (UINT32 *)&mouse_input_context[context_size];
    endpoint_context = (UINT32 *)&mouse_input_context[context_size * (endpoint_context_index + 1)];
    copy_bytes(slot_context, mouse_device_context, context_size);
    input_control[1] = 1U | (1U << endpoint_context_index);
    slot_context[0] &= ~(0x1FU << 27);
    slot_context[0] |= endpoint_context_index << 27;
    endpoint_context[0] = 10U << 16;
    endpoint_context[1] =
        (3U << 1) |
        (7U << 3) |
        ((UINT32)controller->mouse_interrupt_endpoint_packet_size << 16);
    endpoint_context[2] =
        (UINT32)((UINT64)(UINTN)&mouse_interrupt_in_ring[0] | XHCI_TRB_CYCLE);
    endpoint_context[3] = (UINT32)((UINT64)(UINTN)&mouse_interrupt_in_ring[0] >> 32);
    endpoint_context[4] = controller->mouse_interrupt_endpoint_packet_size;
    command->parameter = (UINT64)(UINTN)&mouse_input_context[0];
    command->control =
        XHCI_TRB_TYPE_CONFIGURE_ENDPOINT |
        XHCI_TRB_CYCLE |
        ((UINT32)controller->mouse_enabled_slot << 24);
    *command_doorbell = 0;
    if (!wait_for_command_completion(capabilities, &slot_id)) {
        return 0;
    }
    controller->mouse_interrupt_endpoint_configured =
        slot_id == controller->mouse_enabled_slot;
    return controller->mouse_interrupt_endpoint_configured;
}

static int initialize_mouse_device(
    volatile UINT8 *capabilities,
    volatile UINT8 *operational,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *doorbells = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT8 port_number;

    for (port_number = 1; port_number <= controller->max_ports; port_number++) {
        UINT32 endpoint_context_index;
        UINT8 port_speed;
        UINT8 configuration_value;

        if (port_number == controller->addressed_port) {
            continue;
        }
        controller->mouse_addressed_port = port_number;
        controller->mouse_enabled_slot = 0;
        controller->hid_mouse_detected = 0;
        controller->mouse_interrupt_endpoint_address = 0;
        controller->mouse_interrupt_endpoint_packet_size = 0;
        controller->mouse_interrupt_endpoint_configured = 0;
        controller->mouse_report_queued = 0;

        if (
            !reset_port_number(operational, port_number, &port_speed) ||
            !enable_mouse_slot(capabilities, controller) ||
            !address_mouse_device(capabilities, controller, port_speed) ||
            !read_mouse_descriptors(capabilities, controller)
        ) {
            (void)disable_slot_id(capabilities, controller->mouse_enabled_slot);
            continue;
        }
        configuration_value = mouse_configuration_descriptor[5];
        if (
            !mouse_control_transfer(
                capabilities, controller, 6,
                0x0000000000000900ULL | ((UINT64)configuration_value << 16),
                0, 0, 0
            ) ||
            !configure_mouse_endpoint(capabilities, controller)
        ) {
            (void)disable_slot_id(capabilities, controller->mouse_enabled_slot);
            continue;
        }
        clear_bytes(hid_mouse_report, sizeof(hid_mouse_report));
        mouse_interrupt_ring_enqueue_index = 0;
        mouse_interrupt_in_ring[0].parameter = (UINT64)(UINTN)&hid_mouse_report[0];
        mouse_interrupt_in_ring[0].status = controller->mouse_interrupt_endpoint_packet_size;
        mouse_interrupt_in_ring[0].control =
            XHCI_TRB_TYPE_NORMAL | XHCI_TRB_CYCLE | (1U << 5);
        mouse_interrupt_ring_enqueue_index = 1;
        endpoint_context_index =
            (controller->mouse_interrupt_endpoint_address & 0x0F) * 2 + 1;
        doorbells[controller->mouse_enabled_slot] = endpoint_context_index;
        controller->mouse_report_queued = 1;
        return 1;
    }
    controller->mouse_addressed_port = 0;
    return 0;
}

static int enable_storage_slot(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *command_doorbell = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT8 slot_id;
    XHCI_TRB *command = &command_ring[command_ring_enqueue_index++];

    command->parameter = 0;
    command->status = 0;
    command->control = XHCI_TRB_TYPE_ENABLE_SLOT | XHCI_TRB_CYCLE;
    *command_doorbell = 0;
    if (!wait_for_command_completion(capabilities, &slot_id)) {
        return 0;
    }
    controller->storage_enabled_slot = slot_id;
    return slot_id != 0;
}

static int address_storage_device(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller,
    UINT8 port_speed
)
{
    UINT32 context_size = (*(volatile UINT32 *)(capabilities + 0x10) & (1U << 2)) != 0 ? 64 : 32;
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *command_doorbell = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT32 *input_control;
    UINT32 *slot_context;
    UINT32 *endpoint_context;
    UINT8 slot_id;
    XHCI_TRB *command = &command_ring[command_ring_enqueue_index++];

    clear_bytes(storage_input_context, sizeof(storage_input_context));
    clear_bytes(storage_device_context, sizeof(storage_device_context));
    clear_bytes(storage_endpoint_zero_ring, sizeof(storage_endpoint_zero_ring));
    storage_endpoint_zero_ring[255].parameter = (UINT64)(UINTN)&storage_endpoint_zero_ring[0];
    storage_endpoint_zero_ring[255].control =
        XHCI_TRB_TYPE_LINK | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    input_control = (UINT32 *)&storage_input_context[0];
    slot_context = (UINT32 *)&storage_input_context[context_size];
    endpoint_context = (UINT32 *)&storage_input_context[context_size * 2];
    input_control[1] = 3;
    slot_context[0] = ((UINT32)port_speed << 20) | (1U << 27);
    slot_context[1] = (UINT32)controller->storage_addressed_port << 16;
    endpoint_context[1] =
        (3U << 1) | (4U << 3) | (endpoint_zero_max_packet_size(port_speed) << 16);
    endpoint_context[2] =
        (UINT32)((UINT64)(UINTN)&storage_endpoint_zero_ring[0] | XHCI_TRB_CYCLE);
    endpoint_context[3] = (UINT32)((UINT64)(UINTN)&storage_endpoint_zero_ring[0] >> 32);
    endpoint_context[4] = 8;
    device_context_base_array[controller->storage_enabled_slot] =
        (UINT64)(UINTN)&storage_device_context[0];
    command->parameter = (UINT64)(UINTN)&storage_input_context[0];
    command->control =
        XHCI_TRB_TYPE_ADDRESS_DEVICE |
        XHCI_TRB_CYCLE |
        ((UINT32)controller->storage_enabled_slot << 24);
    *command_doorbell = 0;
    return
        wait_for_command_completion(capabilities, &slot_id) &&
        slot_id == controller->storage_enabled_slot;
}

static int storage_control_transfer(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller,
    UINT32 ring_index,
    UINT64 setup,
    void *buffer,
    UINT32 length,
    int data_in
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *doorbells = (volatile UINT32 *)(capabilities + doorbell_offset);

    storage_endpoint_zero_ring[ring_index].parameter = setup;
    storage_endpoint_zero_ring[ring_index].status = 8;
    storage_endpoint_zero_ring[ring_index].control =
        XHCI_TRB_TYPE_SETUP_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 6) |
        (buffer != 0 ? (3U << 16) : 0);
    if (buffer != 0) {
        storage_endpoint_zero_ring[ring_index + 1].parameter = (UINT64)(UINTN)buffer;
        storage_endpoint_zero_ring[ring_index + 1].status = length;
        storage_endpoint_zero_ring[ring_index + 1].control =
            XHCI_TRB_TYPE_DATA_STAGE | XHCI_TRB_CYCLE | (data_in ? (1U << 16) : 0);
        ring_index++;
    }
    storage_endpoint_zero_ring[ring_index + 1].control =
        XHCI_TRB_TYPE_STATUS_STAGE |
        XHCI_TRB_CYCLE |
        (1U << 5) |
        (data_in ? 0 : (1U << 16));
    doorbells[controller->storage_enabled_slot] = 1;
    return wait_for_transfer_completion(capabilities);
}

static int read_storage_descriptors(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 index;
    int storage_interface = 0;

    clear_bytes(storage_device_descriptor, sizeof(storage_device_descriptor));
    if (!storage_control_transfer(
        capabilities, controller, 0, 0x0012000001000680ULL,
        storage_device_descriptor, 18, 1
    )) {
        return 0;
    }
    clear_bytes(storage_configuration_descriptor, sizeof(storage_configuration_descriptor));
    if (!storage_control_transfer(
        capabilities, controller, 3, 0x0100000002000680ULL,
        storage_configuration_descriptor, sizeof(storage_configuration_descriptor), 1
    )) {
        return 0;
    }
    index = storage_configuration_descriptor[0];
    while (
        index + 2 <= sizeof(storage_configuration_descriptor) &&
        storage_configuration_descriptor[index] != 0
    ) {
        UINT8 length = storage_configuration_descriptor[index];
        UINT8 type = storage_configuration_descriptor[index + 1];

        if (length < 2 || index + length > sizeof(storage_configuration_descriptor)) {
            break;
        }
        if (type == 4 && length >= 9) {
            storage_interface =
                storage_configuration_descriptor[index + 5] == 8 &&
                storage_configuration_descriptor[index + 6] == 6 &&
                storage_configuration_descriptor[index + 7] == 0x50;
            if (storage_interface) {
                controller->usb_storage_detected = 1;
            }
        } else if (type == 5 && length >= 7 && storage_interface) {
            UINT8 endpoint_address = storage_configuration_descriptor[index + 2];
            UINT8 attributes = storage_configuration_descriptor[index + 3] & 3;
            UINT16 packet_size = (UINT16)(
                storage_configuration_descriptor[index + 4] |
                ((UINT16)storage_configuration_descriptor[index + 5] << 8)
            );

            if (attributes == 2 && (endpoint_address & 0x80) != 0) {
                controller->storage_bulk_in_endpoint_address = endpoint_address;
                controller->storage_bulk_in_packet_size = packet_size;
            } else if (attributes == 2) {
                controller->storage_bulk_out_endpoint_address = endpoint_address;
                controller->storage_bulk_out_packet_size = packet_size;
            }
        }
        index += length;
    }
    return
        controller->usb_storage_detected &&
        controller->storage_bulk_in_endpoint_address != 0 &&
        controller->storage_bulk_out_endpoint_address != 0;
}

static int configure_storage_bulk_endpoints(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT32 context_size = (*(volatile UINT32 *)(capabilities + 0x10) & (1U << 2)) != 0 ? 64 : 32;
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *command_doorbell = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT32 in_endpoint_number = controller->storage_bulk_in_endpoint_address & 0x0F;
    UINT32 out_endpoint_number = controller->storage_bulk_out_endpoint_address & 0x0F;
    UINT32 in_context_index = in_endpoint_number * 2 + 1;
    UINT32 out_context_index = out_endpoint_number * 2;
    UINT32 max_context_index = in_context_index > out_context_index ? in_context_index : out_context_index;
    UINT32 *input_control;
    UINT32 *slot_context;
    UINT32 *endpoint_context;
    UINT8 slot_id;
    XHCI_TRB *command = &command_ring[command_ring_enqueue_index++];

    if (
        in_endpoint_number == 0 ||
        out_endpoint_number == 0 ||
        in_context_index >= 32 ||
        out_context_index >= 32
    ) {
        return 0;
    }
    clear_bytes(storage_input_context, sizeof(storage_input_context));
    clear_bytes(storage_bulk_in_ring, sizeof(storage_bulk_in_ring));
    clear_bytes(storage_bulk_out_ring, sizeof(storage_bulk_out_ring));
    storage_bulk_in_ring_enqueue_index = 0;
    storage_bulk_out_ring_enqueue_index = 0;
    storage_bulk_in_cycle = 1;
    storage_bulk_out_cycle = 1;
    storage_bulk_in_ring[255].parameter = (UINT64)(UINTN)&storage_bulk_in_ring[0];
    storage_bulk_in_ring[255].control =
        XHCI_TRB_TYPE_LINK | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    storage_bulk_out_ring[255].parameter = (UINT64)(UINTN)&storage_bulk_out_ring[0];
    storage_bulk_out_ring[255].control =
        XHCI_TRB_TYPE_LINK | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    input_control = (UINT32 *)&storage_input_context[0];
    slot_context = (UINT32 *)&storage_input_context[context_size];
    copy_bytes(slot_context, storage_device_context, context_size);
    input_control[1] = 1U | (1U << in_context_index) | (1U << out_context_index);
    slot_context[0] &= ~(0x1FU << 27);
    slot_context[0] |= max_context_index << 27;

    endpoint_context = (UINT32 *)&storage_input_context[context_size * (out_context_index + 1)];
    endpoint_context[1] =
        (3U << 1) |
        (2U << 3) |
        ((UINT32)controller->storage_bulk_out_packet_size << 16);
    endpoint_context[2] =
        (UINT32)((UINT64)(UINTN)&storage_bulk_out_ring[0] | XHCI_TRB_CYCLE);
    endpoint_context[3] = (UINT32)((UINT64)(UINTN)&storage_bulk_out_ring[0] >> 32);
    endpoint_context[4] = controller->storage_bulk_out_packet_size;

    endpoint_context = (UINT32 *)&storage_input_context[context_size * (in_context_index + 1)];
    endpoint_context[1] =
        (3U << 1) |
        (6U << 3) |
        ((UINT32)controller->storage_bulk_in_packet_size << 16);
    endpoint_context[2] =
        (UINT32)((UINT64)(UINTN)&storage_bulk_in_ring[0] | XHCI_TRB_CYCLE);
    endpoint_context[3] = (UINT32)((UINT64)(UINTN)&storage_bulk_in_ring[0] >> 32);
    endpoint_context[4] = controller->storage_bulk_in_packet_size;

    command->parameter = (UINT64)(UINTN)&storage_input_context[0];
    command->control =
        XHCI_TRB_TYPE_CONFIGURE_ENDPOINT |
        XHCI_TRB_CYCLE |
        ((UINT32)controller->storage_enabled_slot << 24);
    *command_doorbell = 0;
    if (!wait_for_command_completion(capabilities, &slot_id)) {
        return 0;
    }
    controller->storage_bulk_endpoints_configured =
        slot_id == controller->storage_enabled_slot;
    return controller->storage_bulk_endpoints_configured;
}

static int storage_bulk_transfer(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller,
    void *buffer,
    UINT32 length,
    int data_in
)
{
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT32 *doorbells = (volatile UINT32 *)(capabilities + doorbell_offset);
    XHCI_TRB *ring;
    UINT32 *enqueue_index;
    UINT8 *cycle;
    UINT32 endpoint_context_index;

    if (length == 0 || buffer == 0) {
        return 1;
    }
    if (data_in) {
        UINT32 endpoint_number = controller->storage_bulk_in_endpoint_address & 0x0F;

        ring = storage_bulk_in_ring;
        enqueue_index = &storage_bulk_in_ring_enqueue_index;
        cycle = &storage_bulk_in_cycle;
        endpoint_context_index = endpoint_number * 2 + 1;
    } else {
        UINT32 endpoint_number = controller->storage_bulk_out_endpoint_address & 0x0F;

        ring = storage_bulk_out_ring;
        enqueue_index = &storage_bulk_out_ring_enqueue_index;
        cycle = &storage_bulk_out_cycle;
        endpoint_context_index = endpoint_number * 2;
    }
    ring[*enqueue_index].parameter = (UINT64)(UINTN)buffer;
    ring[*enqueue_index].status = length;
    ring[*enqueue_index].control =
        XHCI_TRB_TYPE_NORMAL |
        (*cycle ? XHCI_TRB_CYCLE : 0) |
        (1U << 5);
    if (*enqueue_index == 254) {
        ring[255].control = XHCI_TRB_TYPE_LINK | XHCI_TRB_TOGGLE_CYCLE |
                            (*cycle ? XHCI_TRB_CYCLE : 0);
    }
    (*enqueue_index)++;
    doorbells[controller->storage_enabled_slot] = endpoint_context_index;
    if (!wait_for_storage_transfer_completion(capabilities, controller)) return 0;
    if (*enqueue_index == 255) {
        *enqueue_index = 0;
        *cycle ^= 1U;
    }
    return 1;
}

static int storage_bot_command(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller,
    const UINT8 *command,
    UINT8 command_length,
    void *data,
    UINT32 data_length,
    int data_in
)
{
    clear_bytes(&storage_cbw, sizeof(storage_cbw));
    clear_bytes(&storage_csw, sizeof(storage_csw));
    storage_cbw.signature = USB_BOT_CBW_SIGNATURE;
    storage_cbw.tag = USB_BOT_TAG;
    storage_cbw.transfer_length = data_length;
    storage_cbw.flags = data_in ? USB_BOT_DIRECTION_IN : 0;
    storage_cbw.lun = 0;
    storage_cbw.command_length = command_length;
    copy_bytes(storage_cbw.command, command, command_length);

    if (!storage_bulk_transfer(capabilities, controller, &storage_cbw, sizeof(storage_cbw), 0)) {
        return 0;
    }
    if (data_length != 0 && !storage_bulk_transfer(capabilities, controller, data, data_length, data_in)) {
        return 0;
    }
    if (!storage_bulk_transfer(capabilities, controller, &storage_csw, sizeof(storage_csw), 1)) {
        return 0;
    }
    if (
        storage_csw.signature != USB_BOT_CSW_SIGNATURE ||
        storage_csw.tag != USB_BOT_TAG ||
        storage_csw.status != 0
    ) {
        logger_write_hex("ERROR", "USB Mass Storage CSW signature", storage_csw.signature);
        logger_write_hex("ERROR", "USB Mass Storage CSW status", storage_csw.status);
        return 0;
    }
    return 1;
}

static int initialize_storage_scsi(
    volatile UINT8 *capabilities,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT8 inquiry_command[6] = { 0x12, 0, 0, 0, sizeof(storage_inquiry_data), 0 };
    UINT8 read_capacity_command[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    UINT8 mode_sense_command[6] = { 0x1A, 0, 0x3F, 0, 4, 0 };
    UINT8 mode_sense_data[4];
    UINT8 read_sector_command[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0 };
    UINT32 last_lba;

    clear_bytes(storage_inquiry_data, sizeof(storage_inquiry_data));
    if (!storage_bot_command(
        capabilities,
        controller,
        inquiry_command,
        sizeof(inquiry_command),
        storage_inquiry_data,
        sizeof(storage_inquiry_data),
        1
    )) {
        return 0;
    }
    controller->storage_inquiry_completed = 1;

    clear_bytes(storage_capacity_data, sizeof(storage_capacity_data));
    if (!storage_bot_command(
        capabilities,
        controller,
        read_capacity_command,
        sizeof(read_capacity_command),
        storage_capacity_data,
        sizeof(storage_capacity_data),
        1
    )) {
        return 0;
    }
    last_lba = read_be32(storage_capacity_data);
    controller->storage_sector_count = last_lba + 1;
    controller->storage_block_size = read_be32(storage_capacity_data + 4);
    if (controller->storage_block_size < USB_STORAGE_MIN_BLOCK_SIZE ||
        controller->storage_block_size > USB_STORAGE_MAX_BLOCK_SIZE ||
        (controller->storage_block_size &
         (controller->storage_block_size - 1U)) != 0) {
        logger_write_hex("ERROR", "USB Mass Storage block size", controller->storage_block_size);
        return 0;
    }
    controller->storage_capacity_read = 1;

    clear_bytes(mode_sense_data, sizeof(mode_sense_data));
    controller->storage_read_only = 0;
    if (storage_bot_command(
        capabilities, controller, mode_sense_command, sizeof(mode_sense_command),
        mode_sense_data, sizeof(mode_sense_data), 1
    )) {
        controller->storage_read_only = (mode_sense_data[2] & 0x80U) != 0;
    }

    clear_bytes(storage_sector_data, sizeof(storage_sector_data));
    write_be32(&read_sector_command[2], 0);
    if (!storage_bot_command(
        capabilities,
        controller,
        read_sector_command,
        sizeof(read_sector_command),
        storage_sector_data,
        controller->storage_block_size,
        1
    )) {
        return 0;
    }
    if (storage_sector_data[510] != 0x55 || storage_sector_data[511] != 0xAA) {
        logger_write_hex("ERROR", "USB Mass Storage boot signature", storage_sector_data[510]);
        logger_write_hex("ERROR", "USB Mass Storage boot signature", storage_sector_data[511]);
        return 0;
    }
    controller->storage_sector_read = 1;
    return 1;
}

static int initialize_storage_device(
    volatile UINT8 *capabilities,
    volatile UINT8 *operational,
    ASAS_XHCI_CONTROLLER *controller
)
{
    UINT8 port_number;

    for (port_number = 1; port_number <= controller->max_ports; port_number++) {
        UINT8 port_speed;
        if (
            port_number == controller->addressed_port ||
            port_number == controller->mouse_addressed_port
        ) {
            continue;
        }
        controller->storage_addressed_port = port_number;
        controller->storage_enabled_slot = 0;
        controller->usb_storage_detected = 0;
        controller->storage_bulk_in_endpoint_address = 0;
        controller->storage_bulk_out_endpoint_address = 0;
        controller->storage_bulk_in_packet_size = 0;
        controller->storage_bulk_out_packet_size = 0;
        controller->storage_bulk_endpoints_configured = 0;
        controller->storage_inquiry_completed = 0;
        controller->storage_capacity_read = 0;
        controller->storage_sector_read = 0;
        controller->storage_block_size = 0;
        controller->storage_sector_count = 0;
        controller->storage_read_only = 0;

        if (
            !reset_port_number(operational, port_number, &port_speed) ||
            !enable_storage_slot(capabilities, controller) ||
            !address_storage_device(capabilities, controller, port_speed)
        ) {
            (void)disable_slot_id(capabilities, controller->storage_enabled_slot);
            continue;
        }
        if (!read_storage_descriptors(capabilities, controller)) {
            (void)disable_slot_id(capabilities, controller->storage_enabled_slot);
            continue;
        }
        if (
            configure_storage_bulk_endpoints(capabilities, controller) &&
            storage_control_transfer(
                capabilities,
                controller,
                6,
                0x0000000000000900ULL |
                    ((UINT64)storage_configuration_descriptor[5] << 16),
                0,
                0,
                0
            )
        ) {
            if (initialize_storage_scsi(capabilities, controller)) {
                return 1;
            }
        }
        (void)disable_slot_id(capabilities, controller->storage_enabled_slot);
    }
    controller->storage_addressed_port = 0;
    return 0;
}

static char hid_keycode_to_character(UINT8 keycode, UINT8 modifiers)
{
    static const char normal[] = {
        0,0,0,0,'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p',
        'q','r','s','t','u','v','w','x','y','z','1','2','3','4','5','6','7','8','9',
        '0','\n','\b','\t',' ','-','=','[',']','\\',0,';','\'','`',',','.','/'
    };
    char value;

    if (keycode >= sizeof(normal)) {
        return 0;
    }
    value = normal[keycode];
    if ((modifiers & 0x22) != 0 && value >= 'a' && value <= 'z') {
        value = (char)(value - 'a' + 'A');
    }
    return value;
}

int xhci_poll_keyboard(ASAS_XHCI_CONTROLLER *controller)
{
    volatile UINT8 *capabilities = (volatile UINT8 *)(UINTN)controller->mmio_base;
    UINT32 runtime_offset = *(volatile UINT32 *)(capabilities + 0x18) & ~0x1FU;
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT8 *interrupter = capabilities + runtime_offset + 0x20;
    volatile UINT32 *doorbells = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT32 control = event_ring[event_dequeue_index].control;
    UINT32 endpoint_number = controller->interrupt_endpoint_address & 0x0F;
    UINT32 endpoint_context_index = endpoint_number * 2 + 1;
    char character;
    UINT8 slot_id = (UINT8)(control >> 24);

    if (
        (control & XHCI_TRB_CYCLE) == 0 ||
        ((control >> 10) & 0x3F) != XHCI_TRB_TYPE_TRANSFER_EVENT ||
        slot_id != controller->enabled_slot
    ) {
        return 0;
    }
    event_dequeue_index++;
    *(volatile UINT64 *)(interrupter + 0x18) =
        ((UINT64)(UINTN)&event_ring[event_dequeue_index]) | (1ULL << 3);
    controller->hid_report_count++;
    character = hid_keycode_to_character(hid_keyboard_report[2], hid_keyboard_report[0]);
    if (character != 0) {
        keyboard_inject_character(character);
    }

    clear_bytes(hid_keyboard_report, sizeof(hid_keyboard_report));
    interrupt_in_ring[interrupt_ring_enqueue_index].parameter = (UINT64)(UINTN)&hid_keyboard_report[0];
    interrupt_in_ring[interrupt_ring_enqueue_index].status = controller->interrupt_endpoint_packet_size;
    interrupt_in_ring[interrupt_ring_enqueue_index].control =
        XHCI_TRB_TYPE_NORMAL |
        XHCI_TRB_CYCLE |
        (1U << 5);
    interrupt_ring_enqueue_index++;
    doorbells[controller->enabled_slot] = endpoint_context_index;
    return character != 0;
}

int xhci_poll_devices(ASAS_XHCI_CONTROLLER *controller)
{
    volatile UINT8 *capabilities = (volatile UINT8 *)(UINTN)controller->mmio_base;
    UINT32 runtime_offset = *(volatile UINT32 *)(capabilities + 0x18) & ~0x1FU;
    UINT32 doorbell_offset = *(volatile UINT32 *)(capabilities + 0x14) & ~3U;
    volatile UINT8 *interrupter = capabilities + runtime_offset + 0x20;
    volatile UINT32 *doorbells = (volatile UINT32 *)(capabilities + doorbell_offset);
    UINT32 control = event_ring[event_dequeue_index].control;
    UINT8 slot_id = (UINT8)(control >> 24);
    UINT32 endpoint_context_index;

    if (
        (control & XHCI_TRB_CYCLE) == 0 ||
        ((control >> 10) & 0x3F) != XHCI_TRB_TYPE_TRANSFER_EVENT
    ) {
        return 0;
    }
    if (slot_id == controller->enabled_slot) {
        return xhci_poll_keyboard(controller);
    }
    if (slot_id != controller->mouse_enabled_slot) {
        return 0;
    }

    event_dequeue_index++;
    *(volatile UINT64 *)(interrupter + 0x18) =
        ((UINT64)(UINTN)&event_ring[event_dequeue_index]) | (1ULL << 3);
    mouse_inject_report(
        (long long)(char)hid_mouse_report[1],
        (long long)(char)hid_mouse_report[2],
        hid_mouse_report[0]
    );
    controller->mouse_report_count++;
    clear_bytes(hid_mouse_report, sizeof(hid_mouse_report));
    mouse_interrupt_in_ring[mouse_interrupt_ring_enqueue_index].parameter =
        (UINT64)(UINTN)&hid_mouse_report[0];
    mouse_interrupt_in_ring[mouse_interrupt_ring_enqueue_index].status =
        controller->mouse_interrupt_endpoint_packet_size;
    mouse_interrupt_in_ring[mouse_interrupt_ring_enqueue_index].control =
        XHCI_TRB_TYPE_NORMAL | XHCI_TRB_CYCLE | (1U << 5);
    mouse_interrupt_ring_enqueue_index++;
    endpoint_context_index =
        (controller->mouse_interrupt_endpoint_address & 0x0F) * 2 + 1;
    doorbells[controller->mouse_enabled_slot] = endpoint_context_index;
    return 2;
}

int xhci_poll_active_keyboard(void)
{
    return active_controller != 0 && xhci_poll_devices(active_controller) == 1;
}

static int xhci_storage_transfer(UINT8 opcode, UINT64 lba, UINT32 count,
                                 void *buffer)
{
    volatile UINT8 *capabilities;
    UINT8 command[10] = { 0 };
    UINT8 *bytes = (UINT8 *)buffer;
    UINT32 chunk;

    if (active_controller == 0 || buffer == 0 || count == 0 ||
        !active_controller->storage_capacity_read ||
        active_controller->storage_block_size < USB_STORAGE_MIN_BLOCK_SIZE ||
        active_controller->storage_block_size > USB_STORAGE_MAX_BLOCK_SIZE ||
        lba >= active_controller->storage_sector_count ||
        (UINT64)count > active_controller->storage_sector_count - lba ||
        lba + count - 1U > 0xFFFFFFFFULL) {
        return 0;
    }

    capabilities = (volatile UINT8 *)(UINTN)active_controller->mmio_base;
    command[0] = opcode;
    while (count != 0) {
        chunk = count;
        if (chunk > USB_STORAGE_MAX_TRANSFER /
                    active_controller->storage_block_size)
            chunk = USB_STORAGE_MAX_TRANSFER /
                    active_controller->storage_block_size;
        if (chunk > 0xFFFFU) chunk = 0xFFFFU;
        write_be32(&command[2], (UINT32)lba);
        command[7] = (UINT8)(chunk >> 8);
        command[8] = (UINT8)chunk;
        if (opcode == 0x2AU) {
            copy_bytes(storage_sector_data,
                       bytes, chunk * active_controller->storage_block_size);
        } else {
            clear_bytes(storage_sector_data,
                        chunk * active_controller->storage_block_size);
        }
        if (!storage_bot_command(
            capabilities, active_controller, command, sizeof(command),
            storage_sector_data,
            chunk * active_controller->storage_block_size,
            opcode == 0x28U
        )) return 0;
        if (opcode == 0x28U) {
            copy_bytes(bytes, storage_sector_data,
                       chunk * active_controller->storage_block_size);
        }
        lba += chunk;
        bytes += (UINT64)chunk * active_controller->storage_block_size;
        count -= chunk;
    }
    return 1;
}

int xhci_storage_read_blocks(UINT64 lba, UINT32 count, void *buffer)
{
    return xhci_storage_transfer(0x28U, lba, count, buffer);
}

int xhci_storage_write_blocks(UINT64 lba, UINT32 count, const void *buffer)
{
    if (active_controller == 0 || active_controller->storage_read_only) return 0;
    return xhci_storage_transfer(0x2AU, lba, count, (void *)buffer);
}

int xhci_storage_flush(void)
{
    volatile UINT8 *capabilities;
    UINT8 command[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    if (active_controller == 0 || !active_controller->storage_capacity_read) return 0;
    if (active_controller->storage_read_only) return 1;
    capabilities = (volatile UINT8 *)(UINTN)active_controller->mmio_base;
    return storage_bot_command(capabilities, active_controller, command,
                               sizeof(command), 0, 0, 0);
}

static int usb_block_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                          UINT32 count, void *buffer)
{
    (void)device;
    return xhci_storage_read_blocks(lba, count, buffer);
}

static int usb_block_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                           UINT32 count, const void *buffer)
{
    (void)device;
    return xhci_storage_write_blocks(lba, count, buffer);
}

static int usb_block_flush(ASAS_BLOCK_DEVICE *device)
{
    (void)device;
    return xhci_storage_flush();
}

static const ASAS_BLOCK_DEVICE_OPS usb_block_ops = {
    usb_block_read,
    usb_block_write,
    usb_block_flush
};

int xhci_register_storage_block_device(void)
{
    ASAS_BLOCK_DEVICE description = { 0 };
    static UINT8 multi_block_probe[USB_STORAGE_MAX_BLOCK_SIZE * 2U];

    if (active_controller == 0 || !active_controller->storage_capacity_read ||
        block_device_find("usb0") != 0) {
        return 0;
    }
    if (!xhci_storage_read_blocks(0, 2, multi_block_probe)) return 0;
    description.name[0] = 'u';
    description.name[1] = 's';
    description.name[2] = 'b';
    description.name[3] = '0';
    description.logical_block_size = active_controller->storage_block_size;
    description.physical_block_size = active_controller->storage_block_size;
    description.block_count = active_controller->storage_sector_count;
    description.flags = BLOCK_DEVICE_FLAG_REMOVABLE | BLOCK_DEVICE_FLAG_HOT_PLUG |
        (active_controller->storage_read_only ? BLOCK_DEVICE_FLAG_READ_ONLY : 0);
    description.ops = &usb_block_ops;
    if (block_device_register(&description) == 0) return 0;
    logger_write("INFO", "USB Mass Storage direct block device registered");
    logger_write("INFO", "USB Mass Storage multi-block request verified");
    return 1;
}

int xhci_initialize(
    const ASAS_PCI_DEVICE *device,
    ASAS_XHCI_CONTROLLER *controller,
    ASAS_PAGE_TABLES *tables
)
{
    volatile UINT8 *capabilities;
    volatile UINT8 *operational;
    volatile UINT32 *command;
    volatile UINT32 *status;
    UINT64 physical_base;
    UINT32 structural_parameters;
    UINT32 page;
    UINT32 port;

    physical_base = (UINT64)(device->bars[0] & ~0xFU);
    if ((device->bars[0] & 0x6U) == 0x4U) {
        physical_base |= (UINT64)device->bars[1] << 32;
    }
    if (physical_base == 0) {
        return 0;
    }
    logger_write_hex("INFO", "xHCI physical MMIO base", physical_base);

    pci_enable_bus_mastering(device);
    for (page = 0; page < XHCI_MMIO_PAGE_COUNT; page++) {
        if (!map_page(
            tables,
            XHCI_MMIO_VIRTUAL_BASE + (UINT64)page * 4096,
            physical_base + (UINT64)page * 4096,
            ASAS_PAGE_WRITABLE | ASAS_PAGE_NO_EXECUTE
        )) {
            return 0;
        }
    }
    controller->mmio_base = XHCI_MMIO_VIRTUAL_BASE;
    capabilities = (volatile UINT8 *)(UINTN)controller->mmio_base;
    controller->version = *(volatile UINT16 *)(capabilities + 2);
    structural_parameters = *(volatile UINT32 *)(capabilities + 4);
    controller->max_slots = (UINT8)(structural_parameters & 0xFF);
    controller->max_ports = (UINT8)((structural_parameters >> 24) & 0xFF);
    controller->connected_ports = 0;
    controller->enabled_slot = 0;
    controller->addressed_port = 0;
    controller->port_speed = 0;
    controller->device_addressed = 0;
    controller->descriptor_read = 0;
    controller->vendor_id = 0;
    controller->product_id = 0;
    controller->interface_class = 0;
    controller->interface_protocol = 0;
    controller->hid_keyboard_detected = 0;
    controller->interrupt_endpoint_address = 0;
    controller->interrupt_endpoint_interval = 0;
    controller->interrupt_endpoint_packet_size = 0;
    controller->interrupt_endpoint_configured = 0;
    controller->configuration_set = 0;
    controller->hid_report_queued = 0;
    controller->hid_report_count = 0;
    controller->mouse_enabled_slot = 0;
    controller->mouse_addressed_port = 0;
    controller->hid_mouse_detected = 0;
    controller->mouse_interrupt_endpoint_address = 0;
    controller->mouse_interrupt_endpoint_packet_size = 0;
    controller->mouse_interrupt_endpoint_configured = 0;
    controller->mouse_report_queued = 0;
    controller->mouse_report_count = 0;
    controller->storage_enabled_slot = 0;
    controller->storage_addressed_port = 0;
    controller->usb_storage_detected = 0;
    controller->storage_bulk_in_endpoint_address = 0;
    controller->storage_bulk_out_endpoint_address = 0;
    controller->storage_bulk_in_packet_size = 0;
    controller->storage_bulk_out_packet_size = 0;
    controller->storage_bulk_endpoints_configured = 0;
    controller->storage_inquiry_completed = 0;
    controller->storage_capacity_read = 0;
    controller->storage_sector_read = 0;
    controller->storage_block_size = 0;
    controller->storage_sector_count = 0;
    controller->storage_read_only = 0;
    controller->supported_device_capacity = XHCI_SUPPORTED_DEVICE_CAPACITY;
    controller->static_dma_bytes = xhci_static_dma_bytes();
    logger_write_hex("INFO", "xHCI version", controller->version);
    logger_write_hex("INFO", "xHCI max slots", controller->max_slots);
    logger_write_hex("INFO", "xHCI max ports", controller->max_ports);
    logger_write_hex("INFO", "xHCI supported device capacity", controller->supported_device_capacity);
    logger_write_hex("INFO", "xHCI static DMA workspace bytes", controller->static_dma_bytes);

    if (controller->max_slots == 0 || controller->max_ports == 0) {
        return 0;
    }

    operational = capabilities + capabilities[0];
    command = (volatile UINT32 *)(operational + 0x00);
    status = (volatile UINT32 *)(operational + 0x04);

    *command &= ~XHCI_COMMAND_RUN;
    if (!wait_for_set(status, XHCI_STATUS_HALTED)) {
        return 0;
    }
    *command |= XHCI_COMMAND_RESET;
    if (!wait_for_clear(command, XHCI_COMMAND_RESET) || !wait_for_clear(status, XHCI_STATUS_NOT_READY)) {
        return 0;
    }
    if (!configure_rings(capabilities, operational, controller->max_slots)) {
        return 0;
    }
    *command |= XHCI_COMMAND_RUN;
    if (!wait_for_clear(status, XHCI_STATUS_HALTED)) {
        return 0;
    }
    controller->running = 1;
    for (port = 0; port < controller->max_ports; port++) {
        volatile UINT32 *port_status = (volatile UINT32 *)(
            operational + XHCI_PORT_REGISTER_BASE + port * XHCI_PORT_REGISTER_STRIDE
        );

        if ((*port_status & XHCI_PORT_CONNECTED) != 0) {
            controller->connected_ports++;
        }
    }
    if (!initialize_keyboard_device(capabilities, operational, controller)) {
        logger_write("ERROR", "xHCI keyboard device initialization failed");
        return 0;
    }
    if (controller->connected_ports > 1 && !initialize_mouse_device(capabilities, operational, controller)) {
        logger_write("ERROR", "xHCI mouse device initialization failed");
        return 0;
    }
    if (controller->connected_ports > 2 && !initialize_storage_device(capabilities, operational, controller)) {
        logger_write("ERROR", "xHCI storage device initialization failed");
        return 0;
    }
    active_controller = controller;
    return 1;
}
