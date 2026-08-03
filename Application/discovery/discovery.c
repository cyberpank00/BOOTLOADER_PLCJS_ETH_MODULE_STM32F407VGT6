/**
  ******************************************************************************
  * @file    discovery.c
  * @brief   PLCJS Discovery Protocol (PDP) responder — bootloader, UDP/20556.
  *
  * Frame (16-byte header, little payload):
  *   magic[4]="PLCD", version=1, opcode, txid(BE), target_mac[6], len(BE), payload
  * Device replies via UDP broadcast, echoing txid, with its own MAC and the
  * response opcode (request | 0x80).
  *
  * Opcodes (request / response):
  *   0x01/0x81 IDENTIFY  -> {product_id(BE32), hw(BE16), fw(BE16), net_mode,
  *                           in_bootloader=1, ip[4], mask[4], gw[4], name[16]}
  *   0x02/0x82 SET_NET   -> req {mode, ip[4], mask[4], gw[4]} ; resp {status}
  *                          applied live (NO_SYS), not persisted.
  *   0x04/0x84 FLASH_LED -> req {seconds}                     ; resp {status}
  *   0x05/0x85 REBOOT    -> resp {status} then reset (deferred to super-loop)
  *   0x03 SET_NAME / 0x06 FACTORY -> resp {status=err} (no settings store)
  ******************************************************************************
  */
#include "discovery.h"

#include "lwip/opt.h"
#include "lwip/udp.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/def.h"
#include "lwip/pbuf.h"

#include <string.h>

#include "net_id.h"
#include "led_indication.h"
#include "flash_map.h"   /* PRODUCT_ID_DEFAULT, HW_REVISION_DEFAULT, BOOTLOADER_VERSION */

extern struct netif gnetif;

#define PDP_PORT            20556u
#define PDP_VERSION         1u
#define PDP_RESP_FLAG       0x80u

#define PDP_OP_IDENTIFY     0x01u
#define PDP_OP_SET_NET      0x02u
#define PDP_OP_SET_NAME     0x03u
#define PDP_OP_FLASH_LED    0x04u
#define PDP_OP_REBOOT       0x05u
#define PDP_OP_FACTORY      0x06u

#define PDP_HDR_LEN         16u
#define PDP_NAME_LEN        16u
#define PDP_STATUS_OK       0u
#define PDP_STATUS_ERR      1u

/* Network modes (mirror NET_MODE_* in the applications' settings.h). */
#define PDP_NET_STATIC      0u
#define PDP_NET_DHCP        1u
#define PDP_NET_LINKLOCAL   2u

static struct udp_pcb*  s_pcb;
static volatile uint8_t s_pending_reboot;
static uint8_t          s_net_mode = PDP_NET_LINKLOCAL; /* reported by IDENTIFY */

bool discovery_take_pending_reboot(void)
{
    uint8_t v = s_pending_reboot;
    s_pending_reboot = 0u;
    return v != 0u;
}

static void put16(uint8_t* p, uint16_t v) { uint16_t n = lwip_htons(v); memcpy(p, &n, 2); }
static void put32(uint8_t* p, uint32_t v) { uint32_t n = lwip_htonl(v); memcpy(p, &n, 4); }

static void pdp_send(uint8_t opcode, const uint8_t txid[2],
                     const uint8_t* payload, uint16_t plen)
{
    const uint16_t total = (uint16_t)(PDP_HDR_LEN + plen);
    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, total, PBUF_RAM);
    if (p == NULL) { return; }

    uint8_t* b = (uint8_t*)p->payload;
    memset(b, 0, total);
    b[0] = 'P'; b[1] = 'L'; b[2] = 'C'; b[3] = 'D';
    b[4] = PDP_VERSION;
    b[5] = opcode;
    b[6] = txid[0]; b[7] = txid[1];
    net_id_get_mac(&b[8]);
    put16(&b[14], plen);
    if (plen != 0u && payload != NULL) { memcpy(&b[16], payload, plen); }

    udp_sendto(s_pcb, p, IP_ADDR_BROADCAST, PDP_PORT);
    pbuf_free(p);
}

static void pdp_reply_status(uint8_t req_op, const uint8_t txid[2], uint8_t status)
{
    pdp_send((uint8_t)(req_op | PDP_RESP_FLAG), txid, &status, 1u);
}

static void pdp_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p,
                     const ip_addr_t* addr, u16_t port)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(pcb);
    LWIP_UNUSED_ARG(addr);
    LWIP_UNUSED_ARG(port);

    uint8_t buf[128];
    const u16_t len = pbuf_copy_partial(p, buf, sizeof(buf), 0);
    pbuf_free(p);

    if (len < PDP_HDR_LEN) { return; }
    if (!(buf[0] == 'P' && buf[1] == 'L' && buf[2] == 'C' && buf[3] == 'D')) { return; }
    if (buf[4] != PDP_VERSION) { return; }

    const uint8_t opcode = buf[5];
    if ((opcode & PDP_RESP_FLAG) != 0u) { return; }  /* ignore responses */

    const uint8_t* txid = &buf[6];

    /* Target-MAC filter: act only if broadcast (all-zero) or our MAC. */
    uint8_t mymac[6];
    net_id_get_mac(mymac);
    const uint8_t* tgt = &buf[8];
    const bool bcast = ((tgt[0] | tgt[1] | tgt[2] | tgt[3] | tgt[4] | tgt[5]) == 0u);
    if (!bcast && memcmp(tgt, mymac, 6) != 0) { return; }

    uint16_t plen;
    { uint16_t n; memcpy(&n, &buf[14], 2); plen = lwip_ntohs(n); }
    if ((uint32_t)PDP_HDR_LEN + plen > len) {
        plen = (len > PDP_HDR_LEN) ? (uint16_t)(len - PDP_HDR_LEN) : 0u;
    }
    const uint8_t* pl = &buf[PDP_HDR_LEN];

    switch (opcode) {
    case PDP_OP_IDENTIFY: {
        uint8_t r[64];
        memset(r, 0, sizeof(r));
        put32(&r[0], (uint32_t)PRODUCT_ID_DEFAULT);
        put16(&r[4], (uint16_t)(HW_REVISION_DEFAULT >> 8)); /* major.minor */
        put16(&r[6], (uint16_t)BOOTLOADER_VERSION);          /* bootloader ver as "fw" */
        r[8] = s_net_mode;
        r[9] = 1u;                                            /* in bootloader */
        const u32_t ip = ip4_addr_get_u32(netif_ip4_addr(&gnetif));
        const u32_t mk = ip4_addr_get_u32(netif_ip4_netmask(&gnetif));
        const u32_t gw = ip4_addr_get_u32(netif_ip4_gw(&gnetif));
        memcpy(&r[10], &ip, 4);
        memcpy(&r[14], &mk, 4);
        memcpy(&r[18], &gw, 4);
        /* r[22..37] name — bootloader has none, left as zeros. */
        pdp_send((uint8_t)(PDP_OP_IDENTIFY | PDP_RESP_FLAG), txid,
                 r, (uint16_t)(22u + PDP_NAME_LEN));
        break;
    }
    case PDP_OP_SET_NET: {
        if (plen < 1u) { pdp_reply_status(opcode, txid, PDP_STATUS_ERR); break; }
        const uint8_t mode = pl[0];
        ip4_addr_t ip, mask, gw;
        if (mode == PDP_NET_LINKLOCAL) {
            uint8_t ll[4];
            net_id_get_linklocal(ll);
            IP4_ADDR(&ip, ll[0], ll[1], ll[2], ll[3]);
            IP4_ADDR(&mask, 255u, 255u, 0u, 0u);
            ip4_addr_set_zero(&gw);
        } else if (mode == PDP_NET_STATIC) {
            if (plen < 13u) { pdp_reply_status(opcode, txid, PDP_STATUS_ERR); break; }
            IP4_ADDR(&ip,   pl[1], pl[2],  pl[3],  pl[4]);
            IP4_ADDR(&mask, pl[5], pl[6],  pl[7],  pl[8]);
            IP4_ADDR(&gw,   pl[9], pl[10], pl[11], pl[12]);
        } else {
            /* DHCP not supported in the bootloader. */
            pdp_reply_status(opcode, txid, PDP_STATUS_ERR);
            break;
        }
        /* NO_SYS: safe to reconfigure the netif directly from this callback. */
        netif_set_addr(&gnetif, &ip, &mask, &gw);
        s_net_mode = mode;
        pdp_reply_status(opcode, txid, PDP_STATUS_OK);
        break;
    }
    case PDP_OP_FLASH_LED: {
        const uint8_t secs = (plen >= 1u) ? pl[0] : 10u;
        led_indication_signal_identify((uint32_t)secs * 1000u);
        pdp_reply_status(opcode, txid, PDP_STATUS_OK);
        break;
    }
    case PDP_OP_REBOOT: {
        s_pending_reboot = 1u;
        pdp_reply_status(opcode, txid, PDP_STATUS_OK);
        break;
    }
    case PDP_OP_SET_NAME:
    case PDP_OP_FACTORY:
        /* Not applicable in the bootloader (no persistent settings store). */
        pdp_reply_status(opcode, txid, PDP_STATUS_ERR);
        break;
    default:
        break;
    }
}

void discovery_init(void)
{
    if (s_pcb != NULL) { return; }
    s_pcb = udp_new();
    if (s_pcb == NULL) { return; }
    ip_set_option(s_pcb, SOF_BROADCAST);   /* allow send/recv of broadcast */
    if (udp_bind(s_pcb, IP_ANY_TYPE, PDP_PORT) != ERR_OK) {
        udp_remove(s_pcb);
        s_pcb = NULL;
        return;
    }
    udp_recv(s_pcb, pdp_recv, NULL);
}
