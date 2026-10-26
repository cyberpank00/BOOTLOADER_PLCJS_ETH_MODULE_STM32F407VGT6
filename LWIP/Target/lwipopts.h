/**
 * @file  lwipopts.h
 * @brief LwIP configuration for bootloader — bare-metal (NO_SYS = 1).
 */
#ifndef __LWIPOPTS__H__
#define __LWIPOPTS__H__

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Bare-metal (no OS) ------------------------------------------------- */
#define NO_SYS                  1
#define NO_SYS_NO_TIMERS        0
#define WITH_RTOS               0
#define SYS_LIGHTWEIGHT_PROT    0

/* ---- Memory settings ---------------------------------------------------- */
#define MEM_ALIGNMENT           4
#define MEM_SIZE                (8 * 1024)
#define MEMP_NUM_PBUF           16
#define MEMP_NUM_TCP_PCB        4
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG        16
#define MEMP_NUM_SYS_TIMEOUT    8
#define PBUF_POOL_SIZE          16
/* Must be >= ETH_RX_BUF_SIZE (1536) so each pool entry can hold one full
   DMA buffer without overflow. Previously was 1524, causing a 12-byte
   buffer overrun on every received frame.                               */
#define PBUF_POOL_BUFSIZE       1536

/* ---- TCP settings ------------------------------------------------------- */
#define LWIP_TCP                1
#define LWIP_TCP_KEEPALIVE      1
#define TCP_MSS                 1460
#define TCP_SND_BUF             (4 * TCP_MSS)
#define TCP_WND                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN        ((4 * TCP_SND_BUF + (TCP_MSS - 1)) / TCP_MSS)
#define TCP_SNDLOWAT            ((TCP_SND_BUF) / 2)
#define TCP_SNDQUEUELOWAT       5
#define TCP_WND_UPDATE_THRESHOLD 536

/* ---- Protocols ---------------------------------------------------------- */
#define LWIP_UDP                1
#define LWIP_DHCP               0    /* bootloader uses static IP */
#define LWIP_AUTOIP             0
#define LWIP_DNS                0
#define LWIP_IGMP               0
#define LWIP_ARP                1
#define LWIP_ETHERNET           1
#define LWIP_ICMP               1
#define LWIP_RAW                0
#define LWIP_NETCONN            0    /* NO_SYS — no sequential API */
#define LWIP_SOCKET             0

/* ---- Checksum offload (STM32 ETH DMA) ---------------------------------- */
#define CHECKSUM_BY_HARDWARE    1
#define CHECKSUM_GEN_IP         0
#define CHECKSUM_GEN_UDP        0
#define CHECKSUM_GEN_TCP        0
#define CHECKSUM_GEN_ICMP       0
#define CHECKSUM_GEN_ICMP6      0
#define CHECKSUM_CHECK_IP       0
#define CHECKSUM_CHECK_UDP      0
#define CHECKSUM_CHECK_TCP      0
#define CHECKSUM_CHECK_ICMP     0
#define CHECKSUM_CHECK_ICMP6    0

/* ---- Link callback ------------------------------------------------------ */
#define LWIP_NETIF_LINK_CALLBACK 1

/* ---- Stats / debug ------------------------------------------------------ */
#define LWIP_STATS              0
#define LWIP_DEBUG              0

#ifdef __cplusplus
}
#endif

#endif /* __LWIPOPTS__H__ */
