/**
  ******************************************************************************
  * @file    netbiosns.c
  * @brief   Minimal NetBIOS name-service responder (UDP/137).
  *
  * Answers NetBIOS NAME QUERY requests for our own name with the netif IPv4
  * address, so the (static-IP) bootloader is identifiable by name on the LAN.
  * Self-contained: raw UDP API, NO_SYS compatible. No dependency on the
  * (absent) LwIP netbiosns app module.
  ******************************************************************************
  */
#include "netbiosns.h"

#include "lwip/opt.h"
#include "lwip/udp.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/def.h"
#include "lwip/pbuf.h"

#include "net_id.h"

#include <string.h>

#define NETBIOS_PORT        137u
#define NETBIOS_NAME_LEN    16u   /* 15 chars + 1 suffix byte */

/* Header flag bits */
#define NB_FLAG_RESPONSE    0x8000u
#define NB_FLAG_OPCODE_MASK 0x7800u
#define NB_FLAG_OPCODE_QUERY 0x0000u
#define NB_FLAG_AUTHORATIVE 0x0400u
/* Name flags (answer) */
#define NB_NFLAG_UNIQUE     0x8000u

/* Question / RR types */
#define NB_TYPE_NB          0x0020u   /* name query (name -> IP)     */
#define NB_TYPE_NBSTAT      0x0021u   /* node status (report names)  */
#define NB_CLASS_IN         0x0001u

/* Node-status NAME_FLAGS: unique, B-node, active. */
#define NB_STAT_NAME_FLAGS  0x0400u

typedef struct __attribute__((packed)) {
    uint16_t trans_id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answerRRs;
    uint16_t authorityRRs;
    uint16_t additionalRRs;
} nb_hdr_t;

/* Question / RR name field: 1 length byte (0x20) + 32 encoded chars + null. */
typedef struct __attribute__((packed)) {
    uint8_t  name_size;                       /* 0x20 */
    uint8_t  name[(NETBIOS_NAME_LEN * 2) + 1];
    uint16_t type;
    uint16_t cls;
} nb_question_t;

typedef struct __attribute__((packed)) {
    nb_hdr_t hdr;
    uint8_t  name_size;
    uint8_t  name[(NETBIOS_NAME_LEN * 2) + 1];
    uint16_t type;
    uint16_t cls;
    uint32_t ttl;
    uint16_t data_len;
    uint16_t flags;
    uint8_t  addr[4];
} nb_answer_t;

static struct udp_pcb* s_pcb;
static char            s_name[NETBIOS_NAME_LEN];  /* upper-cased, space/nul padded not required */

static char to_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

void netbiosns_set_name(const char* name)
{
    memset(s_name, 0, sizeof(s_name));
    for (unsigned i = 0; i < (NETBIOS_NAME_LEN - 1) && name[i] != '\0'; i++) {
        s_name[i] = to_upper(name[i]);
    }
}

/* Decode a 32-char first-level-encoded NetBIOS name into up to 15 trimmed,
 * upper-cased chars (ignoring the 16th suffix byte). */
static void nb_decode(const uint8_t* enc, char* out /* [NETBIOS_NAME_LEN] */)
{
    char tmp[NETBIOS_NAME_LEN];
    for (unsigned i = 0; i < NETBIOS_NAME_LEN; i++) {
        tmp[i] = (char)((((enc[2u * i]     - 'A') & 0x0F) << 4) |
                        (((enc[2u * i + 1] - 'A') & 0x0F)));
    }
    unsigned n = 0;
    for (unsigned i = 0; i < (NETBIOS_NAME_LEN - 1); i++) {
        if (tmp[i] == ' ' || tmp[i] == '\0') { break; }
        out[n++] = to_upper(tmp[i]);
    }
    out[n] = '\0';
}

static void nb_send_response(struct udp_pcb* pcb, const ip_addr_t* addr, u16_t port,
                             const nb_hdr_t* qhdr, const nb_question_t* q)
{
    struct netif* nif = netif_default;
    if (nif == NULL) { return; }

    struct pbuf* r = pbuf_alloc(PBUF_TRANSPORT, sizeof(nb_answer_t), PBUF_RAM);
    if (r == NULL) { return; }

    nb_answer_t* a = (nb_answer_t*)r->payload;
    memset(a, 0, sizeof(*a));
    a->hdr.trans_id = qhdr->trans_id;  /* echo (already network order) */
    a->hdr.flags    = lwip_htons(NB_FLAG_RESPONSE | NB_FLAG_OPCODE_QUERY | NB_FLAG_AUTHORATIVE);
    a->hdr.answerRRs = lwip_htons(1);
    a->name_size    = 0x20u;
    memcpy(a->name, q->name, sizeof(a->name));  /* echo the queried name */
    a->type     = lwip_htons(0x0020u);          /* NB */
    a->cls      = lwip_htons(0x0001u);           /* IN */
    a->ttl      = lwip_htonl(300000u);
    a->data_len = lwip_htons(6u);
    a->flags    = lwip_htons(NB_NFLAG_UNIQUE);   /* unique, B-node */

    const u32_t ip = ip4_addr_get_u32(netif_ip4_addr(nif)); /* network order */
    memcpy(a->addr, &ip, 4);

    udp_sendto(pcb, r, addr, port);
    pbuf_free(r);
}

/* Write a 16-byte NetBIOS name (15 chars space-padded + suffix) at dst. */
static void nb_write_nbname(uint8_t* dst, const char* name, uint8_t suffix)
{
    memset(dst, ' ', 15);
    for (unsigned i = 0; i < 15u && name[i] != '\0'; i++) { dst[i] = (uint8_t)name[i]; }
    dst[15] = suffix;
}

static void put16(uint8_t* p, uint16_t v) { const uint16_t n = lwip_htons(v); memcpy(p, &n, 2); }

/* Respond to a NODE STATUS (NBSTAT) request with our name(s) so passive network
 * scanners / managed switches display the device name. */
static void nb_send_nbstat(struct udp_pcb* pcb, const ip_addr_t* addr, u16_t port,
                           const uint8_t* buf /* received packet */)
{
    if (s_name[0] == '\0') { return; }

    #define NB_STAT_NUM_NAMES 2u
    const uint16_t rdlen = (uint16_t)(1u + (NB_STAT_NUM_NAMES * 18u) + 46u);
    const uint16_t total = (uint16_t)(12u + 34u + 2u + 2u + 4u + 2u + rdlen);

    struct pbuf* r = pbuf_alloc(PBUF_TRANSPORT, total, PBUF_RAM);
    if (r == NULL) { return; }
    uint8_t* b = (uint8_t*)r->payload;
    memset(b, 0, total);

    memcpy(b + 0, buf + 0, 2);                       /* echo trans_id      */
    put16(b + 2, NB_FLAG_RESPONSE | NB_FLAG_OPCODE_QUERY | NB_FLAG_AUTHORATIVE);
    put16(b + 6, 1);                                 /* answerRRs = 1      */
    b[12] = 0x20;
    memcpy(b + 13, buf + 13, 32);                    /* echo queried name  */
    b[45] = 0x00;
    put16(b + 46, NB_TYPE_NBSTAT);
    put16(b + 48, NB_CLASS_IN);
    /* ttl (b+50..53) = 0 */
    put16(b + 54, rdlen);

    uint8_t* rd = b + 56;
    rd[0] = (uint8_t)NB_STAT_NUM_NAMES;
    uint8_t* np = rd + 1;
    nb_write_nbname(np, s_name, 0x00); put16(np + 16, NB_STAT_NAME_FLAGS); np += 18; /* <00> workstation */
    nb_write_nbname(np, s_name, 0x20); put16(np + 16, NB_STAT_NAME_FLAGS); np += 18; /* <20> server      */

    /* Statistics block (46 bytes): first 6 = MAC (unit ID), rest zero. */
    uint8_t mac[6];
    net_id_get_mac(mac);
    memcpy(np, mac, 6);

    udp_sendto(pcb, r, addr, port);
    pbuf_free(r);
}

static void nb_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p,
                    const ip_addr_t* addr, u16_t port)
{
    LWIP_UNUSED_ARG(arg);

    uint8_t buf[128];
    const u16_t len = pbuf_copy_partial(p, buf, sizeof(buf), 0);
    pbuf_free(p);

    if (len < (u16_t)(sizeof(nb_hdr_t) + sizeof(nb_question_t))) { return; }

    const nb_hdr_t*      hdr = (const nb_hdr_t*)buf;
    const nb_question_t* q   = (const nb_question_t*)(buf + sizeof(nb_hdr_t));
    const u16_t flags = lwip_ntohs(hdr->flags);

    if ((flags & NB_FLAG_RESPONSE) != 0) { return; }                 /* not a query */
    if ((flags & NB_FLAG_OPCODE_MASK) != NB_FLAG_OPCODE_QUERY) { return; }
    if (lwip_ntohs(hdr->questions) < 1) { return; }
    if (q->name_size != 0x20u) { return; }

    const u16_t qtype = lwip_ntohs(q->type);
    if (qtype == NB_TYPE_NBSTAT) {
        /* Node status: report our name(s) regardless of the queried name
         * (scanners send the wildcard "*"). */
        nb_send_nbstat(pcb, addr, port, buf);
        return;
    }
    if (qtype != NB_TYPE_NB) { return; }

    char qname[NETBIOS_NAME_LEN];
    nb_decode(q->name, qname);

    if (s_name[0] != '\0' && strcmp(qname, s_name) == 0) {
        nb_send_response(pcb, addr, port, hdr, q);
    }
}

void netbiosns_init(void)
{
    if (s_pcb != NULL) { return; }
    s_pcb = udp_new();
    if (s_pcb == NULL) { return; }
    if (udp_bind(s_pcb, IP_ANY_TYPE, NETBIOS_PORT) != ERR_OK) {
        udp_remove(s_pcb);
        s_pcb = NULL;
        return;
    }
    udp_recv(s_pcb, nb_recv, NULL);
}
