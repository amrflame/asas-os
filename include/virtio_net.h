#ifndef ASAS_VIRTIO_NET_H
#define ASAS_VIRTIO_NET_H

#include "pci.h"

typedef struct {
    UINT16 io_base;
    UINT16 rx_queue_size;
    UINT16 tx_queue_size;
    UINT8 mac[6];
    UINT8 has_mac;
    UINT8 initialized;
    UINT8 rx_ready;
    UINT8 tx_ready;
    UINT8 ethernet_tx_verified;
    UINT8 arp_tx_verified;
    UINT8 icmp_tx_verified;
    UINT8 rx_poll_verified;
    UINT8 arp_reply_received;
    UINT8 icmp_reply_received;
    UINT8 gateway_mac[6];
    UINT8 ip[4];
    UINT8 dhcp_server_ip[4];
    UINT8 dhcp_discover_sent;
    UINT8 dhcp_offer_received;
    UINT8 dhcp_request_sent;
    UINT8 dhcp_ack_received;
    UINT8 dns_server_mac[6];
    UINT8 dns_query_sent;
    UINT8 dns_response_received;
    UINT8 dns_a_record_received;
    UINT8 dns_resolved_ip[4];
    UINT8 tcp_syn_sent;
    UINT8 tcp_syn_ack_received;
    UINT8 tcp_ack_sent;
    UINT8 http_get_sent;
    UINT8 http_response_received;
    UINT32 tcp_peer_sequence;
} ASAS_VIRTIO_NET;

int virtio_net_initialize(const ASAS_PCI_DEVICE *device, ASAS_VIRTIO_NET *net);
int virtio_net_self_test(ASAS_VIRTIO_NET *net);
int virtio_net_ping_gateway(void);
int virtio_net_ping_ipv4(const UINT8 target_ip[4]);
int virtio_net_http_ready(void);
int virtio_net_http_get_ipv4(
    const UINT8 target_ip[4],
    UINT16 port,
    const char *host,
    UINT8 *output,
    UINT32 output_capacity,
    UINT32 *output_size
);
int virtio_net_http_get_resolved_ipv4(
    UINT16 port,
    const char *host,
    UINT8 *output,
    UINT32 output_capacity,
    UINT32 *output_size
);
int virtio_net_http_copy_cached(
    UINT8 *output,
    UINT32 output_capacity,
    UINT32 *output_size
);
int virtio_net_http_server_once(void);

#endif
