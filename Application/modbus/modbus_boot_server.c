/**
 * @file  modbus_boot_server.c
 * @brief Bare-metal single-client Modbus TCP server.
 *
 * Uses LwIP raw TCP API (NO_SYS=1) and nanoMODBUS byte-at-a-time
 * transport callbacks backed by a ring buffer.
 */

#include "modbus_boot_server.h"
#include "fw_update_proto.h"
#include "nanomodbus.h"
#include "lwip/tcp.h"
#include <string.h>

/* ---- Ring buffer for incoming TCP data ---------------------------------- */
#define RX_BUF_SIZE 1024u

static uint8_t  s_rx_buf[RX_BUF_SIZE];
static uint16_t s_rx_head;
static uint16_t s_rx_tail;
static uint16_t s_rx_count;

static void rx_put(uint8_t b)
{
    if (s_rx_count < RX_BUF_SIZE) {
        s_rx_buf[s_rx_head] = b;
        s_rx_head = (s_rx_head + 1u) % RX_BUF_SIZE;
        s_rx_count++;
    }
}

static int rx_get(uint8_t *b)
{
    if (s_rx_count == 0u) return 0;
    *b = s_rx_buf[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1u) % RX_BUF_SIZE;
    s_rx_count--;
    return 1;
}

static void rx_flush(void)
{
    s_rx_head  = 0;
    s_rx_tail  = 0;
    s_rx_count = 0;
}

/* Peek the byte at logical offset `idx` from the tail without consuming it. */
static int rx_peek(uint16_t idx, uint8_t *b)
{
    if (idx >= s_rx_count) return 0;
    *b = s_rx_buf[(s_rx_tail + idx) % RX_BUF_SIZE];
    return 1;
}

/* Return true when a complete Modbus TCP (MBAP) frame is buffered.
 *
 * The MBAP header length field (bytes 4-5, big-endian) counts the unit id
 * plus the PDU, so the full frame is 6 + length bytes. We only run the
 * nanoMODBUS poll once the whole frame is present; this avoids feeding a
 * partially-received (TCP-fragmented) frame into the byte-at-a-time parser,
 * whose read callback returns immediately and cannot honour a real timeout.
 *
 * If the length field is impossible for our buffer (desync or garbage), the
 * ring is flushed to force resynchronisation on the next frame. */
static bool rx_has_full_frame(void)
{
    if (s_rx_count < 7u) {
        return false;   /* need at least the 7-byte MBAP header */
    }

    uint8_t len_hi = 0, len_lo = 0;
    rx_peek(4u, &len_hi);
    rx_peek(5u, &len_lo);
    uint32_t len   = ((uint32_t)len_hi << 8) | (uint32_t)len_lo;
    uint32_t total = 6u + len;

    if (len < 2u || total > RX_BUF_SIZE) {
        /* Length cannot be valid: drop the buffered bytes to resync. */
        rx_flush();
        return false;
    }

    return s_rx_count >= (uint16_t)total;
}

/* ---- TCP transmit buffer ------------------------------------------------ */
#define TX_BUF_SIZE 512u

static uint8_t  s_tx_buf[TX_BUF_SIZE];
static uint16_t s_tx_len;

/* ---- State -------------------------------------------------------------- */
static struct tcp_pcb *s_listener;
static struct tcp_pcb *s_client;
static nmbs_t          s_nmbs;
static bool            s_nmbs_ready;
static metadata_t     *s_meta;

/* ---- nanoMODBUS transport callbacks ------------------------------------- */
static int nmbs_read_cb(uint8_t *b, int32_t timeout_ms, void *arg)
{
    (void)arg;
    (void)timeout_ms;
    return rx_get(b);
}

static int nmbs_write_cb(uint8_t b, int32_t timeout_ms, void *arg)
{
    (void)arg;
    (void)timeout_ms;
    if (s_tx_len < TX_BUF_SIZE) {
        s_tx_buf[s_tx_len++] = b;
        return 1;
    }
    return -1;
}

static void nmbs_sleep_cb(uint32_t ms, void *arg)
{
    (void)ms;
    (void)arg;
}

/* ---- nanoMODBUS Modbus callbacks ---------------------------------------- */
static nmbs_error cb_read_input_regs(uint16_t addr, uint16_t qty,
                                     uint16_t *out)
{
    return fw_proto_read_input_regs(addr, qty, out);
}

static nmbs_error cb_read_holding_regs(uint16_t addr, uint16_t qty,
                                       uint16_t *out)
{
    return fw_proto_read_holding_regs(addr, qty, out);
}

static nmbs_error cb_write_multiple_regs(uint16_t addr, uint16_t qty,
                                         const uint16_t *in)
{
    return fw_proto_write_holding_regs(addr, qty, in);
}

static nmbs_error cb_write_single_reg(uint16_t addr, uint16_t val)
{
    return fw_proto_write_holding_regs(addr, 1u, &val);
}

/* ---- LwIP raw TCP callbacks --------------------------------------------- */
static void close_client(void)
{
    if (s_client != NULL) {
        tcp_arg(s_client, NULL);
        tcp_recv(s_client, NULL);
        tcp_err(s_client, NULL);
        tcp_close(s_client);
        s_client = NULL;
    }
    s_rx_head  = 0;
    s_rx_tail  = 0;
    s_rx_count = 0;
    s_tx_len   = 0;
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p,
                     err_t err)
{
    (void)arg;

    if (p == NULL || err != ERR_OK) {
        close_client();
        return ERR_OK;
    }

    /* Copy incoming bytes into the ring buffer. */
    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        const uint8_t *d = (const uint8_t *)q->payload;
        for (uint16_t i = 0; i < q->len; i++) {
            rx_put(d[i]);
        }
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    s_client = NULL;
    s_rx_head  = 0;
    s_rx_tail  = 0;
    s_rx_count = 0;
    s_tx_len   = 0;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;

    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    /* Single client only. */
    if (s_client != NULL) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    s_client = newpcb;
    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, on_recv);
    tcp_err(newpcb, on_err);

    s_rx_head  = 0;
    s_rx_tail  = 0;
    s_rx_count = 0;
    s_tx_len   = 0;

    return ERR_OK;
}

/* ---- Public API --------------------------------------------------------- */
void modbus_boot_server_init(metadata_t *meta, uint16_t port)
{
    s_meta     = meta;
    s_client   = NULL;
    s_nmbs_ready = false;

    fw_proto_init(meta);

    /* Create nanoMODBUS server instance. */
    nmbs_platform_conf platform = {
        .transport  = NMBS_TRANSPORT_TCP,
        .read_byte  = nmbs_read_cb,
        .write_byte = nmbs_write_cb,
        .sleep      = nmbs_sleep_cb,
        .arg        = NULL,
    };

    nmbs_callbacks cbs = {0};
    cbs.read_input_registers        = cb_read_input_regs;
    cbs.read_holding_registers      = cb_read_holding_regs;
    cbs.write_multiple_registers    = cb_write_multiple_regs;
    cbs.write_single_register       = cb_write_single_reg;

    if (nmbs_server_create(&s_nmbs, 1, &platform, &cbs) == NMBS_ERROR_NONE) {
        nmbs_set_read_timeout(&s_nmbs, 100);
        nmbs_set_byte_timeout(&s_nmbs, 50);
        s_nmbs_ready = true;
    }

    /* Start listening. */
    s_listener = tcp_new();
    if (s_listener != NULL) {
        tcp_bind(s_listener, IP_ADDR_ANY, port);
        s_listener = tcp_listen(s_listener);
        tcp_accept(s_listener, on_accept);
    }
}

void modbus_boot_server_poll(void)
{
    if (!s_nmbs_ready || s_client == NULL) {
        return;
    }

    /* Only feed the parser once a whole MBAP frame is buffered, so a
     * TCP-fragmented request is never partially consumed (see
     * rx_has_full_frame()). */
    if (!rx_has_full_frame()) {
        return;
    }

    s_tx_len = 0;

    nmbs_error e = nmbs_server_poll(&s_nmbs);

    /* Flush any response accumulated in s_tx_buf. */
    if (s_tx_len > 0u && s_client != NULL) {
        err_t werr = tcp_write(s_client, s_tx_buf, s_tx_len, TCP_WRITE_FLAG_COPY);
        if (werr == ERR_OK) {
            tcp_output(s_client);
        }
        s_tx_len = 0;
    }

    /* Transport-level errors (nmbs_error < 0) mean the byte stream is out of
     * sync; Modbus exceptions (> 0) are valid answered responses. On a
     * transport error, discard buffered bytes to resynchronise on the next
     * frame instead of staying stuck. */
    if (e < NMBS_ERROR_NONE) {
        rx_flush();
    }
}
