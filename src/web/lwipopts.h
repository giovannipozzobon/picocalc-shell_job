/* lwipopts.h — lwIP for web.uf2 (design.md §3.9): no OS, polled
 * (pico_cyw43_arch_lwip_poll), IPv4 with DHCP, TCP, UDP for DHCP and
 * mDNS, and no DNS client. After PicoMultiLoader's, cut down. */
#ifndef WEB_LWIPOPTS_H
#define WEB_LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (14 * 1024)   /* queued output (tcp_write copies) */
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_ARP_QUEUE          10
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)
#define PBUF_POOL_SIZE              10            /* received frames */

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    0
#define LWIP_DHCP                   1
#define LWIP_IGMP                   1             /* for mDNS */
#define LWIP_AUTOIP                 0
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define TCP_LISTEN_BACKLOG          1
#define LWIP_TCP_KEEPALIVE          1

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1
#define LWIP_NUM_NETIF_CLIENT_DATA  1

/* mDNS: http://picocalc.local/ */
#define LWIP_MDNS_RESPONDER         1
#define MDNS_MAX_SERVICES           1

#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3

#endif /* WEB_LWIPOPTS_H */
