#include "hyperv_storage.h"
#include "block_device.h"
#include "keyboard.h"
#include "logger.h"
#include "mouse.h"

extern void memory_fence(void);

#pragma intrinsic(__cpuid)
void __cpuid(int registers[4], int leaf);
#pragma intrinsic(__readmsr)
UINT64 __readmsr(UINT32 register_number);
#pragma intrinsic(__writemsr)
void __writemsr(UINT32 register_number, UINT64 value);
extern UINT64 hyperv_do_hypercall(
    UINT64 hypercall_page,
    UINT64 control,
    UINT64 input_gpa,
    UINT64 output_gpa
);
extern void apic_eoi(void);

#define HV_X64_MSR_GUEST_OS_ID 0x40000000U
#define HV_X64_MSR_HYPERCALL 0x40000001U
#define HV_X64_MSR_SCONTROL 0x40000080U
#define HV_X64_MSR_EOM 0x40000084U
#define HV_X64_MSR_SINT2 0x40000092U
#define HV_X64_MSR_SIEFP 0x40000082U
#define HV_X64_MSR_SIMP 0x40000083U
#define HV_SYNIC_ENABLE 1ULL
#define HV_SYNIC_PAGE_ENABLE 1ULL
#define HV_SYNIC_VECTOR 64ULL
#define HV_SYNIC_AUTO_EOI (1ULL << 17)
#define HVCALL_POST_MESSAGE 0x005CULL
#define HVCALL_SIGNAL_FAST 0x0001005DULL
#define HV_STATUS_SUCCESS 0
#define HV_MESSAGE_PAYLOAD_BYTE_COUNT 240
#define HV_MESSAGE_PAYLOAD_QWORD_COUNT 30
#define HVMSG_NONE 0
#define HVMSG_CHANNEL_MESSAGE 1
#define VMBUS_MESSAGE_CONNECTION_ID_4 4
#define VMBUS_MESSAGE_SINT 2
#define CHANNELMSG_OFFERCHANNEL 1
#define CHANNELMSG_REQUESTOFFERS 3
#define CHANNELMSG_ALLOFFERS_DELIVERED 4
#define CHANNELMSG_OPENCHANNEL 5
#define CHANNELMSG_OPENCHANNEL_RESULT 6
#define CHANNELMSG_GPADL_HEADER 8
#define CHANNELMSG_GPADL_CREATED 10
#define CHANNELMSG_INITIATE_CONTACT 14
#define CHANNELMSG_VERSION_RESPONSE 15
#define VERSION_WIN10_V5 0x00050000U

#pragma pack(push, 1)
typedef struct {
    UINT32 message_type;
    UINT8 payload_size;
    UINT8 message_flags;
    UINT8 reserved[2];
    UINT64 sender;
} HV_MESSAGE_HEADER;

typedef struct {
    HV_MESSAGE_HEADER header;
    UINT64 payload[HV_MESSAGE_PAYLOAD_QWORD_COUNT];
} HV_MESSAGE;

typedef struct {
    UINT32 connection_id;
    UINT32 reserved;
    UINT32 message_type;
    UINT32 payload_size;
    UINT64 payload[HV_MESSAGE_PAYLOAD_QWORD_COUNT];
} HV_INPUT_POST_MESSAGE;

typedef struct {
    UINT32 msgtype;
    UINT32 padding;
} VMBUS_CHANNEL_MESSAGE_HEADER;

typedef struct {
    UINT8 bytes[16];
} VMBUS_GUID;

typedef struct {
    VMBUS_GUID if_type;
    VMBUS_GUID if_instance;
    UINT64 reserved1;
    UINT64 reserved2;
    UINT16 channel_flags;
    UINT16 mmio_megabytes;
    UINT8 user_defined[120];
    UINT16 sub_channel_index;
    UINT16 reserved3;
} VMBUS_CHANNEL_OFFER;

typedef struct {
    VMBUS_CHANNEL_MESSAGE_HEADER header;
    VMBUS_CHANNEL_OFFER offer;
    UINT32 child_relid;
    UINT8 monitor_id;
    UINT8 monitor_allocated;
    UINT16 interrupt_flags;
    UINT32 connection_id;
} VMBUS_CHANNEL_OFFER_CHANNEL;

typedef struct {
    UINT32 byte_count;
    UINT32 byte_offset;
    UINT64 pfn_array[4];
} VMBUS_GPA_RANGE_4;

typedef struct {
    VMBUS_CHANNEL_MESSAGE_HEADER header;
    UINT32 child_relid;
    UINT32 gpadl;
    UINT16 range_buflen;
    UINT16 range_count;
    VMBUS_GPA_RANGE_4 range;
} VMBUS_CHANNEL_GPADL_HEADER_4;

typedef struct {
    VMBUS_CHANNEL_MESSAGE_HEADER header;
    UINT32 child_relid;
    UINT32 gpadl;
    UINT32 creation_status;
} VMBUS_CHANNEL_GPADL_CREATED;

typedef struct {
    VMBUS_CHANNEL_MESSAGE_HEADER header;
    UINT32 child_relid;
    UINT32 open_id;
    UINT32 ringbuffer_gpadl;
    UINT32 target_vp;
    UINT32 downstream_ringbuffer_page_offset;
    UINT8 user_data[120];
} VMBUS_CHANNEL_OPEN_CHANNEL;

typedef struct {
    VMBUS_CHANNEL_MESSAGE_HEADER header;
    UINT32 child_relid;
    UINT32 open_id;
    UINT32 open_status;
} VMBUS_CHANNEL_OPEN_RESULT;

typedef struct {
    VMBUS_CHANNEL_MESSAGE_HEADER header;
    UINT32 vmbus_version_requested;
    UINT32 target_vcpu;
    UINT8 msg_sint;
    UINT8 msg_vtl;
    UINT8 reserved[2];
    UINT32 feature_flags;
    UINT64 monitor_page1;
    UINT64 monitor_page2;
} VMBUS_CHANNEL_INITIATE_CONTACT;

typedef struct {
    VMBUS_CHANNEL_MESSAGE_HEADER header;
    UINT8 version_supported;
    UINT8 connection_state;
    UINT16 padding;
    UINT32 msg_conn_id;
} VMBUS_CHANNEL_VERSION_RESPONSE;
#pragma pack(pop)

static ASAS_HYPERV_STORAGE_STATUS status;
static volatile UINT64 hyperv_interrupt_count;
static const VMBUS_GUID storvsc_guid = {
    { 0xD9, 0x63, 0x61, 0xBA, 0xA1, 0x04, 0x29, 0x4D,
      0xB6, 0x05, 0x72, 0xE2, 0xFF, 0xB1, 0xDC, 0x7F }
};
static const VMBUS_GUID keyboard_guid = {
    { 0x6D, 0xAD, 0x12, 0xF9, 0x17, 0x2B, 0xEA, 0x48,
      0xBD, 0x65, 0xF9, 0x27, 0xA6, 0x1C, 0x76, 0x84 }
};
static const VMBUS_GUID hid_input_guid = {
    { 0x9E, 0xB6, 0xA8, 0xCF, 0x4A, 0x5B, 0xC0, 0x4C,
      0xB9, 0x8B, 0x8B, 0xA1, 0xA1, 0xF3, 0xF9, 0x5A }
};

static void clear_page(UINT64 page)
{
    UINT64 index;
    UINT8 *bytes = (UINT8 *)(UINTN)page;

    for (index = 0; index < 4096; index++) {
        bytes[index] = 0;
    }
}

static int bytes_equal(const char *left, const char *right, UINT32 count)
{
    UINT32 index;

    for (index = 0; index < count; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static void copy_bytes(void *destination, const void *source, UINT32 count)
{
    UINT32 index;
    UINT8 *out = (UINT8 *)destination;
    const UINT8 *in = (const UINT8 *)source;

    for (index = 0; index < count; index++) {
        out[index] = in[index];
    }
}

static int guid_equal(const VMBUS_GUID *left, const VMBUS_GUID *right)
{
    UINT32 index;

    for (index = 0; index < sizeof(left->bytes); index++) {
        if (left->bytes[index] != right->bytes[index]) {
            return 0;
        }
    }
    return 1;
}

static int hyperv_keyboard_protocol_initialize(void);

static int hyperv_post_message(UINT32 connection_id, const void *payload, UINT32 payload_size)
{
    HV_INPUT_POST_MESSAGE *message;

    if (
        status.post_message_page == 0 ||
        status.hypercall_page == 0 ||
        payload_size > HV_MESSAGE_PAYLOAD_BYTE_COUNT
    ) {
        return 0;
    }

    clear_page(status.post_message_page);
    message = (HV_INPUT_POST_MESSAGE *)(UINTN)status.post_message_page;
    message->connection_id = connection_id;
    message->message_type = HVMSG_CHANNEL_MESSAGE;
    message->payload_size = payload_size;
    copy_bytes(message->payload, payload, payload_size);

    status.post_status = hyperv_do_hypercall(
        status.hypercall_page,
        HVCALL_POST_MESSAGE,
        status.post_message_page,
        0
    ) & 0xFFFFULL;

    return status.post_status == HV_STATUS_SUCCESS;
}

static void zero_bytes(void *buffer, UINT32 count)
{
    UINT32 index;
    UINT8 *bytes = (UINT8 *)buffer;

    for (index = 0; index < count; index++) {
        bytes[index] = 0;
    }
}

static void hyperv_ack_message(volatile HV_MESSAGE *message)
{
    message->header.message_type = HVMSG_NONE;
    __writemsr(HV_X64_MSR_EOM, 0);
}

static void hyperv_ack_channel_events(void)
{
    volatile UINT64 *flags;
    UINT32 index;

    if (status.event_flags_page == 0) {
        return;
    }

    flags = (volatile UINT64 *)(UINTN)(status.event_flags_page + (VMBUS_MESSAGE_SINT * 256U));
    for (index = 0; index < 32U; index++) {
        flags[index] = 0;
    }
    __writemsr(HV_X64_MSR_EOM, 0);
}

static void hyperv_poll_version_response(void)
{
    UINT32 spin;
    volatile HV_MESSAGE *message =
        &((volatile HV_MESSAGE *)(UINTN)status.message_page)[VMBUS_MESSAGE_SINT];

    for (spin = 0; spin < 10000000U; spin++) {
        if (message->header.message_type != HVMSG_NONE) {
            VMBUS_CHANNEL_MESSAGE_HEADER *channel_header =
                (VMBUS_CHANNEL_MESSAGE_HEADER *)message->payload;

            status.response_message_type = message->header.message_type;
            status.response_channel_type = channel_header->msgtype;
            if (channel_header->msgtype == CHANNELMSG_VERSION_RESPONSE) {
                VMBUS_CHANNEL_VERSION_RESPONSE *response =
                    (VMBUS_CHANNEL_VERSION_RESPONSE *)message->payload;

                status.version_response_received = 1;
                status.version_supported = response->version_supported;
                status.message_connection_id = response->msg_conn_id;
                if (response->version_supported) {
                    status.vmbus_connected = 1;
                }
            }
            hyperv_ack_message(message);
            return;
        }
    }
}

static void hyperv_handle_offer(const VMBUS_CHANNEL_OFFER_CHANNEL *offer)
{
    status.offer_count++;
    if (guid_equal(&offer->offer.if_type, &storvsc_guid)) {
        status.storvsc_offered = 1;
        status.storvsc_relid = offer->child_relid;
        status.storvsc_connection_id = offer->connection_id;
        logger_write("INFO", "Hyper-V StorVSC channel offer found");
    } else if (guid_equal(&offer->offer.if_type, &keyboard_guid)) {
        status.keyboard_offered = 1;
        status.keyboard_relid = offer->child_relid;
        status.keyboard_connection_id = offer->connection_id;
        logger_write("INFO", "Hyper-V keyboard channel offer found");
    } else if (guid_equal(&offer->offer.if_type, &hid_input_guid)) {
        status.hid_input_offered = 1;
        status.hid_input_relid = offer->child_relid;
        status.hid_input_connection_id = offer->connection_id;
        logger_write("INFO", "Hyper-V HID input channel offer found");
    }
}

static void hyperv_poll_channel_open(void)
{
    UINT32 spin;
    volatile HV_MESSAGE *message =
        &((volatile HV_MESSAGE *)(UINTN)status.message_page)[VMBUS_MESSAGE_SINT];

    for (spin = 0; spin < 50000000U; spin++) {
        if (message->header.message_type != HVMSG_NONE) {
            VMBUS_CHANNEL_MESSAGE_HEADER *channel_header =
                (VMBUS_CHANNEL_MESSAGE_HEADER *)message->payload;

            status.response_message_type = message->header.message_type;
            status.response_channel_type = channel_header->msgtype;
            if (channel_header->msgtype == CHANNELMSG_GPADL_CREATED) {
                VMBUS_CHANNEL_GPADL_CREATED *created =
                    (VMBUS_CHANNEL_GPADL_CREATED *)message->payload;

                if (created->child_relid == status.storvsc_relid &&
                    created->gpadl == status.storvsc_gpadl &&
                    created->creation_status == 0) {
                    status.storvsc_gpadl_created = 1;
                } else if (created->child_relid == status.keyboard_relid &&
                    created->gpadl == status.keyboard_gpadl &&
                    created->creation_status == 0) {
                    status.keyboard_gpadl_created = 1;
                } else if (created->child_relid == status.hid_input_relid &&
                    created->gpadl == status.hid_input_gpadl &&
                    created->creation_status == 0) {
                    status.hid_input_gpadl_created = 1;
                }
            } else if (channel_header->msgtype == CHANNELMSG_OPENCHANNEL_RESULT) {
                VMBUS_CHANNEL_OPEN_RESULT *result =
                    (VMBUS_CHANNEL_OPEN_RESULT *)message->payload;

                if (result->child_relid == status.storvsc_relid) {
                    status.storvsc_open_status = result->open_status;
                    if (result->open_status == 0) {
                        status.storvsc_opened = 1;
                    }
                } else if (result->child_relid == status.keyboard_relid) {
                    status.keyboard_open_status = result->open_status;
                    if (result->open_status == 0) {
                        status.keyboard_opened = 1;
                    }
                } else if (result->child_relid == status.hid_input_relid) {
                    status.hid_input_open_status = result->open_status;
                    if (result->open_status == 0) {
                        status.hid_input_opened = 1;
                    }
                }
            }
            hyperv_ack_message(message);
            if (
                status.storvsc_opened &&
                (!status.keyboard_offered || status.keyboard_opened) &&
                (!status.hid_input_offered || status.hid_input_opened)
            ) {
                return;
            }
        }
    }
}

static int hyperv_open_channel(
    UINT32 child_relid,
    UINT32 gpadl_id,
    UINT64 ring_page1,
    UINT64 ring_page2,
    UINT64 ring_page3,
    UINT64 ring_page4,
    UINT8 *gpadl_posted,
    UINT8 *gpadl_created,
    UINT8 *open_posted,
    UINT8 *opened,
    UINT32 *open_status
)
{
    UINT32 connection_id = status.message_connection_id != 0 ?
        status.message_connection_id :
        VMBUS_MESSAGE_CONNECTION_ID_4;
    VMBUS_CHANNEL_GPADL_HEADER_4 gpadl;
    VMBUS_CHANNEL_OPEN_CHANNEL open_channel;

    if (child_relid == 0 || ring_page1 == 0 || ring_page2 == 0 || ring_page3 == 0 || ring_page4 == 0) {
        return 0;
    }

    zero_bytes(&gpadl, sizeof(gpadl));
    gpadl.header.msgtype = CHANNELMSG_GPADL_HEADER;
    gpadl.child_relid = child_relid;
    gpadl.gpadl = gpadl_id;
    gpadl.range_buflen = (UINT16)sizeof(gpadl.range);
    gpadl.range_count = 1;
    gpadl.range.byte_count = 16384;
    gpadl.range.byte_offset = 0;
    gpadl.range.pfn_array[0] = ring_page1 >> 12;
    gpadl.range.pfn_array[1] = ring_page2 >> 12;
    gpadl.range.pfn_array[2] = ring_page3 >> 12;
    gpadl.range.pfn_array[3] = ring_page4 >> 12;

    *gpadl_posted = (UINT8)hyperv_post_message(connection_id, &gpadl, sizeof(gpadl));
    if (!*gpadl_posted) {
        return 0;
    }

    hyperv_poll_channel_open();
    if (!*gpadl_created) {
        return 0;
    }

    zero_bytes(&open_channel, sizeof(open_channel));
    open_channel.header.msgtype = CHANNELMSG_OPENCHANNEL;
    open_channel.child_relid = child_relid;
    open_channel.open_id = child_relid;
    open_channel.ringbuffer_gpadl = gpadl_id;
    open_channel.target_vp = 0;
    open_channel.downstream_ringbuffer_page_offset = 2;

    *open_posted = (UINT8)hyperv_post_message(connection_id, &open_channel, sizeof(open_channel));
    if (!*open_posted) {
        return 0;
    }

    hyperv_poll_channel_open();
    return *opened && *open_status == 0;
}

static void hyperv_open_storvsc(void)
{
    UINT32 connection_id = status.message_connection_id != 0 ?
        status.message_connection_id :
        VMBUS_MESSAGE_CONNECTION_ID_4;
    VMBUS_CHANNEL_GPADL_HEADER_4 gpadl;
    VMBUS_CHANNEL_OPEN_CHANNEL open_channel;

    if (
        !status.storvsc_offered ||
        status.storvsc_ring_page1 == 0 ||
        status.storvsc_ring_page2 == 0 ||
        status.storvsc_ring_page3 == 0 ||
        status.storvsc_ring_page4 == 0
    ) {
        return;
    }

    zero_bytes(&gpadl, sizeof(gpadl));
    gpadl.header.msgtype = CHANNELMSG_GPADL_HEADER;
    gpadl.child_relid = status.storvsc_relid;
    gpadl.gpadl = status.storvsc_gpadl;
    gpadl.range_buflen = (UINT16)sizeof(gpadl.range);
    gpadl.range_count = 1;
    gpadl.range.byte_count = 16384;
    gpadl.range.byte_offset = 0;
    gpadl.range.pfn_array[0] = status.storvsc_ring_page1 >> 12;
    gpadl.range.pfn_array[1] = status.storvsc_ring_page2 >> 12;
    gpadl.range.pfn_array[2] = status.storvsc_ring_page3 >> 12;
    gpadl.range.pfn_array[3] = status.storvsc_ring_page4 >> 12;

    status.storvsc_gpadl_posted = (UINT8)hyperv_post_message(
        connection_id,
        &gpadl,
        sizeof(gpadl)
    );
    if (!status.storvsc_gpadl_posted) {
        logger_write_hex("ERROR", "Hyper-V StorVSC GPADL post failed", status.post_status);
        return;
    }

    hyperv_poll_channel_open();
    if (!status.storvsc_gpadl_created) {
        logger_write("ERROR", "Hyper-V StorVSC GPADL was not created");
        return;
    }

    zero_bytes(&open_channel, sizeof(open_channel));
    open_channel.header.msgtype = CHANNELMSG_OPENCHANNEL;
    open_channel.child_relid = status.storvsc_relid;
    open_channel.open_id = status.storvsc_relid;
    open_channel.ringbuffer_gpadl = status.storvsc_gpadl;
    open_channel.target_vp = 0;
    open_channel.downstream_ringbuffer_page_offset = 2;

    status.storvsc_open_posted = (UINT8)hyperv_post_message(
        connection_id,
        &open_channel,
        sizeof(open_channel)
    );
    if (!status.storvsc_open_posted) {
        logger_write_hex("ERROR", "Hyper-V StorVSC open post failed", status.post_status);
        return;
    }

    hyperv_poll_channel_open();
    if (status.storvsc_opened) {
        logger_write("INFO", "Hyper-V StorVSC channel opened");
    }
}

static void hyperv_poll_offers(void)
{
    UINT32 spin;
    volatile HV_MESSAGE *message =
        &((volatile HV_MESSAGE *)(UINTN)status.message_page)[VMBUS_MESSAGE_SINT];

    for (spin = 0; spin < 50000000U; spin++) {
        if (message->header.message_type != HVMSG_NONE) {
            VMBUS_CHANNEL_MESSAGE_HEADER *channel_header =
                (VMBUS_CHANNEL_MESSAGE_HEADER *)message->payload;

            status.response_message_type = message->header.message_type;
            status.response_channel_type = channel_header->msgtype;
            if (channel_header->msgtype == CHANNELMSG_OFFERCHANNEL) {
                hyperv_handle_offer((const VMBUS_CHANNEL_OFFER_CHANNEL *)message->payload);
            } else if (channel_header->msgtype == CHANNELMSG_ALLOFFERS_DELIVERED) {
                status.all_offers_delivered = 1;
            }
            hyperv_ack_message(message);
            if (status.all_offers_delivered || (status.storvsc_offered && (status.keyboard_offered || status.hid_input_offered))) {
                return;
            }
        }
    }
}

static void hyperv_open_keyboard(void)
{
    if (!status.keyboard_offered) {
        return;
    }
    if (hyperv_open_channel(
        status.keyboard_relid,
        status.keyboard_gpadl,
        status.keyboard_ring_page1,
        status.keyboard_ring_page2,
        status.keyboard_ring_page3,
        status.keyboard_ring_page4,
        &status.keyboard_gpadl_posted,
        &status.keyboard_gpadl_created,
        &status.keyboard_open_posted,
        &status.keyboard_opened,
        &status.keyboard_open_status
    )) {
        logger_write("INFO", "Hyper-V keyboard channel opened");
        (void)hyperv_keyboard_protocol_initialize();
    }
}

static int hyperv_hid_protocol_initialize(void);
static void hyperv_hid_poll(void);

static void hyperv_open_hid_input(void)
{
    if (!status.hid_input_offered) {
        return;
    }
    if (hyperv_open_channel(
        status.hid_input_relid,
        status.hid_input_gpadl,
        status.hid_input_ring_page1,
        status.hid_input_ring_page2,
        status.hid_input_ring_page3,
        status.hid_input_ring_page4,
        &status.hid_input_gpadl_posted,
        &status.hid_input_gpadl_created,
        &status.hid_input_open_posted,
        &status.hid_input_opened,
        &status.hid_input_open_status
    )) {
        logger_write("INFO", "Hyper-V HID input channel opened");
        (void)hyperv_hid_protocol_initialize();
    }
}

static void hyperv_request_offers(void)
{
    VMBUS_CHANNEL_MESSAGE_HEADER message;
    UINT32 connection_id = status.message_connection_id != 0 ?
        status.message_connection_id :
        VMBUS_MESSAGE_CONNECTION_ID_4;

    if (!status.vmbus_connected) {
        return;
    }

    message.msgtype = CHANNELMSG_REQUESTOFFERS;
    message.padding = 0;
    status.offers_requested = (UINT8)hyperv_post_message(
        connection_id,
        &message,
        sizeof(message)
    );

    if (status.offers_requested) {
        logger_write("INFO", "Hyper-V VMBus offers requested");
        hyperv_poll_offers();
        if (status.storvsc_offered) {
            hyperv_open_storvsc();
        }
        if (status.keyboard_offered) {
            hyperv_open_keyboard();
        }
        if (status.hid_input_offered) {
            hyperv_open_hid_input();
        }
    } else {
        logger_write_hex("ERROR", "Hyper-V VMBus request offers failed", status.post_status);
    }
}

static void hyperv_initiate_contact(void)
{
    VMBUS_CHANNEL_INITIATE_CONTACT message;

    message.header.msgtype = CHANNELMSG_INITIATE_CONTACT;
    message.header.padding = 0;
    message.vmbus_version_requested = VERSION_WIN10_V5;
    message.target_vcpu = 0;
    message.msg_sint = VMBUS_MESSAGE_SINT;
    message.msg_vtl = 0;
    message.reserved[0] = 0;
    message.reserved[1] = 0;
    message.feature_flags = 0;
    message.monitor_page1 = status.monitor_page1;
    message.monitor_page2 = status.monitor_page2;

    status.contact_posted = (UINT8)hyperv_post_message(
        VMBUS_MESSAGE_CONNECTION_ID_4,
        &message,
        sizeof(message)
    );

    if (status.contact_posted) {
        logger_write("INFO", "Hyper-V VMBus initiate contact posted");
        hyperv_poll_version_response();
        hyperv_request_offers();
    } else {
        logger_write_hex("ERROR", "Hyper-V VMBus initiate contact failed", status.post_status);
    }
}

int hyperv_storage_initialize(ASAS_FRAME_ALLOCATOR *allocator)
{
    int registers[4];
    char vendor[13];

    status.detected = 0;
    status.hypercall_ready = 0;
    status.synic_ready = 0;
    status.contact_posted = 0;
    status.version_response_received = 0;
    status.version_supported = 0;
    status.vmbus_connected = 0;
    status.offers_requested = 0;
    status.all_offers_delivered = 0;
    status.storvsc_offered = 0;
    status.storvsc_gpadl_posted = 0;
    status.storvsc_gpadl_created = 0;
    status.storvsc_open_posted = 0;
    status.storvsc_opened = 0;
    status.keyboard_offered = 0;
    status.keyboard_gpadl_posted = 0;
    status.keyboard_gpadl_created = 0;
    status.keyboard_open_posted = 0;
    status.keyboard_opened = 0;
    status.keyboard_protocol_ready = 0;
    status.hid_input_offered = 0;
    status.hid_input_gpadl_posted = 0;
    status.hid_input_gpadl_created = 0;
    status.hid_input_open_posted = 0;
    status.hid_input_opened = 0;
    status.hid_input_protocol_ready = 0;
    status.hid_input_device_ready = 0;
    status.hypercall_page = 0;
    status.post_message_page = 0;
    status.message_page = 0;
    status.event_flags_page = 0;
    status.monitor_page1 = 0;
    status.monitor_page2 = 0;
    status.storvsc_ring_page1 = 0;
    status.storvsc_ring_page2 = 0;
    status.storvsc_ring_page3 = 0;
    status.storvsc_ring_page4 = 0;
    status.keyboard_ring_page1 = 0;
    status.keyboard_ring_page2 = 0;
    status.keyboard_ring_page3 = 0;
    status.keyboard_ring_page4 = 0;
    status.hid_input_ring_page1 = 0;
    status.hid_input_ring_page2 = 0;
    status.hid_input_ring_page3 = 0;
    status.hid_input_ring_page4 = 0;
    status.post_status = 0;
    status.message_connection_id = 0;
    status.response_message_type = 0;
    status.response_channel_type = 0;
    status.offer_count = 0;
    status.storvsc_relid = 0;
    status.storvsc_connection_id = 0;
    status.storvsc_gpadl = 0xE1E10;
    status.storvsc_open_status = 0xFFFFFFFFU;
    status.keyboard_relid = 0;
    status.keyboard_connection_id = 0;
    status.keyboard_gpadl = 0xE1E20;
    status.keyboard_open_status = 0xFFFFFFFFU;
    status.keyboard_protocol_status = 0;
    status.keyboard_event_count = 0;
    status.keyboard_packet_count = 0;
    status.keyboard_bad_packet_count = 0;
    status.keyboard_last_make_code = 0;
    status.keyboard_last_info = 0;
    status.hid_input_relid = 0;
    status.hid_input_connection_id = 0;
    status.hid_input_gpadl = 0xE1E30;
    status.hid_input_open_status = 0xFFFFFFFFU;
    status.hid_input_report_count = 0;
    status.hid_input_packet_count = 0;
    status.hid_input_last_report_size = 0;
    status.hid_input_vendor = 0;
    status.hid_input_product = 0;
    status.hid_input_report_desc_size = 0;
    status.hid_input_report_desc0 = 0;
    status.hid_input_report_desc1 = 0;
    status.hid_input_report_desc2 = 0;
    status.hid_input_report_desc3 = 0;
    status.hid_input_last_report0 = 0;
    status.hid_input_last_report1 = 0;
    status.hid_input_last_report2 = 0;
    status.hid_input_last_report3 = 0;
    status.hid_input_last_report4 = 0;
    status.hid_input_last_report5 = 0;
    status.hid_input_last_report6 = 0;
    status.hid_input_last_report7 = 0;
    status.vsp_initialized = 0;
    status.raw_read_ready = 0;
    status.raw_write_supported = 0;
    status.last_srb_status = 0;
    status.last_scsi_status = 0;
    status.last_vstor_status = 0;

    __cpuid(registers, 1);
    if ((((UINT32)registers[2] >> 31) & 1U) == 0) {
        return 0;
    }

    __cpuid(registers, 0x40000000);
    vendor[0] = (char)(registers[1] & 0xFF);
    vendor[1] = (char)((registers[1] >> 8) & 0xFF);
    vendor[2] = (char)((registers[1] >> 16) & 0xFF);
    vendor[3] = (char)((registers[1] >> 24) & 0xFF);
    vendor[4] = (char)(registers[2] & 0xFF);
    vendor[5] = (char)((registers[2] >> 8) & 0xFF);
    vendor[6] = (char)((registers[2] >> 16) & 0xFF);
    vendor[7] = (char)((registers[2] >> 24) & 0xFF);
    vendor[8] = (char)(registers[3] & 0xFF);
    vendor[9] = (char)((registers[3] >> 8) & 0xFF);
    vendor[10] = (char)((registers[3] >> 16) & 0xFF);
    vendor[11] = (char)((registers[3] >> 24) & 0xFF);
    vendor[12] = '\0';

    if (!bytes_equal(vendor, "Microsoft Hv", 12)) {
        return 0;
    }

    status.detected = 1;
    status.hypercall_page = frame_allocate(allocator);
    status.post_message_page = frame_allocate(allocator);
    status.message_page = frame_allocate(allocator);
    status.event_flags_page = frame_allocate(allocator);
    status.monitor_page1 = frame_allocate(allocator);
    status.monitor_page2 = frame_allocate(allocator);
    status.storvsc_ring_page1 = frame_allocate(allocator);
    status.storvsc_ring_page2 = frame_allocate(allocator);
    status.storvsc_ring_page3 = frame_allocate(allocator);
    status.storvsc_ring_page4 = frame_allocate(allocator);
    status.keyboard_ring_page1 = frame_allocate(allocator);
    status.keyboard_ring_page2 = frame_allocate(allocator);
    status.keyboard_ring_page3 = frame_allocate(allocator);
    status.keyboard_ring_page4 = frame_allocate(allocator);
    status.hid_input_ring_page1 = frame_allocate(allocator);
    status.hid_input_ring_page2 = frame_allocate(allocator);
    status.hid_input_ring_page3 = frame_allocate(allocator);
    status.hid_input_ring_page4 = frame_allocate(allocator);
    if (status.hypercall_page == 0 || status.message_page == 0 || status.event_flags_page == 0) {
        logger_write("ERROR", "Hyper-V SynIC page allocation failed");
        return 1;
    }
    if (
        status.post_message_page == 0 ||
        status.monitor_page1 == 0 ||
        status.monitor_page2 == 0 ||
        status.storvsc_ring_page1 == 0 ||
        status.storvsc_ring_page2 == 0 ||
        status.storvsc_ring_page3 == 0 ||
        status.storvsc_ring_page4 == 0 ||
        status.keyboard_ring_page1 == 0 ||
        status.keyboard_ring_page2 == 0 ||
        status.keyboard_ring_page3 == 0 ||
        status.keyboard_ring_page4 == 0 ||
        status.hid_input_ring_page1 == 0 ||
        status.hid_input_ring_page2 == 0 ||
        status.hid_input_ring_page3 == 0 ||
        status.hid_input_ring_page4 == 0
    ) {
        logger_write("ERROR", "Hyper-V VMBus page allocation failed");
        return 1;
    }

    clear_page(status.hypercall_page);
    clear_page(status.post_message_page);
    clear_page(status.message_page);
    clear_page(status.event_flags_page);
    clear_page(status.monitor_page1);
    clear_page(status.monitor_page2);
    clear_page(status.storvsc_ring_page1);
    clear_page(status.storvsc_ring_page2);
    clear_page(status.storvsc_ring_page3);
    clear_page(status.storvsc_ring_page4);
    clear_page(status.keyboard_ring_page1);
    clear_page(status.keyboard_ring_page2);
    clear_page(status.keyboard_ring_page3);
    clear_page(status.keyboard_ring_page4);
    clear_page(status.hid_input_ring_page1);
    clear_page(status.hid_input_ring_page2);
    clear_page(status.hid_input_ring_page3);
    clear_page(status.hid_input_ring_page4);

    __writemsr(HV_X64_MSR_GUEST_OS_ID, 0x0001415341530001ULL);
    __writemsr(HV_X64_MSR_HYPERCALL, HV_SYNIC_PAGE_ENABLE | status.hypercall_page);
    if ((__readmsr(HV_X64_MSR_HYPERCALL) & HV_SYNIC_PAGE_ENABLE) != 0) {
        status.hypercall_ready = 1;
        logger_write("INFO", "Hyper-V hypercall page initialized");
    }
    __writemsr(HV_X64_MSR_SIMP, HV_SYNIC_PAGE_ENABLE | status.message_page);
    __writemsr(HV_X64_MSR_SIEFP, HV_SYNIC_PAGE_ENABLE | status.event_flags_page);
    __writemsr(HV_X64_MSR_SINT2, HV_SYNIC_VECTOR | HV_SYNIC_AUTO_EOI);
    __writemsr(HV_X64_MSR_SCONTROL, __readmsr(HV_X64_MSR_SCONTROL) | HV_SYNIC_ENABLE);

    status.synic_ready = 1;
    logger_write("INFO", "Hyper-V SynIC message and event pages initialized");
    hyperv_initiate_contact();
    return 1;
}

int hyperv_storage_detected(void)
{
    return status.detected;
}

const ASAS_HYPERV_STORAGE_STATUS *hyperv_storage_status(void)
{
    return &status;
}

/* ====================================================================
 * VSP PROTOCOL -- ring buffer + VSTOR handshake + SCSI sector read
 * ==================================================================== */

#define VSP_RING_DATA_SIZE  4096U
#define VSP_RING_TRAIL      8U
#define VMBUS_PKT_INBAND     6U   /* VM_PKT_DATA_INBAND           = 0x6 */
#define VMBUS_PKT_GPA_DIRECT 9U   /* VM_PKT_DATA_USING_GPA_DIRECT = 0x9 (Linux hyperv.h) */
#define VMBUS_PKT_ACK        1U   /* VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED */
#define VSTOR_OP_COMPLETE   1U
#define VSTOR_OP_SRB        3U
#define VSTOR_OP_BEGIN      7U
#define VSTOR_OP_END        8U
#define VSTOR_OP_QUERYVER   9U
#define VSTOR_OP_QUERYPROP  10U
/* VMSTOR_PROTO_VERSION(MAJOR, MINOR) = ((MAJOR & 0xff) << 8) | (MINOR & 0xff)
 * Win8   = VMSTOR_PROTO_VERSION(5,1) = 0x0501
 * Win8.1 = VMSTOR_PROTO_VERSION(6,0) = 0x0600
 * Win10  = VMSTOR_PROTO_VERSION(6,2) = 0x0602
 */
#define VMSTOR_VER_WIN10  0x00000602U   /* major_minor LE16 | revision LE16 */
#define VMSTOR_VER_WIN8_1 0x00000600U
#define VMSTOR_VER_WIN8   0x00000501U
#define SRB_FLAGS_DISABLE_SYNCH_TRANSFER 0x00000008U
#define SRB_FLAGS_DATA_IN 0x00000040U
#define SRB_FLAGS_DATA_OUT 0x00000080U
#define STORVSC_SENSE_BUFFER_SIZE 0x14U
#define VMSCSI_REQUEST_SIZE 52U

#pragma pack(push, 1)
typedef struct {
    UINT16 type;
    UINT16 offset8;
    UINT16 len8;
    UINT16 flags;
    UINT64 trans_id;
} VSP_PKT_HDR;      /* 16 bytes */

typedef struct {
    VSP_PKT_HDR header;
    UINT32 reserved;
    UINT32 range_count;
    UINT32 byte_count;
    UINT32 byte_offset;
    UINT64 pfn;
} VSP_GPA_DIRECT_1;

typedef struct {
    UINT32 operation;
    UINT32 flags;
    UINT32 status;
    UINT8  payload[52];
} VSP_VSTOR_PKT;    /* 64 bytes */
#pragma pack(pop)

/* Static DMA buffers -- identity-mapped so VA == GPA */
__declspec(align(4096)) static UINT8 vsp_sector_buf[4096];
__declspec(align(512)) static UINT8 vsp_probe_buf[512];
static UINT64 vsp_tid;

/* Multi-device probe table */
static ASAS_STORAGE_DEVICE storage_devices[ASAS_MAX_STORAGE_DEVICES];
static int storage_device_count;

static void ring_put_byte(UINT64 data_page, UINT32 *wi, UINT8 b)
{
    ((UINT8 *)(UINTN)data_page)[*wi] = b;
    *wi = (*wi + 1U) % VSP_RING_DATA_SIZE;
}

static UINT8 ring_get_byte(UINT64 data_page, UINT32 *ri)
{
    UINT8 b = ((volatile UINT8 *)(UINTN)data_page)[*ri];
    *ri = (*ri + 1U) % VSP_RING_DATA_SIZE;
    return b;
}

/* Write a VSTOR packet to the outbound ring (ring_page1 header, ring_page2 data) */
static void vsp_ring_send(const VSP_VSTOR_PKT *vstor, UINT64 tid)
{
    volatile UINT32 *wi_ptr =
        (volatile UINT32 *)(UINTN)status.storvsc_ring_page1;
    UINT64 dp   = status.storvsc_ring_page2;
    UINT32 wi   = *wi_ptr;                                      /* local write index */
    UINT32 w0   = wi;
    UINT32 raw  = sizeof(VSP_PKT_HDR) + sizeof(VSP_VSTOR_PKT); /* 72 */
    UINT32 pad  = ((raw + 7U) & ~7U) - raw;                    /*  0 */
    UINT32 len8 = (raw + pad) / 8U;                            /*  9 */
    UINT64 trailing = (UINT64)w0;
    VSP_PKT_HDR hdr;
    UINT32 i;

    hdr.type     = VMBUS_PKT_INBAND;
    hdr.offset8  = (UINT16)(sizeof(VSP_PKT_HDR) / 8U);
    hdr.len8     = (UINT16)len8;
    hdr.flags    = VMBUS_PKT_ACK;
    hdr.trans_id = tid;

    for (i = 0; i < (UINT32)sizeof(VSP_PKT_HDR); i++)
        ring_put_byte(dp, &wi, ((const UINT8 *)&hdr)[i]);
    for (i = 0; i < (UINT32)sizeof(VSP_VSTOR_PKT); i++)
        ring_put_byte(dp, &wi, ((const UINT8 *)vstor)[i]);
    for (i = 0; i < pad; i++)
        ring_put_byte(dp, &wi, 0);
    for (i = 0; i < VSP_RING_TRAIL; i++)
        ring_put_byte(dp, &wi, ((const UINT8 *)&trailing)[i]);
    memory_fence();
    *wi_ptr = wi;  /* commit write index atomically after all data is written */
}

static void vsp_ring_send_gpa(const VSP_VSTOR_PKT *vstor, UINT64 tid, const void *buffer, UINT32 size)
{
    volatile UINT32 *wi_ptr =
        (volatile UINT32 *)(UINTN)status.storvsc_ring_page1;
    UINT64 dp = status.storvsc_ring_page2;
    UINT32 wi = *wi_ptr;                     /* local write index */
    UINT32 w0 = wi;
    UINT32 desc_size = sizeof(VSP_GPA_DIRECT_1);
    UINT32 raw = desc_size + sizeof(VSP_VSTOR_PKT);
    UINT32 pad = ((raw + 7U) & ~7U) - raw;
    UINT64 trailing = (UINT64)w0;
    VSP_GPA_DIRECT_1 desc;
    UINT32 i;

    zero_bytes(&desc, sizeof(desc));
    desc.header.type = VMBUS_PKT_GPA_DIRECT;
    desc.header.offset8 = (UINT16)(desc_size / 8U);
    desc.header.len8 = (UINT16)((raw + pad) / 8U);
    desc.header.flags = VMBUS_PKT_ACK;
    desc.header.trans_id = tid;
    desc.range_count = 1;
    desc.byte_count = size;
    desc.byte_offset = (UINT32)((UINTN)buffer & 0xFFFU);
    desc.pfn = ((UINT64)(UINTN)buffer) >> 12;

    for (i = 0; i < desc_size; i++)
        ring_put_byte(dp, &wi, ((const UINT8 *)&desc)[i]);
    for (i = 0; i < (UINT32)sizeof(VSP_VSTOR_PKT); i++)
        ring_put_byte(dp, &wi, ((const UINT8 *)vstor)[i]);
    for (i = 0; i < pad; i++)
        ring_put_byte(dp, &wi, 0);
    for (i = 0; i < VSP_RING_TRAIL; i++)
        ring_put_byte(dp, &wi, ((const UINT8 *)&trailing)[i]);
    memory_fence();
    *wi_ptr = wi;  /* commit write index atomically after all data is written */
}

/* Poll inbound ring (ring_page3 header, ring_page4 data) for next response */
static int vsp_ring_recv(VSP_VSTOR_PKT *out, UINT32 spin)
{
    volatile UINT32 *host_wi =
        (volatile UINT32 *)(UINTN)status.storvsc_ring_page3;
    volatile UINT32 *our_ri =
        (volatile UINT32 *)((UINTN)status.storvsc_ring_page3 + 4U);
    UINT64 dp = status.storvsc_ring_page4;
    UINT32 s;

    for (s = 0; s < spin; s++) {
        UINT32 r = *our_ri;
        if (r != *host_wi) {
            VSP_PKT_HDR hdr;
            UINT32 i, total, offset, data_sz, to_copy, skip_prefix, skip_tail;

            for (i = 0; i < (UINT32)sizeof(VSP_PKT_HDR); i++)
                ((UINT8 *)&hdr)[i] = ring_get_byte(dp, &r);

            total   = (UINT32)hdr.len8 * 8U;
            offset  = (UINT32)hdr.offset8 * 8U;
            if (offset < (UINT32)sizeof(VSP_PKT_HDR) || offset > total) {
                return 0;
            }
            skip_prefix = offset - (UINT32)sizeof(VSP_PKT_HDR);
            for (i = 0; i < skip_prefix; i++)
                ring_get_byte(dp, &r);

            data_sz = total - offset;
            to_copy = data_sz < (UINT32)sizeof(VSP_VSTOR_PKT)
                        ? data_sz : (UINT32)sizeof(VSP_VSTOR_PKT);
            skip_tail = data_sz - to_copy;

            for (i = 0; i < to_copy; i++)
                ((UINT8 *)out)[i] = ring_get_byte(dp, &r);
            for (i = 0; i < skip_tail + VSP_RING_TRAIL; i++)
                ring_get_byte(dp, &r);

            memory_fence();
            *our_ri = r;  /* advance consumer index only after all data is read */
            return 1;
        }
    }
    return 0;
}

/* Fast hypercall to notify host of new outbound ring data */
static void vsp_signal(void)
{
    memory_fence();  /* ensure all ring writes are visible before notifying host */
    hyperv_do_hypercall(
        status.hypercall_page,
        HVCALL_SIGNAL_FAST,
        (UINT64)status.storvsc_connection_id,
        0
    );
}

#define SYNTH_KBD_PROTOCOL_REQUEST  1U
#define SYNTH_KBD_PROTOCOL_RESPONSE 2U
#define SYNTH_KBD_EVENT             3U
#define SYNTH_KBD_VERSION           0x00010000U
#define SYNTH_KBD_PROTOCOL_ACCEPTED 1U
#define SYNTH_KBD_IS_BREAK          2U
#define SYNTH_KBD_IS_E0             4U
#define SYNTH_KBD_IS_E1             8U

#pragma pack(push, 1)
typedef struct {
    UINT32 type;
    UINT32 version_requested;
} SYNTH_KBD_PROTOCOL_REQUEST_PKT;

typedef struct {
    UINT32 type;
    UINT32 protocol_status;
} SYNTH_KBD_PROTOCOL_RESPONSE_PKT;

typedef struct {
    UINT32 type;
    UINT16 make_code;
    UINT16 reserved0;
    UINT32 info;
} SYNTH_KBD_EVENT_PKT;
#pragma pack(pop)

static void hyperv_keyboard_signal(void)
{
    memory_fence();  /* ensure all ring writes are visible before notifying host */
    hyperv_do_hypercall(
        status.hypercall_page,
        HVCALL_SIGNAL_FAST,
        (UINT64)status.keyboard_connection_id,
        0
    );
}

static void hyperv_ring_send_inband(
    UINT64 ring_header_page,
    UINT64 ring_data_page,
    const void *payload,
    UINT32 payload_size,
    UINT64 tid
)
{
    volatile UINT32 *wi_ptr = (volatile UINT32 *)(UINTN)ring_header_page;
    UINT32 wi = *wi_ptr;                     /* local write index */
    UINT32 w0 = wi;
    UINT32 raw = sizeof(VSP_PKT_HDR) + payload_size;
    UINT32 pad = ((raw + 7U) & ~7U) - raw;
    UINT64 trailing = (UINT64)w0;
    VSP_PKT_HDR hdr;
    UINT32 i;

    hdr.type = VMBUS_PKT_INBAND;
    hdr.offset8 = (UINT16)(sizeof(VSP_PKT_HDR) / 8U);
    hdr.len8 = (UINT16)((raw + pad) / 8U);
    hdr.flags = VMBUS_PKT_ACK;
    hdr.trans_id = tid;

    for (i = 0; i < (UINT32)sizeof(VSP_PKT_HDR); i++) {
        ring_put_byte(ring_data_page, &wi, ((const UINT8 *)&hdr)[i]);
    }
    for (i = 0; i < payload_size; i++) {
        ring_put_byte(ring_data_page, &wi, ((const UINT8 *)payload)[i]);
    }
    for (i = 0; i < pad; i++) {
        ring_put_byte(ring_data_page, &wi, 0);
    }
    for (i = 0; i < VSP_RING_TRAIL; i++) {
        ring_put_byte(ring_data_page, &wi, ((const UINT8 *)&trailing)[i]);
    }
    memory_fence();
    *wi_ptr = wi;  /* commit write index atomically after all data is written */
}

static int hyperv_ring_recv_payload(
    UINT64 ring_header_page,
    UINT64 ring_data_page,
    void *buffer,
    UINT32 buffer_size,
    UINT32 *payload_size,
    UINT32 spin
)
{
    volatile UINT32 *host_wi = (volatile UINT32 *)(UINTN)ring_header_page;
    volatile UINT32 *our_ri = (volatile UINT32 *)((UINTN)ring_header_page + 4U);
    UINT32 s;

    for (s = 0; s < spin; s++) {
        UINT32 r = *our_ri;
        if (r != *host_wi) {
            VSP_PKT_HDR hdr;
            UINT32 i;
            UINT32 total;
            UINT32 offset;
            UINT32 data_size;
            UINT32 to_copy;
            UINT32 skip_prefix;
            UINT32 skip_tail;

            for (i = 0; i < (UINT32)sizeof(VSP_PKT_HDR); i++) {
                ((UINT8 *)&hdr)[i] = ring_get_byte(ring_data_page, &r);
            }

            total = (UINT32)hdr.len8 * 8U;
            offset = (UINT32)hdr.offset8 * 8U;
            if (offset < (UINT32)sizeof(VSP_PKT_HDR) || offset > total) {
                return 0;
            }

            skip_prefix = offset - (UINT32)sizeof(VSP_PKT_HDR);
            for (i = 0; i < skip_prefix; i++) {
                ring_get_byte(ring_data_page, &r);
            }

            data_size = total - offset;
            to_copy = data_size < buffer_size ? data_size : buffer_size;
            skip_tail = data_size - to_copy;
            for (i = 0; i < to_copy; i++) {
                ((UINT8 *)buffer)[i] = ring_get_byte(ring_data_page, &r);
            }
            for (i = 0; i < skip_tail + VSP_RING_TRAIL; i++) {
                ring_get_byte(ring_data_page, &r);
            }

            memory_fence();
            *our_ri = r;  /* advance consumer index only after all data is read */
            *payload_size = data_size;
            return 1;
        }
    }
    return 0;
}

static int hyperv_keyboard_protocol_initialize(void)
{
    SYNTH_KBD_PROTOCOL_REQUEST_PKT request;
    SYNTH_KBD_PROTOCOL_RESPONSE_PKT response;
    UINT32 response_size = 0;

    if (!status.keyboard_opened || status.keyboard_protocol_ready) {
        return status.keyboard_protocol_ready != 0;
    }

    zero_bytes(&request, sizeof(request));
    request.type = SYNTH_KBD_PROTOCOL_REQUEST;
    request.version_requested = SYNTH_KBD_VERSION;
    hyperv_ring_send_inband(
        status.keyboard_ring_page1,
        status.keyboard_ring_page2,
        &request,
        sizeof(request),
        1
    );
    hyperv_keyboard_signal();

    zero_bytes(&response, sizeof(response));
    if (!hyperv_ring_recv_payload(
        status.keyboard_ring_page3,
        status.keyboard_ring_page4,
        &response,
        sizeof(response),
        &response_size,
        50000000U
    )) {
        logger_write("ERROR", "Hyper-V keyboard protocol response timeout");
        return 0;
    }

    if (response_size >= sizeof(response) && response.type == SYNTH_KBD_PROTOCOL_RESPONSE) {
        status.keyboard_protocol_status = response.protocol_status;
        if ((response.protocol_status & SYNTH_KBD_PROTOCOL_ACCEPTED) != 0) {
            status.keyboard_protocol_ready = 1;
            logger_write("INFO", "Hyper-V keyboard protocol ready");
            return 1;
        }
    }

    logger_write_hex("ERROR", "Hyper-V keyboard protocol rejected", status.keyboard_protocol_status);
    return 0;
}

int hyperv_keyboard_ready(void)
{
    return status.keyboard_protocol_ready != 0;
}

void hyperv_keyboard_poll(void)
{
    SYNTH_KBD_EVENT_PKT event;
    UINT32 payload_size = 0;
    UINT32 poll_count = 0;

    hyperv_hid_poll();
    if (!status.keyboard_protocol_ready) {
        return;
    }

    hyperv_ack_channel_events();
    while (poll_count < 8U && hyperv_ring_recv_payload(
        status.keyboard_ring_page3,
        status.keyboard_ring_page4,
        &event,
        sizeof(event),
        &payload_size,
        1U
    )) {
        UINT8 scancode;

        poll_count++;
        status.keyboard_packet_count++;
        if (payload_size < sizeof(event) || event.type != SYNTH_KBD_EVENT) {
            status.keyboard_bad_packet_count++;
            continue;
        }

        status.keyboard_event_count++;
        status.keyboard_last_make_code = event.make_code;
        status.keyboard_last_info = event.info;
        scancode = (UINT8)(event.make_code & 0xFFU);
        if ((event.info & SYNTH_KBD_IS_E0) != 0) {
            keyboard_inject_scancode(0xE0);
        }
        if ((event.info & SYNTH_KBD_IS_E1) != 0) {
            keyboard_inject_scancode(0xE1);
        }
        if ((event.info & SYNTH_KBD_IS_BREAK) != 0) {
            scancode |= 0x80U;
        }
        keyboard_inject_scancode(scancode);
    }
    hyperv_ack_channel_events();
}

#define PIPE_MESSAGE_DATA              1U
#define SYNTH_HID_PROTOCOL_REQUEST     0U
#define SYNTH_HID_PROTOCOL_RESPONSE    1U
#define SYNTH_HID_INITIAL_DEVICE_INFO  2U
#define SYNTH_HID_INITIAL_DEVICE_INFO_ACK 3U
#define SYNTH_HID_INPUT_REPORT         4U
#define SYNTH_HID_VERSION              0x00020000U

#pragma pack(push, 1)
typedef struct {
    UINT32 type;
    UINT32 size;
} PIPE_MSG_HDR;

typedef struct {
    UINT32 type;
    UINT32 size;
} SYNTH_HID_HDR;

typedef struct {
    PIPE_MSG_HDR pipe;
    SYNTH_HID_HDR hid;
    UINT32 version_requested;
} SYNTH_HID_PROTOCOL_REQUEST_PKT;

typedef struct {
    PIPE_MSG_HDR pipe;
    SYNTH_HID_HDR hid;
    UINT32 version_requested;
    UINT8 approved;
} SYNTH_HID_PROTOCOL_RESPONSE_PKT;

typedef struct {
    PIPE_MSG_HDR pipe;
    SYNTH_HID_HDR hid;
    UINT8 reserved;
} SYNTH_HID_DEVICE_INFO_ACK_PKT;
#pragma pack(pop)

static UINT16 read_le16(const UINT8 *bytes)
{
    return (UINT16)((UINT16)bytes[0] | ((UINT16)bytes[1] << 8U));
}

static void hyperv_hid_signal(void)
{
    memory_fence();  /* ensure all ring writes are visible before notifying host */
    hyperv_do_hypercall(
        status.hypercall_page,
        HVCALL_SIGNAL_FAST,
        (UINT64)status.hid_input_connection_id,
        0
    );
}

static void hyperv_hid_send_device_info_ack(void)
{
    SYNTH_HID_DEVICE_INFO_ACK_PKT ack;

    zero_bytes(&ack, sizeof(ack));
    ack.pipe.type = PIPE_MESSAGE_DATA;
    ack.pipe.size = sizeof(SYNTH_HID_HDR) + 1U;
    ack.hid.type = SYNTH_HID_INITIAL_DEVICE_INFO_ACK;
    ack.hid.size = 1U;
    hyperv_ring_send_inband(
        status.hid_input_ring_page1,
        status.hid_input_ring_page2,
        &ack,
        sizeof(ack),
        2
    );
    hyperv_hid_signal();
    status.hid_input_device_ready = 1;
}

static void hyperv_hid_capture_device_info(const UINT8 *data, UINT32 size)
{
    const UINT8 *info;
    const UINT8 *hid_descriptor;
    UINT32 descriptor_length;
    UINT32 report_desc_length = 0;
    const UINT8 *report_desc;

    if (size < sizeof(SYNTH_HID_HDR) + 32U + 9U) {
        return;
    }

    info = data + sizeof(SYNTH_HID_HDR);
    status.hid_input_vendor = read_le16(info + 4U);
    status.hid_input_product = read_le16(info + 6U);

    hid_descriptor = info + 32U;
    descriptor_length = hid_descriptor[0];
    if (descriptor_length < 9U || sizeof(SYNTH_HID_HDR) + 32U + descriptor_length > size) {
        return;
    }
    if (hid_descriptor[5] != 0U && descriptor_length >= 9U) {
        report_desc_length = read_le16(hid_descriptor + 7U);
    }
    status.hid_input_report_desc_size = report_desc_length;

    report_desc = hid_descriptor + descriptor_length;
    if (sizeof(SYNTH_HID_HDR) + 32U + descriptor_length < size) {
        UINT32 available = size - (sizeof(SYNTH_HID_HDR) + 32U + descriptor_length);

        status.hid_input_report_desc0 = available > 0U ? report_desc[0] : 0;
        status.hid_input_report_desc1 = available > 1U ? report_desc[1] : 0;
        status.hid_input_report_desc2 = available > 2U ? report_desc[2] : 0;
        status.hid_input_report_desc3 = available > 3U ? report_desc[3] : 0;
    }
}

static void hyperv_hid_handle_packet(const UINT8 *payload, UINT32 payload_size)
{
    const PIPE_MSG_HDR *pipe;
    const SYNTH_HID_HDR *hid;

    status.hid_input_packet_count++;
    if (payload_size < sizeof(PIPE_MSG_HDR) + sizeof(SYNTH_HID_HDR)) {
        return;
    }
    pipe = (const PIPE_MSG_HDR *)payload;
    if (pipe->type != PIPE_MESSAGE_DATA || pipe->size + sizeof(PIPE_MSG_HDR) > payload_size) {
        return;
    }
    hid = (const SYNTH_HID_HDR *)(payload + sizeof(PIPE_MSG_HDR));
    if (hid->type == SYNTH_HID_PROTOCOL_RESPONSE) {
        const SYNTH_HID_PROTOCOL_RESPONSE_PKT *response;

        if (payload_size >= sizeof(SYNTH_HID_PROTOCOL_RESPONSE_PKT)) {
            response = (const SYNTH_HID_PROTOCOL_RESPONSE_PKT *)payload;
            if (response->approved != 0) {
                status.hid_input_protocol_ready = 1;
            }
        }
    } else if (hid->type == SYNTH_HID_INITIAL_DEVICE_INFO) {
        hyperv_hid_capture_device_info((const UINT8 *)hid, pipe->size);
        hyperv_hid_send_device_info_ack();
    } else if (hid->type == SYNTH_HID_INPUT_REPORT) {
        const UINT8 *report = payload + sizeof(PIPE_MSG_HDR) + sizeof(SYNTH_HID_HDR);
        UINT32 report_size = hid->size;
        UINT32 report_offset = 0;
        signed char delta_x = 0;
        signed char delta_y = 0;
        UINT8 buttons = 0;

        status.hid_input_report_count++;
        status.hid_input_last_report_size = report_size;
        status.hid_input_last_report0 = report_size > 0 ? report[0] : 0;
        status.hid_input_last_report1 = report_size > 1 ? report[1] : 0;
        status.hid_input_last_report2 = report_size > 2 ? report[2] : 0;
        status.hid_input_last_report3 = report_size > 3 ? report[3] : 0;
        status.hid_input_last_report4 = report_size > 4 ? report[4] : 0;
        status.hid_input_last_report5 = report_size > 5 ? report[5] : 0;
        status.hid_input_last_report6 = report_size > 6 ? report[6] : 0;
        status.hid_input_last_report7 = report_size > 7 ? report[7] : 0;

        if (report_size >= 5U && report[0] <= 7U) {
            UINT32 absolute_x = (UINT32)report[1] | ((UINT32)report[2] << 8U);
            UINT32 absolute_y = (UINT32)report[3] | ((UINT32)report[4] << 8U);

            buttons = report[0] & 0x07U;
            mouse_inject_absolute(absolute_x, absolute_y, buttons);
            return;
        }

        if (report_size >= 4U && report[0] > 7U) {
            report_offset = 1U;
        }
        if (report_size >= report_offset + 3U) {
            buttons = report[report_offset] & 0x07U;
            delta_x = (signed char)report[report_offset + 1U];
            delta_y = (signed char)report[report_offset + 2U];
            if (delta_x > 32) {
                delta_x = 32;
            } else if (delta_x < -32) {
                delta_x = -32;
            }
            if (delta_y > 32) {
                delta_y = 32;
            } else if (delta_y < -32) {
                delta_y = -32;
            }
            if (delta_x != 0 || delta_y != 0 || buttons != mouse_buttons()) {
                mouse_inject_report((long long)delta_x, (long long)(-delta_y), buttons);
            }
        }
    }
}

static int hyperv_hid_protocol_initialize(void)
{
    SYNTH_HID_PROTOCOL_REQUEST_PKT request;
    UINT8 payload[256];
    UINT32 payload_size = 0;
    UINT32 attempts;

    if (!status.hid_input_opened || status.hid_input_protocol_ready) {
        return status.hid_input_protocol_ready != 0;
    }

    zero_bytes(&request, sizeof(request));
    request.pipe.type = PIPE_MESSAGE_DATA;
    request.pipe.size = sizeof(SYNTH_HID_HDR) + sizeof(UINT32);
    request.hid.type = SYNTH_HID_PROTOCOL_REQUEST;
    request.hid.size = sizeof(UINT32);
    request.version_requested = SYNTH_HID_VERSION;
    hyperv_ring_send_inband(
        status.hid_input_ring_page1,
        status.hid_input_ring_page2,
        &request,
        sizeof(request),
        1
    );
    hyperv_hid_signal();

    for (attempts = 0; attempts < 4U; attempts++) {
        if (!hyperv_ring_recv_payload(
            status.hid_input_ring_page3,
            status.hid_input_ring_page4,
            payload,
            sizeof(payload),
            &payload_size,
            50000000U
        )) {
            break;
        }
        hyperv_hid_handle_packet(payload, payload_size);
        if (status.hid_input_protocol_ready && status.hid_input_device_ready) {
            logger_write("INFO", "Hyper-V HID input protocol ready");
            return 1;
        }
    }

    return status.hid_input_protocol_ready != 0;
}

static void hyperv_hid_poll(void)
{
    UINT8 payload[256];
    UINT32 payload_size = 0;
    UINT32 count = 0;

    if (!status.hid_input_opened) {
        return;
    }
    hyperv_ack_channel_events();
    while (count < 8U && hyperv_ring_recv_payload(
        status.hid_input_ring_page3,
        status.hid_input_ring_page4,
        payload,
        sizeof(payload),
        &payload_size,
        1U
    )) {
        count++;
        hyperv_hid_handle_packet(payload, payload_size);
    }
    hyperv_ack_channel_events();
}

int hyperv_vsp_initialize(void)
{
    VSP_VSTOR_PKT pkt;

    if (!status.storvsc_opened) {
        return 0;
    }
    status.vsp_initialized = 0;
    vsp_tid = 1;

    /* 1 -- BEGIN_INITIALIZATION */
    zero_bytes(&pkt, sizeof(pkt));
    pkt.operation = VSTOR_OP_BEGIN;
    vsp_ring_send(&pkt, vsp_tid++);
    vsp_signal();
    if (!vsp_ring_recv(&pkt, 50000000U) || pkt.operation != VSTOR_OP_COMPLETE) {
        logger_write("ERROR", "StorVSC VSP: BEGIN_INIT no response");
        return 0;
    }

    /* 2 -- QUERY_PROTOCOL_VERSION
     * Try Win10 → Win8.1 → Win8 in order (mirrors Linux storvsc_drv.c).
     * The payload holds struct vmstorage_protocol_version { u16 major_minor; u16 revision; }.
     * Encoding: VMSTOR_PROTO_VERSION(MAJOR,MINOR) = ((MAJOR & 0xff) << 8) | (MINOR & 0xff)
     * We write the version as the first u32 of the payload (revision=0).
     */
    {
        static const UINT32 try_ver[] = { VMSTOR_VER_WIN10, VMSTOR_VER_WIN8_1, VMSTOR_VER_WIN8 };
        UINT32 vi;
        int ver_ok = 0;
        for (vi = 0; vi < 3U; vi++) {
            zero_bytes(&pkt, sizeof(pkt));
            pkt.operation = VSTOR_OP_QUERYVER;
            /* major_minor at payload[0..1], revision=0 at payload[2..3] */
            pkt.payload[0] = (UINT8)(try_ver[vi]);
            pkt.payload[1] = (UINT8)(try_ver[vi] >> 8U);
            vsp_ring_send(&pkt, vsp_tid++);
            vsp_signal();
            if (!vsp_ring_recv(&pkt, 50000000U) || pkt.operation != VSTOR_OP_COMPLETE) {
                logger_write("ERROR", "StorVSC VSP: QUERY_VERSION no response");
                return 0;
            }
            logger_write_hex("INFO", "StorVSC VSP: QUERY_VERSION status", pkt.status);
            if (pkt.status == 0) {
                logger_write_hex("INFO", "StorVSC VSP: protocol version accepted", try_ver[vi]);
                ver_ok = 1;
                break;
            }
        }
        if (!ver_ok) {
            logger_write("ERROR", "StorVSC VSP: no supported protocol version");
            return 0;
        }
    }

    /* 3 -- QUERY_PROPERTIES */
    zero_bytes(&pkt, sizeof(pkt));
    pkt.operation = VSTOR_OP_QUERYPROP;
    vsp_ring_send(&pkt, vsp_tid++);
    vsp_signal();
    if (!vsp_ring_recv(&pkt, 50000000U) || pkt.operation != VSTOR_OP_COMPLETE) {
        logger_write("ERROR", "StorVSC VSP: QUERY_PROPS no response");
        return 0;
    }

    /* 4 -- END_INITIALIZATION */
    zero_bytes(&pkt, sizeof(pkt));
    pkt.operation = VSTOR_OP_END;
    vsp_ring_send(&pkt, vsp_tid++);
    vsp_signal();
    if (!vsp_ring_recv(&pkt, 50000000U) || pkt.operation != VSTOR_OP_COMPLETE) {
        logger_write("ERROR", "StorVSC VSP: END_INIT no response");
        return 0;
    }

    status.vsp_initialized = 1;
    logger_write("INFO", "StorVSC VSP protocol initialized");
    return 1;
}

static void build_srb_packet(VSP_VSTOR_PKT *pkt, UINT64 lba, UINT32 count,
                             UINT32 block_size, UINT32 write)
{
    UINT8 *p;

    zero_bytes(pkt, sizeof(*pkt));
    pkt->operation = VSTOR_OP_SRB;
    pkt->flags = 1;   /* REQUEST_COMPLETION_FLAG */
    p = pkt->payload;

    /*
     * struct vmscsi_request layout (verified from Linux storvsc_drv.c):
     *   p[ 0.. 1]  length         (u16 LE) = sizeof(vmscsi_request)
     *   p[ 2]      srb_status
     *   p[ 3]      scsi_status
     *   p[ 4]      port_number
     *   p[ 5]      path_id
     *   p[ 6]      target_id      ← set by caller after build
     *   p[ 7]      lun            ← set by caller after build
     *   p[ 8]      cdb_length
     *   p[ 9]      sense_info_length
     *   p[10]      data_in        (0=WRITE, 1=READ, 2=UNSPECIFIED)
     *   p[11]      reserved
     *   p[12..15]  data_transfer_length  (u32 LE)  ← DMA byte count
     *   p[16..31]  cdb[16]               ← SCSI command
     *   -- Win8 extension (packed, immediately after cdb union) --
     *   p[36..37]  reserve        (u16)
     *   p[38]      queue_tag      = 0xFF (SP_UNTAGGED)
     *   p[39]      queue_action   = 0x20 (SRB_SIMPLE_TAG_REQUEST)
     *   p[40..43]  srb_flags      (u32 LE)
     *   p[44..47]  time_out_value (u32 LE)
     *   p[48..51]  queue_sort_key (u32 LE)
     */

    /* length = sizeof(vmscsi_request) = 52 bytes (header 20 + cdb-union 16 + win8 16) */
    p[0] = VMSCSI_REQUEST_SIZE; p[1] = 0;

    /* p[4..7] = 0 (port/path=0, target/lun set by caller) */

    p[8]  = 10;                        /* cdb_length */
    p[9]  = STORVSC_SENSE_BUFFER_SIZE;
    p[10] = write ? 0 : 1;            /* data_in: 0=WRITE, 1=READ */

    {
        UINT32 bytes = count * block_size;
        p[12] = (UINT8)bytes; p[13] = (UINT8)(bytes >> 8);
        p[14] = (UINT8)(bytes >> 16); p[15] = (UINT8)(bytes >> 24);
    }

    /* CDB: READ(10) or WRITE(10) at p[16..25] */
    p[16] = write ? 0x2A : 0x28;
    p[17] = 0x00;
    p[18] = (UINT8)(lba >> 24U);
    p[19] = (UINT8)(lba >> 16U);
    p[20] = (UINT8)(lba >>  8U);
    p[21] = (UINT8)(lba);
    p[22] = 0x00;
    p[23] = (UINT8)(count >> 8);
    p[24] = (UINT8)count;
    p[25] = 0x00;

    /* Win8 extension at p[36..51] */
    p[38] = 0xFF;   /* queue_tag   = SP_UNTAGGED */
    p[39] = 0x20;   /* queue_action = SRB_SIMPLE_TAG_REQUEST */
    {
        UINT32 flags = (write ? SRB_FLAGS_DATA_OUT : SRB_FLAGS_DATA_IN) |
            SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
        p[40] = (UINT8)(flags);
        p[41] = (UINT8)(flags >>  8U);
        p[42] = (UINT8)(flags >> 16U);
        p[43] = (UINT8)(flags >> 24U);
    }
    /* time_out_value = 60 at p[44..47] */
    p[44] = 60; p[45] = 0; p[46] = 0; p[47] = 0;
}

/* Generic SRB builder for probe commands (variable CDB / transfer length) */
static void build_srb_generic(VSP_VSTOR_PKT *pkt, UINT8 target, UINT8 lun,
                               const UINT8 *cdb, UINT8 cdb_len,
                               UINT8 data_dir, UINT32 transfer_len)
{
    UINT8 *p;
    UINT32 i;
    UINT32 flags;

    zero_bytes(pkt, sizeof(*pkt));
    pkt->operation = VSTOR_OP_SRB;
    pkt->flags = 1;
    p = pkt->payload;

    p[0] = VMSCSI_REQUEST_SIZE; p[1] = 0;
    p[4] = 0; p[5] = 0;
    p[6] = target;
    p[7] = lun;
    p[8] = cdb_len;
    p[9] = STORVSC_SENSE_BUFFER_SIZE;
    p[10] = data_dir;            /* 0=data out/none, 1=data in, 2=unspecified */
    p[12] = (UINT8)(transfer_len);
    p[13] = (UINT8)(transfer_len >> 8U);
    p[14] = (UINT8)(transfer_len >> 16U);
    p[15] = (UINT8)(transfer_len >> 24U);
    for (i = 0; i < (UINT32)cdb_len && i < 16U; i++) {
        p[16U + i] = cdb[i];
    }
    p[38] = 0xFF;
    p[39] = 0x20;
    flags = (data_dir == 0U && transfer_len != 0U ? SRB_FLAGS_DATA_OUT :
             data_dir == 1U ? SRB_FLAGS_DATA_IN : 0U) |
            SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
    p[40] = (UINT8)(flags);
    p[41] = (UINT8)(flags >> 8U);
    p[42] = (UINT8)(flags >> 16U);
    p[43] = (UINT8)(flags >> 24U);
    p[44] = 60;
}

/* Probe one SCSI target:lun via INQUIRY + READ CAPACITY(10).
   Returns 1 if a device was found, 0 otherwise.             */
static int hyperv_storage_probe_one(UINT8 target, UINT8 lun,
                                    ASAS_STORAGE_DEVICE *dev)
{
    VSP_VSTOR_PKT pkt;
    UINT8 cdb[10];
    UINT32 i;

    if (!status.vsp_initialized) return 0;

    /* Zero probe buffer */
    for (i = 0; i < 512U; i++) vsp_probe_buf[i] = 0;
    for (i = 0; i < 10U; i++) cdb[i] = 0;

    /* SCSI INQUIRY (6-byte CDB) */
    cdb[0] = 0x12;   /* INQUIRY */
    cdb[4] = 36;     /* allocation length */
    build_srb_generic(&pkt, target, lun, cdb, 6, 1, 36);
    vsp_ring_send_gpa(&pkt, vsp_tid++, vsp_probe_buf, 512U);
    vsp_signal();

    if (!vsp_ring_recv(&pkt, 10000000U)) return 0;   /* probe timeout */
    if (pkt.operation != VSTOR_OP_COMPLETE || pkt.status != 0) return 0;
    if (pkt.payload[2] != 1 || pkt.payload[3] != 0) return 0;   /* SRB/SCSI ok */

    /* Device present.  Peripheral device type byte 0 bits[4:0]:
       0x00 = direct access (disk), 0x05 = CD/DVD            */
    dev->target    = target;
    dev->lun       = lun;
    dev->is_cdrom  = ((vsp_probe_buf[0] & 0x1FU) == 0x05U) ? 1U : 0U;
    dev->valid     = 1;
    dev->sector_count = 0;
    dev->sector_size  = 512;

    /* READ CAPACITY(10) -- 10-byte CDB */
    for (i = 0; i < 512U; i++) vsp_probe_buf[i] = 0;
    for (i = 0; i < 10U; i++) cdb[i] = 0;
    cdb[0] = 0x25;   /* READ CAPACITY(10) */
    build_srb_generic(&pkt, target, lun, cdb, 10, 1, 8);
    vsp_ring_send_gpa(&pkt, vsp_tid++, vsp_probe_buf, 512U);
    vsp_signal();

    if (vsp_ring_recv(&pkt, 10000000U) &&   /* probe timeout */
        pkt.operation == VSTOR_OP_COMPLETE && pkt.status == 0 &&
        pkt.payload[2] == 1 && pkt.payload[3] == 0)
    {
        /* 8-byte response: last_lba (BE32) + block_size (BE32) */
        UINT32 last_lba  = ((UINT32)vsp_probe_buf[0] << 24U) |
                           ((UINT32)vsp_probe_buf[1] << 16U) |
                           ((UINT32)vsp_probe_buf[2] <<  8U) |
                            (UINT32)vsp_probe_buf[3];
        UINT32 blk_size  = ((UINT32)vsp_probe_buf[4] << 24U) |
                           ((UINT32)vsp_probe_buf[5] << 16U) |
                           ((UINT32)vsp_probe_buf[6] <<  8U) |
                            (UINT32)vsp_probe_buf[7];
        dev->sector_count = last_lba + 1U;
        dev->sector_size  = blk_size ? blk_size : 512U;
    }
    return 1;
}

static int hyperv_storage_transfer(UINT8 target, UINT8 lun, UINT64 lba,
                                   UINT32 count, UINT32 block_size,
                                   void *buffer, UINT32 write)
{
    VSP_VSTOR_PKT pkt;
    UINT32 i;
    UINT32 attempt;

    UINT32 bytes = count * block_size;
    if (!status.vsp_initialized || count == 0 || block_size < 512U ||
        block_size > 4096U || bytes > sizeof(vsp_sector_buf)) {
        return 0;
    }

    if (write) {
        for (i = 0; i < bytes; i++) {
            vsp_sector_buf[i] = ((const UINT8 *)buffer)[i];
        }
    } else {
        for (i = 0; i < bytes; i++) {
            vsp_sector_buf[i] = 0;
        }
    }

    /* Retry loop: SCSI CHECK_CONDITION (unit attention) is normal on first
       access after StorVSC init — retry up to 3 times like Linux does. */
    for (attempt = 0; attempt < 3U; attempt++) {
        if (attempt > 0U) {
            /* Short spin delay before retry */
            UINT32 spin;
            for (spin = 0; spin < 2000000U; spin++) { (void)spin; }
        }

        build_srb_packet(&pkt, lba, count, block_size, write);
        pkt.payload[6] = target;
        pkt.payload[7] = lun;
        vsp_ring_send_gpa(&pkt, vsp_tid++, vsp_sector_buf, bytes);
        vsp_signal();

        if (!vsp_ring_recv(&pkt, 100000000U)) {
            logger_write("ERROR", write ? "StorVSC WRITE: timeout" : "StorVSC READ: timeout");
            return 0;
        }
        status.last_vstor_status = pkt.status;
        status.last_srb_status   = pkt.payload[2];
        status.last_scsi_status  = pkt.payload[3];

        if (pkt.operation != VSTOR_OP_COMPLETE || pkt.status != 0) {
            logger_write_hex("ERROR",
                write ? "StorVSC WRITE: failed" : "StorVSC READ: failed",
                (UINT64)pkt.status);
            return 0;
        }

        if (pkt.payload[2] != 1) {
            /* SRB-level error — not retryable */
            logger_write_hex("ERROR",
                write ? "StorVSC WRITE SRB failed" : "StorVSC READ SRB failed",
                pkt.payload[2]);
            return 0;
        }

        if (pkt.payload[3] != 0) {
            /* SCSI-level error (e.g. CHECK_CONDITION = unit attention on first access).
               Log and retry; Linux retries these automatically. */
            logger_write_hex("WARN",
                write ? "StorVSC WRITE SCSI status (retry)" : "StorVSC READ SCSI status (retry)",
                pkt.payload[3]);
            if (attempt < 2U) continue;
            /* Exhausted retries */
            return 0;
        }

        /* Success — transfer data and return */
        if (write) {
            status.raw_write_supported = 1;
        } else {
            for (i = 0; i < bytes; i++) {
                ((UINT8 *)buffer)[i] = vsp_sector_buf[i];
            }
            status.raw_read_ready = 1;
        }
        return 1;
    }
    return 0;
}

static int hyperv_storage_transfer_sector(UINT8 target, UINT8 lun,
                                          UINT64 lba, void *buffer, UINT32 write)
{
    return hyperv_storage_transfer(target, lun, lba, 1, 512U, buffer, write);
}

int hyperv_storage_read_sector(UINT64 lba, void *buffer)
{
    return hyperv_storage_transfer_sector(0, 0, lba, buffer, 0);
}

int hyperv_storage_write_sector(UINT64 lba, const void *buffer)
{
    return hyperv_storage_transfer_sector(0, 0, lba, (void *)buffer, 1);
}

int hyperv_storage_read_sector_ex(UINT8 target, UINT8 lun, UINT64 lba, void *buffer)
{
    return hyperv_storage_transfer_sector(target, lun, lba, buffer, 0);
}

int hyperv_storage_write_sector_ex(UINT8 target, UINT8 lun, UINT64 lba,
                                   const void *buffer)
{
    return hyperv_storage_transfer_sector(target, lun, lba, (void *)buffer, 1);
}

static int hyperv_block_read(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                             UINT32 count, void *buffer)
{
    UINT32 chunk;
    UINT8 *bytes = (UINT8 *)buffer;
    while (count != 0) {
        UINT32 max_blocks = (UINT32)sizeof(vsp_sector_buf) /
                            device->logical_block_size;
        chunk = count > max_blocks ? max_blocks : count;
        if (!hyperv_storage_transfer(device->target, device->lun, lba,
                                     chunk, device->logical_block_size,
                                     bytes, 0)) return 0;
        lba += chunk;
        bytes += (UINT64)chunk * device->logical_block_size;
        count -= chunk;
    }
    return 1;
}

static int hyperv_block_write(ASAS_BLOCK_DEVICE *device, UINT64 lba,
                              UINT32 count, const void *buffer)
{
    UINT32 chunk;
    const UINT8 *bytes = (const UINT8 *)buffer;
    while (count != 0) {
        UINT32 max_blocks = (UINT32)sizeof(vsp_sector_buf) /
                            device->logical_block_size;
        chunk = count > max_blocks ? max_blocks : count;
        if (!hyperv_storage_transfer(device->target, device->lun, lba,
                                     chunk, device->logical_block_size,
                                     (void *)bytes, 1)) return 0;
        lba += chunk;
        bytes += (UINT64)chunk * device->logical_block_size;
        count -= chunk;
    }
    return 1;
}

static int hyperv_block_flush(ASAS_BLOCK_DEVICE *device)
{
    (void)device;
    return 1;
}

static const ASAS_BLOCK_DEVICE_OPS hyperv_block_ops = {
    hyperv_block_read,
    hyperv_block_write,
    hyperv_block_flush
};

int hyperv_storage_register_block_device(UINT8 target, UINT8 lun)
{
    ASAS_BLOCK_DEVICE description = { 0 };
    const ASAS_STORAGE_DEVICE *geometry = 0;
    int index;
    UINT32 number = 0;
    if (!hyperv_storage_detected() || !status.vsp_initialized ||
        target > 9U || lun > 9U) return 0;
    for (index = 0; index < (int)block_device_count(); index++) {
        ASAS_BLOCK_DEVICE *device = block_device_get((UINT32)index);
        if (device == 0 || device->parent != 0) continue;
        if (device->name[0] == 'h' && device->name[1] == 'y' &&
            device->name[2] == 'p' && device->name[3] == 'e' &&
            device->name[4] == 'r' && device->name[5] == 'v') {
            if (device->target == target && device->lun == lun) return 0;
            number++;
        }
    }
    description.name[0] = 'h';
    description.name[1] = 'y';
    description.name[2] = 'p';
    description.name[3] = 'e';
    description.name[4] = 'r';
    description.name[5] = 'v';
    description.name[6] = (char)('0' + (number % 10U));
    (void)hyperv_storage_probe_devices();
    for (index = 0; index < storage_device_count; index++) {
        if (storage_devices[index].target == target &&
            storage_devices[index].lun == lun) {
            geometry = &storage_devices[index];
            break;
        }
    }
    if (geometry == 0 || geometry->sector_size < 512U ||
        geometry->sector_size > 4096U) return 0;
    description.logical_block_size = geometry->sector_size;
    description.physical_block_size = geometry->sector_size;
    description.block_count = geometry->sector_count;
    description.flags = geometry->is_cdrom
        ? (BLOCK_DEVICE_FLAG_OPTICAL | BLOCK_DEVICE_FLAG_READ_ONLY)
        : 0;
    description.target = target;
    description.lun = lun;
    description.ops = &hyperv_block_ops;
    if (block_device_register(&description) == 0) return 0;
    logger_write("INFO", "Hyper-V direct block device registered");
    return 1;
}

/* Probe storage devices by trying READ(10) on all (target,lun) pairs 0..3x0..3.
   No assumptions about SCSI addressing — works with any Hyper-V configuration.
   Idempotent: returns immediately if already probed. */
int hyperv_storage_probe_devices(void)
{
    UINT8 target, lun;
    if (storage_device_count > 0) return storage_device_count;
    storage_device_count = 0;

    for (target = 0; target < 4U; target++) {
        for (lun = 0; lun < 4U; lun++) {
            ASAS_STORAGE_DEVICE dev;
            if (storage_device_count >= ASAS_MAX_STORAGE_DEVICES) break;
            zero_bytes(&dev, sizeof(dev));
            if (!hyperv_storage_probe_one(target, lun, &dev)) continue;
            storage_devices[storage_device_count] = dev;
            storage_device_count++;
            logger_write_hex("INFO", "StorVSC: device at target", target);
            logger_write_hex("INFO", "StorVSC: device at lun",    lun);
        }
    }
    logger_write_hex("INFO", "StorVSC: total devices", (UINT64)(UINT32)storage_device_count);
    return storage_device_count;
}

int hyperv_storage_rescan_devices(void)
{
    storage_device_count = 0;
    return hyperv_storage_probe_devices();
}

int hyperv_storage_get_device_count(void)
{
    return storage_device_count;
}

const ASAS_STORAGE_DEVICE *hyperv_storage_get_devices(void)
{
    return storage_devices;
}

void hyperv_storage_note_device(UINT8 target, UINT8 lun, UINT8 is_cdrom,
                                UINT32 sector_count, UINT32 sector_size)
{
    int index;
    if (target > 9U || lun > 9U) return;
    if (sector_size == 0) sector_size = 512U;
    for (index = 0; index < storage_device_count; index++) {
        if (storage_devices[index].target == target &&
            storage_devices[index].lun == lun) {
            storage_devices[index].valid = 1;
            if (sector_count != 0) storage_devices[index].sector_count = sector_count;
            storage_devices[index].sector_size = sector_size;
            if (is_cdrom) storage_devices[index].is_cdrom = 1;
            return;
        }
    }
    if (storage_device_count >= ASAS_MAX_STORAGE_DEVICES) return;
    storage_devices[storage_device_count].valid = 1;
    storage_devices[storage_device_count].target = target;
    storage_devices[storage_device_count].lun = lun;
    storage_devices[storage_device_count].is_cdrom = is_cdrom ? 1U : 0U;
    storage_devices[storage_device_count].sector_count = sector_count;
    storage_devices[storage_device_count].sector_size = sector_size;
    storage_device_count++;
}

void hyperv_storage_interrupt_handler(void)
{
    hyperv_interrupt_count++;
    apic_eoi();
}
