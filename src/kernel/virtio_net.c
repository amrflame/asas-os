#include "virtio_net.h"
#include "logger.h"

#pragma intrinsic(__inbyte)
unsigned char __inbyte(unsigned short port);
#pragma intrinsic(__inword)
unsigned short __inword(unsigned short port);
#pragma intrinsic(__indword)
unsigned long __indword(unsigned short port);
#pragma intrinsic(__outbyte)
void __outbyte(unsigned short port, unsigned char value);
#pragma intrinsic(__outword)
void __outword(unsigned short port, unsigned short value);
#pragma intrinsic(__outdword)
void __outdword(unsigned short port, unsigned long value);

#define VIRTIO_PCI_HOST_FEATURES 0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN 0x08
#define VIRTIO_PCI_QUEUE_NUM 0x0C
#define VIRTIO_PCI_QUEUE_SELECT 0x0E
#define VIRTIO_PCI_STATUS 0x12
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10
#define VIRTIO_PCI_CONFIG 0x14
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_NET_F_MAC (1U << 5)
#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTIO_NET_RX_BUFFER_COUNT 8
#define VIRTIO_NET_FRAME_SIZE 1514
#define VIRTIO_NET_BUFFER_SIZE 2048
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IPV4 0x0800
#define IPV4_PROTOCOL_ICMP 1
#define IPV4_PROTOCOL_UDP 17
#define IPV4_PROTOCOL_TCP 6
#define ASAS_NET_IP0 10
#define ASAS_NET_IP1 0
#define ASAS_NET_IP2 2
#define ASAS_NET_IP3 15
#define ASAS_GATEWAY_IP0 10
#define ASAS_GATEWAY_IP1 0
#define ASAS_GATEWAY_IP2 2
#define ASAS_GATEWAY_IP3 2
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_XID 0x41534153U
#define DNS_SERVER_IP0 10
#define DNS_SERVER_IP1 0
#define DNS_SERVER_IP2 2
#define DNS_SERVER_IP3 3
#define DNS_CLIENT_PORT 49152
#define DNS_SERVER_PORT 53
#define DNS_QUERY_ID 0x4153
#define TCP_HTTP_SOURCE_PORT 49153
#define TCP_HTTP_DESTINATION_PORT 80
#define TCP_SEQUENCE_NUMBER 0x41534153U
#define TCP_SERVER_SEQUENCE_NUMBER 0x48545450U
#define TCP_CONNECTION_CAPACITY 4
#define TCP_DEFERRED_PAYLOAD_SIZE 512

#pragma pack(push, 1)
typedef struct {
    UINT64 address;
    UINT32 length;
    UINT16 flags;
    UINT16 next;
} VIRTQ_DESCRIPTOR;

typedef struct {
    UINT16 flags;
    volatile UINT16 index;
    UINT16 ring[256];
} VIRTQ_AVAILABLE;

typedef struct {
    UINT32 id;
    UINT32 length;
} VIRTQ_USED_ELEMENT;

typedef struct {
    UINT16 flags;
    volatile UINT16 index;
    VIRTQ_USED_ELEMENT ring[256];
} VIRTQ_USED;

typedef struct {
    UINT8 flags;
    UINT8 gso_type;
    UINT16 header_length;
    UINT16 gso_size;
    UINT16 checksum_start;
    UINT16 checksum_offset;
} VIRTIO_NET_HEADER;
#pragma pack(pop)

__declspec(align(4096)) static UINT8 rx_queue_memory[16384];
__declspec(align(4096)) static UINT8 tx_queue_memory[16384];
__declspec(align(16)) static VIRTIO_NET_HEADER tx_header;
__declspec(align(16)) static UINT8 tx_frame[VIRTIO_NET_FRAME_SIZE];
__declspec(align(16)) static UINT8 rx_buffers[VIRTIO_NET_RX_BUFFER_COUNT][VIRTIO_NET_BUFFER_SIZE];
__declspec(align(16)) static UINT8 dhcp_frame[VIRTIO_NET_FRAME_SIZE];
__declspec(align(16)) static UINT8 http_frame[VIRTIO_NET_FRAME_SIZE];

static VIRTQ_DESCRIPTOR *rx_descriptors;
static VIRTQ_AVAILABLE *rx_available;
static VIRTQ_USED *rx_used;
static VIRTQ_DESCRIPTOR *tx_descriptors;
static VIRTQ_AVAILABLE *tx_available;
static VIRTQ_USED *tx_used;
static UINT16 rx_used_index;
static UINT16 tx_used_index;
static ASAS_VIRTIO_NET *active_net;
static UINT8 pending_ipv4_target[4];
static UINT8 pending_target_mac[6];
static UINT8 pending_arp_reply_received;
static UINT8 pending_icmp_reply_received;
static UINT16 next_tcp_source_port = TCP_HTTP_SOURCE_PORT;
static UINT8 cached_http_response[1024];
static UINT32 cached_http_response_size;
static UINT8 http_server_active;
static UINT8 http_server_request_received;
static UINT8 http_server_response_sent;

typedef struct {
    UINT8 active;
    UINT16 source_port;
    UINT16 destination_port;
    UINT32 peer_sequence;
    UINT8 syn_ack_received;
    UINT8 response_received;
    UINT8 *output;
    UINT32 output_capacity;
    UINT32 output_size;
    UINT8 deferred_payload[TCP_DEFERRED_PAYLOAD_SIZE];
    UINT32 deferred_payload_size;
} ASAS_TCP_CONNECTION;

static ASAS_TCP_CONNECTION tcp_connections[TCP_CONNECTION_CAPACITY];

extern void memory_fence(void);

static int send_ethernet_frame(ASAS_VIRTIO_NET *net, const UINT8 *frame, UINT32 length);
static UINT32 build_tcp_segment(
    const ASAS_VIRTIO_NET *net,
    const UINT8 target_ip[4],
    const UINT8 next_hop_mac[6],
    UINT16 source_port,
    UINT16 destination_port,
    UINT32 sequence,
    UINT32 acknowledgment,
    UINT8 flags,
    const UINT8 *payload,
    UINT32 payload_length,
    UINT8 *frame
);

static void clear_bytes(void *buffer, UINTN size)
{
    UINT8 *bytes = (UINT8 *)buffer;
    UINTN index;

    for (index = 0; index < size; index++) {
        bytes[index] = 0;
    }
}

static UINT16 find_legacy_io_base(const ASAS_PCI_DEVICE *device)
{
    UINT32 bar_index;

    for (bar_index = 0; bar_index < 6; bar_index++) {
        if ((device->bars[bar_index] & 1U) != 0) {
            UINT16 io_base = (UINT16)(device->bars[bar_index] & ~3U);

            if (io_base == 0) {
                io_base = 0xC100;
                pci_write_bar(device, bar_index, (UINT32)io_base | 1U);
            }
            return io_base;
        }
    }
    return 0;
}

static UINT16 initialize_queue(UINT16 io_base, UINT16 queue_index, void *queue_memory)
{
    UINT16 queue_size;

    __outword(io_base + VIRTIO_PCI_QUEUE_SELECT, queue_index);
    queue_size = __inword(io_base + VIRTIO_PCI_QUEUE_NUM);
    if (queue_size == 0 || queue_size > 256) {
        return 0;
    }
    clear_bytes(queue_memory, 16384);
    __outdword(io_base + VIRTIO_PCI_QUEUE_PFN, (UINT32)((UINTN)queue_memory >> 12));
    return queue_size;
}

static void bind_queue(
    UINT8 *queue_memory,
    UINT16 queue_size,
    VIRTQ_DESCRIPTOR **descriptors,
    VIRTQ_AVAILABLE **available,
    VIRTQ_USED **used
)
{
    UINTN available_offset = sizeof(VIRTQ_DESCRIPTOR) * queue_size;
    UINTN used_offset = (available_offset + 6 + sizeof(UINT16) * queue_size + 4095) & ~4095ULL;

    *descriptors = (VIRTQ_DESCRIPTOR *)&queue_memory[0];
    *available = (VIRTQ_AVAILABLE *)&queue_memory[available_offset];
    *used = (VIRTQ_USED *)&queue_memory[used_offset];
}

static void copy_bytes(void *destination, const void *source, UINTN size)
{
    UINT8 *destination_bytes = (UINT8 *)destination;
    const UINT8 *source_bytes = (const UINT8 *)source;
    UINTN index;

    for (index = 0; index < size; index++) {
        destination_bytes[index] = source_bytes[index];
    }
}

static void tcp_connections_reset(void)
{
    UINT32 index;

    for (index = 0; index < TCP_CONNECTION_CAPACITY; index++) {
        tcp_connections[index].active = 0;
        tcp_connections[index].source_port = 0;
        tcp_connections[index].destination_port = 0;
        tcp_connections[index].peer_sequence = 0;
        tcp_connections[index].syn_ack_received = 0;
        tcp_connections[index].response_received = 0;
        tcp_connections[index].output = 0;
        tcp_connections[index].output_capacity = 0;
        tcp_connections[index].output_size = 0;
        tcp_connections[index].deferred_payload_size = 0;
    }
}

static ASAS_TCP_CONNECTION *tcp_connection_allocate(UINT16 destination_port)
{
    UINT32 index;
    ASAS_TCP_CONNECTION *connection;

    for (index = 0; index < TCP_CONNECTION_CAPACITY; index++) {
        if (!tcp_connections[index].active) {
            connection = &tcp_connections[index];
            connection->active = 1;
            connection->source_port = next_tcp_source_port++;
            if (next_tcp_source_port < TCP_HTTP_SOURCE_PORT) {
                next_tcp_source_port = TCP_HTTP_SOURCE_PORT;
            }
            connection->destination_port = destination_port;
            connection->peer_sequence = 0;
            connection->syn_ack_received = 0;
            connection->response_received = 0;
            connection->output = 0;
            connection->output_capacity = 0;
            connection->output_size = 0;
            connection->deferred_payload_size = 0;
            return connection;
        }
    }
    return 0;
}

static ASAS_TCP_CONNECTION *tcp_connection_find(UINT16 source_port, UINT16 destination_port)
{
    UINT32 index;

    for (index = 0; index < TCP_CONNECTION_CAPACITY; index++) {
        if (
            tcp_connections[index].active &&
            tcp_connections[index].destination_port == source_port &&
            tcp_connections[index].source_port == destination_port
        ) {
            return &tcp_connections[index];
        }
    }
    return 0;
}

static void tcp_connection_set_output(
    ASAS_TCP_CONNECTION *connection,
    UINT8 *output,
    UINT32 output_capacity
)
{
    UINT32 copy_length;

    connection->output = output;
    connection->output_capacity = output_capacity;
    connection->output_size = 0;

    if (connection->deferred_payload_size != 0 && output != 0 && output_capacity != 0) {
        copy_length = connection->deferred_payload_size;
        if (copy_length > output_capacity) {
            copy_length = output_capacity;
        }
        copy_bytes(output, connection->deferred_payload, copy_length);
        connection->output_size = copy_length;
    }
}

static void tcp_connection_store_payload(
    ASAS_TCP_CONNECTION *connection,
    const UINT8 *payload,
    UINT32 payload_length
)
{
    UINT32 copy_length = payload_length;

    if (connection->output != 0 && connection->output_capacity != 0) {
        if (copy_length > connection->output_capacity) {
            copy_length = connection->output_capacity;
        }
        copy_bytes(connection->output, payload, copy_length);
        connection->output_size = copy_length;
    } else {
        if (copy_length > TCP_DEFERRED_PAYLOAD_SIZE) {
            copy_length = TCP_DEFERRED_PAYLOAD_SIZE;
        }
        copy_bytes(connection->deferred_payload, payload, copy_length);
        connection->deferred_payload_size = copy_length;
    }
    connection->response_received = 1;
}

static void tcp_connection_release(ASAS_TCP_CONNECTION *connection)
{
    connection->active = 0;
    connection->output = 0;
    connection->output_capacity = 0;
    connection->output_size = 0;
    connection->deferred_payload_size = 0;
}

static void write_be16(UINT8 *bytes, UINT16 value)
{
    bytes[0] = (UINT8)(value >> 8);
    bytes[1] = (UINT8)value;
}

static UINT16 read_be16(const UINT8 *bytes)
{
    return (UINT16)(((UINT16)bytes[0] << 8) | bytes[1]);
}

static void write_be32(UINT8 *bytes, UINT32 value)
{
    bytes[0] = (UINT8)(value >> 24);
    bytes[1] = (UINT8)(value >> 16);
    bytes[2] = (UINT8)(value >> 8);
    bytes[3] = (UINT8)value;
}

static UINT32 read_be32(const UINT8 *bytes)
{
    return
        ((UINT32)bytes[0] << 24) |
        ((UINT32)bytes[1] << 16) |
        ((UINT32)bytes[2] << 8) |
        (UINT32)bytes[3];
}

static UINT16 internet_checksum(const UINT8 *bytes, UINT32 length)
{
    UINT32 sum = 0;
    UINT32 index;

    for (index = 0; index + 1 < length; index += 2) {
        sum += ((UINT16)bytes[index] << 8) | bytes[index + 1];
    }
    if ((length & 1U) != 0) {
        sum += (UINT16)bytes[length - 1] << 8;
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (UINT16)~sum;
}

static UINT16 tcp_checksum(
    const UINT8 source_ip[4],
    const UINT8 destination_ip[4],
    const UINT8 *tcp,
    UINT32 tcp_length
)
{
    UINT32 sum = 0;
    UINT32 index;

    for (index = 0; index < 4; index += 2) {
        sum += ((UINT16)source_ip[index] << 8) | source_ip[index + 1];
        sum += ((UINT16)destination_ip[index] << 8) | destination_ip[index + 1];
    }
    sum += IPV4_PROTOCOL_TCP;
    sum += tcp_length;
    for (index = 0; index + 1 < tcp_length; index += 2) {
        sum += ((UINT16)tcp[index] << 8) | tcp[index + 1];
    }
    if ((tcp_length & 1U) != 0) {
        sum += (UINT16)tcp[tcp_length - 1] << 8;
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (UINT16)~sum;
}

static void post_receive_buffers(ASAS_VIRTIO_NET *net)
{
    UINT16 index;

    for (index = 0; index < VIRTIO_NET_RX_BUFFER_COUNT; index++) {
        rx_descriptors[index].address = (UINT64)(UINTN)&rx_buffers[index][0];
        rx_descriptors[index].length = VIRTIO_NET_BUFFER_SIZE;
        rx_descriptors[index].flags = VIRTQ_DESC_F_WRITE;
        rx_descriptors[index].next = 0;
        rx_available->ring[rx_available->index % net->rx_queue_size] = index;
        rx_available->index++;
    }
    memory_fence();
    __outword(net->io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);
    net->rx_ready = 1;
}

static void recycle_receive_buffer(ASAS_VIRTIO_NET *net, UINT16 descriptor_index)
{
    rx_available->ring[rx_available->index % net->rx_queue_size] = descriptor_index;
    memory_fence();
    rx_available->index++;
    memory_fence();
    __outword(net->io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);
}

static int is_net_ip(const ASAS_VIRTIO_NET *net, const UINT8 *ip)
{
    return
        ip[0] == net->ip[0] &&
        ip[1] == net->ip[1] &&
        ip[2] == net->ip[2] &&
        ip[3] == net->ip[3];
}

static int is_gateway_ip(const UINT8 *ip)
{
    return
        ip[0] == ASAS_GATEWAY_IP0 &&
        ip[1] == ASAS_GATEWAY_IP1 &&
        ip[2] == ASAS_GATEWAY_IP2 &&
        ip[3] == ASAS_GATEWAY_IP3;
}

static int ip_equals(const UINT8 *left, const UINT8 *right)
{
    return
        left[0] == right[0] &&
        left[1] == right[1] &&
        left[2] == right[2] &&
        left[3] == right[3];
}

static void handle_arp_frame(ASAS_VIRTIO_NET *net, const UINT8 *frame, UINT32 length)
{
    const UINT8 *arp;

    if (length < 42) {
        return;
    }
    arp = &frame[14];
    if (
        read_be16(&arp[0]) != 1 ||
        read_be16(&arp[2]) != ETHERTYPE_IPV4 ||
        arp[4] != 6 ||
        arp[5] != 4 ||
        read_be16(&arp[6]) != 2 ||
        !ip_equals(&arp[14], pending_ipv4_target) ||
        !is_net_ip(net, &arp[24])
    ) {
        return;
    }
    copy_bytes(pending_target_mac, &arp[8], sizeof(pending_target_mac));
    pending_arp_reply_received = 1;
    copy_bytes(net->gateway_mac, &arp[8], sizeof(net->gateway_mac));
    if (is_gateway_ip(&arp[14])) {
        net->arp_reply_received = 1;
    }
}

static void handle_dhcp_packet(ASAS_VIRTIO_NET *net, const UINT8 *payload, UINT32 length)
{
    UINT32 options_index = 240;
    UINT8 message_type = 0;
    UINT8 server_ip[4] = { 0, 0, 0, 0 };

    if (
        length < 244 ||
        payload[0] != 2 ||
        read_be32(&payload[4]) != DHCP_XID ||
        payload[236] != 0x63 ||
        payload[237] != 0x82 ||
        payload[238] != 0x53 ||
        payload[239] != 0x63
    ) {
        return;
    }
    while (options_index < length) {
        UINT8 option = payload[options_index++];
        UINT8 option_length;

        if (option == 0) {
            continue;
        }
        if (option == 255 || options_index >= length) {
            break;
        }
        option_length = payload[options_index++];
        if (options_index + option_length > length) {
            break;
        }
        if (option == 53 && option_length == 1) {
            message_type = payload[options_index];
        } else if (option == 54 && option_length == 4) {
            copy_bytes(server_ip, &payload[options_index], sizeof(server_ip));
        }
        options_index += option_length;
    }
    if (message_type == 2) {
        copy_bytes(net->ip, &payload[16], sizeof(net->ip));
        copy_bytes(net->dhcp_server_ip, server_ip, sizeof(net->dhcp_server_ip));
        net->dhcp_offer_received = 1;
    } else if (message_type == 5) {
        copy_bytes(net->ip, &payload[16], sizeof(net->ip));
        copy_bytes(net->dhcp_server_ip, server_ip, sizeof(net->dhcp_server_ip));
        net->dhcp_ack_received = 1;
    }
}

static UINT32 skip_dns_name(const UINT8 *payload, UINT32 length, UINT32 offset)
{
    while (offset < length) {
        UINT8 label_length = payload[offset++];

        if (label_length == 0) {
            return offset;
        }
        if ((label_length & 0xC0) == 0xC0) {
            return offset + 1 <= length ? offset + 1 : length;
        }
        offset += label_length;
    }
    return length;
}

static void handle_dns_packet(ASAS_VIRTIO_NET *net, const UINT8 *payload, UINT32 length)
{
    UINT32 offset;
    UINT16 question_count;
    UINT16 answer_count;
    UINT16 answer_index;

    if (length < 12 || read_be16(&payload[0]) != DNS_QUERY_ID) {
        return;
    }
    question_count = read_be16(&payload[4]);
    answer_count = read_be16(&payload[6]);
    offset = 12;
    while (question_count != 0 && offset < length) {
        offset = skip_dns_name(payload, length, offset);
        if (offset + 4 > length) {
            return;
        }
        offset += 4;
        question_count--;
    }
    for (answer_index = 0; answer_index < answer_count && offset < length; answer_index++) {
        UINT16 type;
        UINT16 dns_class;
        UINT16 data_length;

        offset = skip_dns_name(payload, length, offset);
        if (offset + 10 > length) {
            return;
        }
        type = read_be16(&payload[offset]);
        dns_class = read_be16(&payload[offset + 2]);
        data_length = read_be16(&payload[offset + 8]);
        offset += 10;
        if (offset + data_length > length) {
            return;
        }
        if (type == 1 && dns_class == 1 && data_length == 4) {
            copy_bytes(net->dns_resolved_ip, &payload[offset], sizeof(net->dns_resolved_ip));
            net->dns_a_record_received = 1;
            return;
        }
        offset += data_length;
    }
}

static void handle_http_server_tcp(
    ASAS_VIRTIO_NET *net,
    const UINT8 *frame,
    const UINT8 *ipv4,
    const UINT8 *tcp,
    UINT32 tcp_payload_length,
    UINT16 source_port,
    UINT8 flags
)
{
    static const UINT8 response[] = {
        'H','T','T','P','/','1','.','0',' ','2','0','0',' ','O','K','\r','\n',
        'C','o','n','t','e','n','t','-','L','e','n','g','t','h',':',' ','1','4','\r','\n',
        '\r','\n',
        'A','s','a','s',' ','h','t','t','p',' ','o','k','\r','\n'
    };
    UINT8 client_ip[4];
    UINT8 client_mac[6];
    UINT32 client_sequence = read_be32(&tcp[4]);
    UINT32 length;

    if (!http_server_active || !is_net_ip(net, &ipv4[16]) || read_be16(&tcp[2]) != TCP_HTTP_DESTINATION_PORT) {
        return;
    }
    copy_bytes(client_ip, &ipv4[12], sizeof(client_ip));
    copy_bytes(client_mac, &frame[6], sizeof(client_mac));
    if ((flags & 0x02) != 0 && (flags & 0x10) == 0) {
        length = build_tcp_segment(
            net,
            client_ip,
            client_mac,
            TCP_HTTP_DESTINATION_PORT,
            source_port,
            TCP_SERVER_SEQUENCE_NUMBER,
            client_sequence + 1,
            0x12,
            0,
            0,
            http_frame
        );
        (void)send_ethernet_frame(net, http_frame, length);
        return;
    }
    if (tcp_payload_length != 0) {
        http_server_request_received = 1;
        length = build_tcp_segment(
            net,
            client_ip,
            client_mac,
            TCP_HTTP_DESTINATION_PORT,
            source_port,
            TCP_SERVER_SEQUENCE_NUMBER + 1,
            client_sequence + tcp_payload_length,
            0x18,
            response,
            sizeof(response),
            http_frame
        );
        if (send_ethernet_frame(net, http_frame, length)) {
            http_server_response_sent = 1;
        }
    }
}

static void handle_ipv4_frame(ASAS_VIRTIO_NET *net, const UINT8 *frame, UINT32 length)
{
    const UINT8 *ipv4;
    UINT32 header_length;

    if (length < 34) {
        return;
    }
    ipv4 = &frame[14];
    if (
        (ipv4[0] >> 4) != 4 ||
        (
            ipv4[9] != IPV4_PROTOCOL_ICMP &&
            ipv4[9] != IPV4_PROTOCOL_UDP &&
            ipv4[9] != IPV4_PROTOCOL_TCP
        )
    ) {
        return;
    }
    header_length = (UINT32)(ipv4[0] & 0x0F) * 4;
    if (header_length < 20 || length < 14 + header_length + 8) {
        return;
    }
    if (ipv4[9] == IPV4_PROTOCOL_UDP) {
        const UINT8 *udp = &ipv4[header_length];
        UINT16 source_port = read_be16(&udp[0]);
        UINT16 destination_port = read_be16(&udp[2]);
        UINT16 udp_length = read_be16(&udp[4]);

        if (
            source_port == DHCP_SERVER_PORT &&
            destination_port == DHCP_CLIENT_PORT &&
            udp_length >= 8 &&
            length >= 14 + header_length + udp_length
        ) {
            handle_dhcp_packet(net, &udp[8], (UINT32)udp_length - 8);
        } else if (
            source_port == DNS_SERVER_PORT &&
            destination_port == DNS_CLIENT_PORT &&
            udp_length >= 20 &&
            length >= 14 + header_length + udp_length &&
            read_be16(&udp[8]) == DNS_QUERY_ID
        ) {
            net->dns_response_received = 1;
            handle_dns_packet(net, &udp[8], (UINT32)udp_length - 8);
        }
        return;
    }
    if (ipv4[9] == IPV4_PROTOCOL_TCP) {
        const UINT8 *tcp = &ipv4[header_length];
        UINT32 total_length = read_be16(&ipv4[2]);
        UINT32 tcp_header_length;
        UINT32 tcp_payload_length;
        UINT16 source_port = read_be16(&tcp[0]);
        UINT16 destination_port = read_be16(&tcp[2]);
        UINT8 flags = tcp[13];

        if (length < 14 + total_length || total_length < header_length + 20) {
            return;
        }
        tcp_header_length = (UINT32)(tcp[12] >> 4) * 4;
        if (tcp_header_length < 20 || total_length < header_length + tcp_header_length) {
            return;
        }
        tcp_payload_length = total_length - header_length - tcp_header_length;
        handle_http_server_tcp(
            net,
            frame,
            ipv4,
            tcp,
            tcp_payload_length,
            source_port,
            flags
        );
        {
            ASAS_TCP_CONNECTION *connection = tcp_connection_find(source_port, destination_port);

            if (connection != 0 && (flags & 0x12) == 0x12) {
                connection->peer_sequence = read_be32(&tcp[4]);
                connection->syn_ack_received = 1;
                net->tcp_peer_sequence = connection->peer_sequence;
                net->tcp_syn_ack_received = 1;
            } else if (connection != 0 && tcp_payload_length != 0) {
                tcp_connection_store_payload(
                    connection,
                    &tcp[tcp_header_length],
                    tcp_payload_length
                );
                net->http_response_received = 1;
            }
        }
        return;
    }
    if (
        ip_equals(&ipv4[12], pending_ipv4_target) &&
        is_net_ip(net, &ipv4[16])
    ) {
        const UINT8 *icmp = &ipv4[header_length];

        if (icmp[0] == 0 && read_be16(&icmp[4]) == 0x4153) {
            pending_icmp_reply_received = 1;
            if (is_gateway_ip(&ipv4[12])) {
                net->icmp_reply_received = 1;
            }
        }
    }
}

static int poll_receive(ASAS_VIRTIO_NET *net)
{
    int handled = 0;

    while (rx_used->index != rx_used_index) {
        VIRTQ_USED_ELEMENT used_element = rx_used->ring[rx_used_index % net->rx_queue_size];
        UINT16 descriptor_index = (UINT16)used_element.id;

        memory_fence();
        if (
            descriptor_index < VIRTIO_NET_RX_BUFFER_COUNT &&
            used_element.length > sizeof(VIRTIO_NET_HEADER)
        ) {
            UINT8 *frame = &rx_buffers[descriptor_index][sizeof(VIRTIO_NET_HEADER)];
            UINT32 frame_length = used_element.length - sizeof(VIRTIO_NET_HEADER);
            UINT16 ethertype = frame_length >= 14 ? read_be16(&frame[12]) : 0;

            net->rx_poll_verified = 1;
            if (ethertype == ETHERTYPE_ARP) {
                handle_arp_frame(net, frame, frame_length);
            } else if (ethertype == ETHERTYPE_IPV4) {
                handle_ipv4_frame(net, frame, frame_length);
            }
            handled = 1;
            recycle_receive_buffer(net, descriptor_index);
        }
        rx_used_index++;
    }
    return handled;
}

static int send_ethernet_frame(ASAS_VIRTIO_NET *net, const UINT8 *frame, UINT32 length)
{
    UINT16 previous_used_index;
    UINT32 timeout;

    if (length == 0 || length > sizeof(tx_frame)) {
        return 0;
    }
    clear_bytes(&tx_header, sizeof(tx_header));
    clear_bytes(tx_frame, sizeof(tx_frame));
    copy_bytes(tx_frame, frame, length);

    previous_used_index = tx_used->index;
    tx_descriptors[0].address = (UINT64)(UINTN)&tx_header;
    tx_descriptors[0].length = sizeof(tx_header);
    tx_descriptors[0].flags = VIRTQ_DESC_F_NEXT;
    tx_descriptors[0].next = 1;
    tx_descriptors[1].address = (UINT64)(UINTN)&tx_frame[0];
    tx_descriptors[1].length = length;
    tx_descriptors[1].flags = 0;
    tx_descriptors[1].next = 0;

    tx_available->ring[tx_available->index % net->tx_queue_size] = 0;
    memory_fence();
    tx_available->index++;
    memory_fence();
    __outword(net->io_base + VIRTIO_PCI_QUEUE_NOTIFY, 1);

    for (timeout = 0; timeout < 100000000U; timeout++) {
        if (tx_used->index != previous_used_index) {
            memory_fence();
            tx_used_index = tx_used->index;
            net->ethernet_tx_verified = 1;
            return 1;
        }
    }
    logger_write("ERROR", "VirtIO network transmit timed out");
    return 0;
}

static UINT32 build_test_ethernet_frame(const ASAS_VIRTIO_NET *net, UINT8 *frame)
{
    UINT32 index;
    static const UINT8 fallback_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    const UINT8 *source = net->has_mac ? net->mac : fallback_mac;

    for (index = 0; index < 6; index++) {
        frame[index] = 0xFF;
        frame[6 + index] = source[index];
    }
    frame[12] = 0x88;
    frame[13] = 0xB5;
    frame[14] = 'A';
    frame[15] = 'S';
    frame[16] = 'A';
    frame[17] = 'S';
    frame[18] = 'N';
    frame[19] = 'E';
    frame[20] = 'T';
    frame[21] = 0;
    return 60;
}

static UINT32 build_arp_request(const ASAS_VIRTIO_NET *net, const UINT8 target_ip[4], UINT8 *frame)
{
    UINT32 index;
    static const UINT8 fallback_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    const UINT8 *source = net->has_mac ? net->mac : fallback_mac;

    for (index = 0; index < 6; index++) {
        frame[index] = 0xFF;
        frame[6 + index] = source[index];
    }
    write_be16(&frame[12], ETHERTYPE_ARP);
    write_be16(&frame[14], 1);
    write_be16(&frame[16], ETHERTYPE_IPV4);
    frame[18] = 6;
    frame[19] = 4;
    write_be16(&frame[20], 1);
    copy_bytes(&frame[22], source, 6);
    frame[28] = net->ip[0];
    frame[29] = net->ip[1];
    frame[30] = net->ip[2];
    frame[31] = net->ip[3];
    for (index = 0; index < 6; index++) {
        frame[32 + index] = 0;
    }
    frame[38] = target_ip[0];
    frame[39] = target_ip[1];
    frame[40] = target_ip[2];
    frame[41] = target_ip[3];
    return 60;
}

static UINT32 build_icmp_echo_request(
    const ASAS_VIRTIO_NET *net,
    const UINT8 target_ip[4],
    const UINT8 target_mac[6],
    UINT8 *frame
)
{
    UINT32 index;
    UINT16 checksum;
    static const UINT8 fallback_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    const UINT8 *source = net->has_mac ? net->mac : fallback_mac;

    for (index = 0; index < 6; index++) {
        frame[index] = target_mac[index];
        frame[6 + index] = source[index];
    }
    write_be16(&frame[12], ETHERTYPE_IPV4);

    frame[14] = 0x45;
    frame[15] = 0;
    write_be16(&frame[16], 28);
    write_be16(&frame[18], 1);
    write_be16(&frame[20], 0);
    frame[22] = 64;
    frame[23] = IPV4_PROTOCOL_ICMP;
    write_be16(&frame[24], 0);
    frame[26] = net->ip[0];
    frame[27] = net->ip[1];
    frame[28] = net->ip[2];
    frame[29] = net->ip[3];
    frame[30] = target_ip[0];
    frame[31] = target_ip[1];
    frame[32] = target_ip[2];
    frame[33] = target_ip[3];
    checksum = internet_checksum(&frame[14], 20);
    write_be16(&frame[24], checksum);

    frame[34] = 8;
    frame[35] = 0;
    write_be16(&frame[36], 0);
    write_be16(&frame[38], 0x4153);
    write_be16(&frame[40], 1);
    checksum = internet_checksum(&frame[34], 8);
    write_be16(&frame[36], checksum);
    return 60;
}

static UINT32 build_dhcp_packet(
    const ASAS_VIRTIO_NET *net,
    UINT8 message_type,
    const UINT8 requested_ip[4],
    const UINT8 server_ip[4],
    UINT8 *frame
)
{
    UINT32 index;
    UINT32 options_index;
    UINT32 dhcp_length;
    UINT16 ipv4_length;
    UINT16 udp_length;
    UINT16 checksum;
    static const UINT8 fallback_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    const UINT8 *source = net->has_mac ? net->mac : fallback_mac;
    UINT8 *ipv4 = &frame[14];
    UINT8 *udp = &frame[34];
    UINT8 *dhcp = &frame[42];

    clear_bytes(frame, VIRTIO_NET_FRAME_SIZE);
    for (index = 0; index < 6; index++) {
        frame[index] = 0xFF;
        frame[6 + index] = source[index];
    }
    write_be16(&frame[12], ETHERTYPE_IPV4);

    dhcp[0] = 1;
    dhcp[1] = 1;
    dhcp[2] = 6;
    dhcp[3] = 0;
    write_be32(&dhcp[4], DHCP_XID);
    write_be16(&dhcp[10], 0x8000);
    copy_bytes(&dhcp[28], source, 6);
    dhcp[236] = 0x63;
    dhcp[237] = 0x82;
    dhcp[238] = 0x53;
    dhcp[239] = 0x63;
    options_index = 240;
    dhcp[options_index++] = 53;
    dhcp[options_index++] = 1;
    dhcp[options_index++] = message_type;
    if (message_type == 3) {
        dhcp[options_index++] = 50;
        dhcp[options_index++] = 4;
        copy_bytes(&dhcp[options_index], requested_ip, 4);
        options_index += 4;
        dhcp[options_index++] = 54;
        dhcp[options_index++] = 4;
        copy_bytes(&dhcp[options_index], server_ip, 4);
        options_index += 4;
    }
    dhcp[options_index++] = 55;
    dhcp[options_index++] = 3;
    dhcp[options_index++] = 1;
    dhcp[options_index++] = 3;
    dhcp[options_index++] = 6;
    dhcp[options_index++] = 255;
    dhcp_length = options_index;
    udp_length = (UINT16)(8 + dhcp_length);
    ipv4_length = (UINT16)(20 + udp_length);

    ipv4[0] = 0x45;
    ipv4[1] = 0;
    write_be16(&ipv4[2], ipv4_length);
    write_be16(&ipv4[4], message_type);
    write_be16(&ipv4[6], 0);
    ipv4[8] = 64;
    ipv4[9] = IPV4_PROTOCOL_UDP;
    write_be16(&ipv4[10], 0);
    ipv4[12] = 0;
    ipv4[13] = 0;
    ipv4[14] = 0;
    ipv4[15] = 0;
    ipv4[16] = 255;
    ipv4[17] = 255;
    ipv4[18] = 255;
    ipv4[19] = 255;
    checksum = internet_checksum(ipv4, 20);
    write_be16(&ipv4[10], checksum);

    write_be16(&udp[0], DHCP_CLIENT_PORT);
    write_be16(&udp[2], DHCP_SERVER_PORT);
    write_be16(&udp[4], udp_length);
    write_be16(&udp[6], 0);
    return 14 + ipv4_length < 60 ? 60 : 14 + ipv4_length;
}

static UINT32 build_dns_query(
    const ASAS_VIRTIO_NET *net,
    const UINT8 dns_ip[4],
    const UINT8 dns_mac[6],
    UINT8 *frame
)
{
    UINT32 index;
    UINT32 dns_index;
    UINT16 udp_length;
    UINT16 ipv4_length;
    UINT16 checksum;
    static const UINT8 fallback_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    const UINT8 *source = net->has_mac ? net->mac : fallback_mac;
    UINT8 *ipv4 = &frame[14];
    UINT8 *udp = &frame[34];
    UINT8 *dns = &frame[42];

    clear_bytes(frame, VIRTIO_NET_FRAME_SIZE);
    for (index = 0; index < 6; index++) {
        frame[index] = dns_mac[index];
        frame[6 + index] = source[index];
    }
    write_be16(&frame[12], ETHERTYPE_IPV4);

    write_be16(&dns[0], DNS_QUERY_ID);
    write_be16(&dns[2], 0x0100);
    write_be16(&dns[4], 1);
    write_be16(&dns[6], 0);
    write_be16(&dns[8], 0);
    write_be16(&dns[10], 0);
    dns_index = 12;
    dns[dns_index++] = 7;
    dns[dns_index++] = 'e';
    dns[dns_index++] = 'x';
    dns[dns_index++] = 'a';
    dns[dns_index++] = 'm';
    dns[dns_index++] = 'p';
    dns[dns_index++] = 'l';
    dns[dns_index++] = 'e';
    dns[dns_index++] = 3;
    dns[dns_index++] = 'c';
    dns[dns_index++] = 'o';
    dns[dns_index++] = 'm';
    dns[dns_index++] = 0;
    write_be16(&dns[dns_index], 1);
    dns_index += 2;
    write_be16(&dns[dns_index], 1);
    dns_index += 2;

    udp_length = (UINT16)(8 + dns_index);
    ipv4_length = (UINT16)(20 + udp_length);
    ipv4[0] = 0x45;
    ipv4[1] = 0;
    write_be16(&ipv4[2], ipv4_length);
    write_be16(&ipv4[4], 0x4453);
    write_be16(&ipv4[6], 0);
    ipv4[8] = 64;
    ipv4[9] = IPV4_PROTOCOL_UDP;
    write_be16(&ipv4[10], 0);
    copy_bytes(&ipv4[12], net->ip, 4);
    copy_bytes(&ipv4[16], dns_ip, 4);
    checksum = internet_checksum(ipv4, 20);
    write_be16(&ipv4[10], checksum);

    write_be16(&udp[0], DNS_CLIENT_PORT);
    write_be16(&udp[2], DNS_SERVER_PORT);
    write_be16(&udp[4], udp_length);
    write_be16(&udp[6], 0);
    return 14 + ipv4_length < 60 ? 60 : 14 + ipv4_length;
}

static UINT32 build_tcp_segment(
    const ASAS_VIRTIO_NET *net,
    const UINT8 target_ip[4],
    const UINT8 next_hop_mac[6],
    UINT16 source_port,
    UINT16 destination_port,
    UINT32 sequence,
    UINT32 acknowledgment,
    UINT8 flags,
    const UINT8 *payload,
    UINT32 payload_length,
    UINT8 *frame
)
{
    UINT32 index;
    UINT16 checksum;
    UINT16 tcp_length;
    UINT16 ipv4_length;
    UINT8 *ipv4 = &frame[14];
    UINT8 *tcp = &frame[34];
    static const UINT8 fallback_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    const UINT8 *source = net->has_mac ? net->mac : fallback_mac;

    clear_bytes(frame, VIRTIO_NET_FRAME_SIZE);
    for (index = 0; index < 6; index++) {
        frame[index] = next_hop_mac[index];
        frame[6 + index] = source[index];
    }
    write_be16(&frame[12], ETHERTYPE_IPV4);
    tcp_length = (UINT16)(20 + payload_length);
    ipv4_length = (UINT16)(20 + tcp_length);
    ipv4[0] = 0x45;
    ipv4[1] = 0;
    write_be16(&ipv4[2], ipv4_length);
    write_be16(&ipv4[4], 0x5443);
    write_be16(&ipv4[6], 0);
    ipv4[8] = 64;
    ipv4[9] = IPV4_PROTOCOL_TCP;
    write_be16(&ipv4[10], 0);
    copy_bytes(&ipv4[12], net->ip, 4);
    copy_bytes(&ipv4[16], target_ip, 4);
    checksum = internet_checksum(ipv4, 20);
    write_be16(&ipv4[10], checksum);

    write_be16(&tcp[0], source_port);
    write_be16(&tcp[2], destination_port);
    write_be32(&tcp[4], sequence);
    write_be32(&tcp[8], acknowledgment);
    tcp[12] = 0x50;
    tcp[13] = flags;
    write_be16(&tcp[14], 64240);
    write_be16(&tcp[16], 0);
    write_be16(&tcp[18], 0);
    if (payload_length != 0 && payload != 0) {
        copy_bytes(&tcp[20], payload, payload_length);
    }
    checksum = tcp_checksum(net->ip, target_ip, tcp, tcp_length);
    write_be16(&tcp[16], checksum);
    return 14 + ipv4_length < 60 ? 60 : 14 + ipv4_length;
}

int virtio_net_initialize(const ASAS_PCI_DEVICE *device, ASAS_VIRTIO_NET *net)
{
    UINT32 host_features;
    UINT32 index;

    net->io_base = find_legacy_io_base(device);
    net->rx_queue_size = 0;
    net->tx_queue_size = 0;
    net->has_mac = 0;
    net->initialized = 0;
    net->rx_ready = 0;
    net->tx_ready = 0;
    net->ethernet_tx_verified = 0;
    net->arp_tx_verified = 0;
    net->icmp_tx_verified = 0;
    net->rx_poll_verified = 0;
    net->arp_reply_received = 0;
    net->icmp_reply_received = 0;
    net->dhcp_discover_sent = 0;
    net->dhcp_offer_received = 0;
    net->dhcp_request_sent = 0;
    net->dhcp_ack_received = 0;
    net->dns_query_sent = 0;
    net->dns_response_received = 0;
    net->dns_a_record_received = 0;
    net->tcp_syn_sent = 0;
    net->tcp_syn_ack_received = 0;
    net->tcp_ack_sent = 0;
    net->http_get_sent = 0;
    net->http_response_received = 0;
    net->tcp_peer_sequence = 0;
    tcp_connections_reset();
    for (index = 0; index < sizeof(net->mac); index++) {
        net->mac[index] = 0;
    }
    for (index = 0; index < sizeof(net->gateway_mac); index++) {
        net->gateway_mac[index] = 0;
    }
    net->ip[0] = ASAS_NET_IP0;
    net->ip[1] = ASAS_NET_IP1;
    net->ip[2] = ASAS_NET_IP2;
    net->ip[3] = ASAS_NET_IP3;
    for (index = 0; index < sizeof(net->dhcp_server_ip); index++) {
        net->dhcp_server_ip[index] = 0;
    }
    for (index = 0; index < sizeof(net->dns_server_mac); index++) {
        net->dns_server_mac[index] = 0;
    }
    for (index = 0; index < sizeof(net->dns_resolved_ip); index++) {
        net->dns_resolved_ip[index] = 0;
    }
    if (net->io_base == 0) {
        return 0;
    }

    pci_enable_bus_mastering(device);
    __outbyte(net->io_base + VIRTIO_PCI_STATUS, 0);
    __outbyte(net->io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    __outbyte(
        net->io_base + VIRTIO_PCI_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
    );

    host_features = __indword(net->io_base + VIRTIO_PCI_HOST_FEATURES);
    __outdword(net->io_base + VIRTIO_PCI_GUEST_FEATURES, host_features & VIRTIO_NET_F_MAC);

    if ((host_features & VIRTIO_NET_F_MAC) != 0) {
        for (index = 0; index < sizeof(net->mac); index++) {
            net->mac[index] = __inbyte((UINT16)(net->io_base + VIRTIO_PCI_CONFIG + index));
        }
        net->has_mac = 1;
    }

    net->rx_queue_size = initialize_queue(net->io_base, 0, rx_queue_memory);
    net->tx_queue_size = initialize_queue(net->io_base, 1, tx_queue_memory);
    if (net->rx_queue_size == 0 || net->tx_queue_size == 0) {
        logger_write("ERROR", "VirtIO network queue initialization failed");
        return 0;
    }
    bind_queue(rx_queue_memory, net->rx_queue_size, &rx_descriptors, &rx_available, &rx_used);
    bind_queue(tx_queue_memory, net->tx_queue_size, &tx_descriptors, &tx_available, &tx_used);
    rx_used_index = rx_used->index;
    tx_used_index = tx_used->index;
    net->tx_ready = 1;

    __outbyte(
        net->io_base + VIRTIO_PCI_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK
    );
    post_receive_buffers(net);
    net->initialized = 1;
    active_net = net;
    return 1;
}

static int virtio_net_dhcp_self_test(ASAS_VIRTIO_NET *net)
{
    UINT8 offered_ip[4];
    UINT32 length;
    UINT32 timeout;

    length = build_dhcp_packet(net, 1, net->ip, net->dhcp_server_ip, dhcp_frame);
    if (!send_ethernet_frame(net, dhcp_frame, length)) {
        return 0;
    }
    net->dhcp_discover_sent = 1;
    for (timeout = 0; timeout < 100000000U && !net->dhcp_offer_received; timeout++) {
        (void)poll_receive(net);
    }
    if (!net->dhcp_offer_received) {
        logger_write("ERROR", "DHCP offer was not received");
        return 0;
    }
    copy_bytes(offered_ip, net->ip, sizeof(offered_ip));
    net->dhcp_ack_received = 0;
    length = build_dhcp_packet(net, 3, offered_ip, net->dhcp_server_ip, dhcp_frame);
    if (!send_ethernet_frame(net, dhcp_frame, length)) {
        return 0;
    }
    net->dhcp_request_sent = 1;
    for (timeout = 0; timeout < 100000000U && !net->dhcp_ack_received; timeout++) {
        (void)poll_receive(net);
    }
    if (!net->dhcp_ack_received) {
        logger_write("ERROR", "DHCP ack was not received");
        return 0;
    }
    return 1;
}

static int resolve_mac_ipv4(ASAS_VIRTIO_NET *net, const UINT8 target_ip[4], UINT8 target_mac[6])
{
    UINT8 frame[VIRTIO_NET_FRAME_SIZE];
    UINT32 length;
    UINT32 timeout;

    copy_bytes(pending_ipv4_target, target_ip, sizeof(pending_ipv4_target));
    clear_bytes(pending_target_mac, sizeof(pending_target_mac));
    pending_arp_reply_received = 0;
    length = build_arp_request(net, target_ip, frame);
    if (!send_ethernet_frame(net, frame, length)) {
        return 0;
    }
    net->arp_tx_verified = 1;
    for (timeout = 0; timeout < 100000000U && !pending_arp_reply_received; timeout++) {
        (void)poll_receive(net);
    }
    if (!pending_arp_reply_received) {
        return 0;
    }
    copy_bytes(target_mac, pending_target_mac, 6);
    return 1;
}

static int virtio_net_dns_self_test(ASAS_VIRTIO_NET *net)
{
    UINT8 dns_ip[4] = {
        DNS_SERVER_IP0,
        DNS_SERVER_IP1,
        DNS_SERVER_IP2,
        DNS_SERVER_IP3
    };
    UINT32 length;
    UINT32 timeout;

    if (!resolve_mac_ipv4(net, dns_ip, net->dns_server_mac)) {
        logger_write("ERROR", "DNS server ARP resolution failed");
        return 0;
    }
    net->dns_response_received = 0;
    length = build_dns_query(net, dns_ip, net->dns_server_mac, dhcp_frame);
    if (!send_ethernet_frame(net, dhcp_frame, length)) {
        return 0;
    }
    net->dns_query_sent = 1;
    for (timeout = 0; timeout < 100000000U && !net->dns_response_received; timeout++) {
        (void)poll_receive(net);
    }
    if (!net->dns_response_received) {
        logger_write("ERROR", "DNS response was not received");
    }
    return 1;
}

static int virtio_net_tcp_self_test(ASAS_VIRTIO_NET *net)
{
    static const UINT8 http_get[] = {
        'G','E','T',' ','/',' ','H','T','T','P','/','1','.','0','\r','\n',
        'H','o','s','t',':',' ','e','x','a','m','p','l','e','.','c','o','m','\r','\n',
        '\r','\n'
    };
    UINT32 length;
    UINT32 timeout;
    ASAS_TCP_CONNECTION *connection;

    if (!net->dns_a_record_received || !net->arp_reply_received) {
        return 0;
    }
    connection = tcp_connection_allocate(TCP_HTTP_DESTINATION_PORT);
    if (connection == 0) {
        return 0;
    }
    connection->source_port = TCP_HTTP_SOURCE_PORT;
    net->tcp_syn_ack_received = 0;
    length = build_tcp_segment(
        net,
        net->dns_resolved_ip,
        net->gateway_mac,
        connection->source_port,
        TCP_HTTP_DESTINATION_PORT,
        TCP_SEQUENCE_NUMBER,
        0,
        0x02,
        0,
        0,
        dhcp_frame
    );
    if (!send_ethernet_frame(net, dhcp_frame, length)) {
        return 0;
    }
    net->tcp_syn_sent = 1;
    for (timeout = 0; timeout < 100000000U && !connection->syn_ack_received; timeout++) {
        (void)poll_receive(net);
    }
    if (!connection->syn_ack_received) {
        logger_write("ERROR", "TCP SYN ACK was not received");
        tcp_connection_release(connection);
        return 0;
    }
    length = build_tcp_segment(
        net,
        net->dns_resolved_ip,
        net->gateway_mac,
        connection->source_port,
        TCP_HTTP_DESTINATION_PORT,
        TCP_SEQUENCE_NUMBER + 1,
        connection->peer_sequence + 1,
        0x10,
        0,
        0,
        dhcp_frame
    );
    if (!send_ethernet_frame(net, dhcp_frame, length)) {
        return 0;
    }
    net->tcp_ack_sent = 1;
    net->http_response_received = 0;
    tcp_connection_set_output(connection, 0, 0);
    length = build_tcp_segment(
        net,
        net->dns_resolved_ip,
        net->gateway_mac,
        connection->source_port,
        TCP_HTTP_DESTINATION_PORT,
        TCP_SEQUENCE_NUMBER + 1,
        connection->peer_sequence + 1,
        0x18,
        http_get,
        sizeof(http_get),
        dhcp_frame
    );
    if (!send_ethernet_frame(net, dhcp_frame, length)) {
        return 0;
    }
    net->http_get_sent = 1;
    for (timeout = 0; timeout < 100000000U && !connection->response_received; timeout++) {
        (void)poll_receive(net);
    }
    if (!connection->response_received) {
        logger_write("ERROR", "HTTP response was not received");
        tcp_connection_release(connection);
        return 0;
    }
    tcp_connection_release(connection);
    return 1;
}

int virtio_net_self_test(ASAS_VIRTIO_NET *net)
{
    UINT8 frame[VIRTIO_NET_FRAME_SIZE];
    UINT8 gateway_ip[4] = {
        ASAS_GATEWAY_IP0,
        ASAS_GATEWAY_IP1,
        ASAS_GATEWAY_IP2,
        ASAS_GATEWAY_IP3
    };
    UINT32 length;

    if (!net->initialized || !net->rx_ready || !net->tx_ready) {
        return 0;
    }
    length = build_test_ethernet_frame(net, frame);
    if (!send_ethernet_frame(active_net, frame, length)) {
        return 0;
    }
    if (!virtio_net_dhcp_self_test(net)) {
        return 0;
    }
    if (!virtio_net_ping_ipv4(gateway_ip)) {
        return 0;
    }
    if (!virtio_net_dns_self_test(net)) {
        return 0;
    }
    if (net->dns_a_record_received) {
        if (virtio_net_http_get_resolved_ipv4(
            TCP_HTTP_DESTINATION_PORT,
            "example.com",
            cached_http_response,
            sizeof(cached_http_response),
            &cached_http_response_size
        )) {
            logger_write("INFO", "HTTP cached response received");
        } else {
            logger_write("ERROR", "HTTP cached response was not received");
        }
    }
    net->arp_tx_verified = 1;
    net->icmp_tx_verified = 1;
    return 1;
}

int virtio_net_ping_ipv4(const UINT8 target_ip[4])
{
    UINT8 frame[VIRTIO_NET_FRAME_SIZE];
    UINT32 length;
    UINT32 timeout;

    if (active_net == 0 || !active_net->initialized || !active_net->rx_ready || !active_net->tx_ready) {
        return 0;
    }
    copy_bytes(pending_ipv4_target, target_ip, sizeof(pending_ipv4_target));
    clear_bytes(pending_target_mac, sizeof(pending_target_mac));
    pending_arp_reply_received = 0;
    pending_icmp_reply_received = 0;

    length = build_arp_request(active_net, target_ip, frame);
    if (!send_ethernet_frame(active_net, frame, length)) {
        return 0;
    }
    active_net->arp_tx_verified = 1;
    for (timeout = 0; timeout < 100000000U && !pending_arp_reply_received; timeout++) {
        (void)poll_receive(active_net);
    }
    if (!pending_arp_reply_received) {
        logger_write("ERROR", "ARP reply was not received");
        return 0;
    }
    length = build_icmp_echo_request(active_net, target_ip, pending_target_mac, frame);
    if (!send_ethernet_frame(active_net, frame, length)) {
        return 0;
    }
    active_net->icmp_tx_verified = 1;
    for (timeout = 0; timeout < 100000000U && !pending_icmp_reply_received; timeout++) {
        (void)poll_receive(active_net);
    }
    if (!pending_icmp_reply_received) {
        logger_write("ERROR", "ICMP echo reply was not received");
        return 0;
    }
    return 1;
}

int virtio_net_ping_gateway(void)
{
    UINT8 gateway_ip[4] = {
        ASAS_GATEWAY_IP0,
        ASAS_GATEWAY_IP1,
        ASAS_GATEWAY_IP2,
        ASAS_GATEWAY_IP3
    };

    return virtio_net_ping_ipv4(gateway_ip);
}

int virtio_net_http_ready(void)
{
    return active_net != 0 && active_net->http_response_received;
}

static UINT32 append_text(UINT8 *buffer, UINT32 offset, const char *text)
{
    while (*text != '\0') {
        buffer[offset++] = (UINT8)*text;
        text++;
    }
    return offset;
}

int virtio_net_http_get_ipv4(
    const UINT8 target_ip[4],
    UINT16 port,
    const char *host,
    UINT8 *output,
    UINT32 output_capacity,
    UINT32 *output_size
)
{
    UINT8 target_mac[6];
    UINT32 request_length = 0;
    UINT32 length;
    UINT32 timeout;
    int target_is_local;
    ASAS_TCP_CONNECTION *connection;

    if (
        active_net == 0 ||
        !active_net->initialized ||
        output == 0 ||
        output_capacity == 0 ||
        output_size == 0
    ) {
        return 0;
    }
    *output_size = 0;
    target_is_local = (
        target_ip[0] == active_net->ip[0] &&
        target_ip[1] == active_net->ip[1] &&
        target_ip[2] == active_net->ip[2]
    );
    if (target_is_local) {
        if (!resolve_mac_ipv4(active_net, target_ip, target_mac)) {
            logger_write("ERROR", "HTTP GET ARP resolution failed");
            return 0;
        }
    } else if (active_net->arp_reply_received) {
        copy_bytes(target_mac, active_net->gateway_mac, sizeof(target_mac));
    } else {
        logger_write("ERROR", "HTTP GET gateway MAC is not available");
        return 0;
    }
    connection = tcp_connection_allocate(port);
    if (connection == 0) {
        logger_write("ERROR", "HTTP GET TCP connection table is full");
        return 0;
    }
    active_net->tcp_syn_ack_received = 0;
    active_net->http_response_received = 0;
    tcp_connection_set_output(connection, output, output_capacity);

    length = build_tcp_segment(
        active_net,
        target_ip,
        target_mac,
        connection->source_port,
        port,
        TCP_SEQUENCE_NUMBER,
        0,
        0x02,
        0,
        0,
        dhcp_frame
    );
    if (!send_ethernet_frame(active_net, dhcp_frame, length)) {
        logger_write("ERROR", "HTTP GET TCP SYN transmit failed");
        tcp_connection_release(connection);
        return 0;
    }
    active_net->tcp_syn_sent = 1;
    for (timeout = 0; timeout < 100000000U && !connection->syn_ack_received; timeout++) {
        (void)poll_receive(active_net);
    }
    if (!connection->syn_ack_received) {
        logger_write("ERROR", "HTTP GET TCP SYN ACK was not received");
        tcp_connection_release(connection);
        return 0;
    }
    length = build_tcp_segment(
        active_net,
        target_ip,
        target_mac,
        connection->source_port,
        port,
        TCP_SEQUENCE_NUMBER + 1,
        connection->peer_sequence + 1,
        0x10,
        0,
        0,
        dhcp_frame
    );
    if (!send_ethernet_frame(active_net, dhcp_frame, length)) {
        logger_write("ERROR", "HTTP GET TCP ACK transmit failed");
        tcp_connection_release(connection);
        return 0;
    }
    active_net->tcp_ack_sent = 1;

    request_length = append_text(dhcp_frame, request_length, "GET / HTTP/1.0\r\nHost: ");
    request_length = append_text(dhcp_frame, request_length, host);
    request_length = append_text(dhcp_frame, request_length, "\r\n\r\n");
    length = build_tcp_segment(
        active_net,
        target_ip,
        target_mac,
        connection->source_port,
        port,
        TCP_SEQUENCE_NUMBER + 1,
        connection->peer_sequence + 1,
        0x18,
        dhcp_frame,
        request_length,
        http_frame
    );
    if (!send_ethernet_frame(active_net, http_frame, length)) {
        logger_write("ERROR", "HTTP GET request transmit failed");
        tcp_connection_release(connection);
        return 0;
    }
    active_net->http_get_sent = 1;
    for (timeout = 0; timeout < 100000000U && !connection->response_received; timeout++) {
        (void)poll_receive(active_net);
    }
    *output_size = connection->output_size;
    if (!connection->response_received) {
        logger_write("ERROR", "HTTP GET response was not received");
    }
    active_net->http_response_received = connection->response_received;
    tcp_connection_release(connection);
    return active_net->http_response_received && *output_size != 0;
}

int virtio_net_http_get_resolved_ipv4(
    UINT16 port,
    const char *host,
    UINT8 *output,
    UINT32 output_capacity,
    UINT32 *output_size
)
{
    if (active_net == 0 || !active_net->dns_a_record_received) {
        return 0;
    }
    return virtio_net_http_get_ipv4(
        active_net->dns_resolved_ip,
        port,
        host,
        output,
        output_capacity,
        output_size
    );
}

int virtio_net_http_copy_cached(
    UINT8 *output,
    UINT32 output_capacity,
    UINT32 *output_size
)
{
    UINT32 copy_length;

    if (
        output == 0 ||
        output_capacity == 0 ||
        output_size == 0 ||
        cached_http_response_size == 0
    ) {
        return 0;
    }
    copy_length = cached_http_response_size;
    if (copy_length > output_capacity) {
        copy_length = output_capacity;
    }
    copy_bytes(output, cached_http_response, copy_length);
    *output_size = copy_length;
    return copy_length != 0;
}

int virtio_net_http_server_once(void)
{
    UINT32 timeout;

    if (active_net == 0 || !active_net->initialized || !active_net->rx_ready || !active_net->tx_ready) {
        return 0;
    }
    http_server_active = 1;
    http_server_request_received = 0;
    http_server_response_sent = 0;
    logger_write("SHELL", "http-server listening on 0.0.0.0:80");
    for (timeout = 0; timeout < 300000000U && !http_server_response_sent; timeout++) {
        (void)poll_receive(active_net);
    }
    http_server_active = 0;
    if (!http_server_request_received || !http_server_response_sent) {
        logger_write("SHELL", "http-server failed");
        return 0;
    }
    logger_write("SHELL", "http-server served one request");
    return 1;
}
