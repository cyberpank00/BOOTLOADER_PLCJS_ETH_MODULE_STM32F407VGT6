#include <assert.h>
#include <stdio.h>
#include "../Application/modbus/modbus_boot_server.c"

static uint32_t now;
static unsigned acknowledged, responses, aborted, freed;
static bool congested;
static struct tcp_pcb listener, client, other;

uint32_t HAL_GetTick(void) { return now; }
void tcp_arg(struct tcp_pcb *p, void *a) { (void)p; (void)a; }
void tcp_recv(struct tcp_pcb *p, err_t (*cb)(void *, struct tcp_pcb *, struct pbuf *, err_t))
{ (void)p; (void)cb; }
void tcp_err(struct tcp_pcb *p, void (*cb)(void *, err_t)) { (void)p; (void)cb; }
void tcp_accept(struct tcp_pcb *p, err_t (*cb)(void *, struct tcp_pcb *, err_t))
{ (void)p; (void)cb; }
void tcp_abort(struct tcp_pcb *p) { (void)p; aborted++; }
err_t tcp_close(struct tcp_pcb *p) { (void)p; return ERR_OK; }
struct tcp_pcb *tcp_new(void) { return &listener; }
err_t tcp_bind(struct tcp_pcb *p, const void *ip, uint16_t port)
{ (void)p; (void)ip; (void)port; return ERR_OK; }
struct tcp_pcb *tcp_listen(struct tcp_pcb *p) { return p; }
void tcp_recved(struct tcp_pcb *p, uint16_t size) { (void)p; acknowledged += size; }
err_t tcp_write(struct tcp_pcb *p, const void *data, uint16_t size, uint8_t flags)
{
    (void)p; (void)data; (void)flags;
    if (congested) return ERR_MEM;
    assert(size == 11);
    responses++;
    return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *p) { (void)p; return ERR_OK; }
uint8_t pbuf_free(struct pbuf *p) { (void)p; freed++; return 1; }
void fw_proto_init(metadata_t *meta) { (void)meta; }
nmbs_error fw_proto_read_input_regs(uint16_t addr, uint16_t qty, uint16_t *out)
{ (void)addr; for (uint16_t i = 0; i < qty; i++) out[i] = 0x1234; return NMBS_ERROR_NONE; }
nmbs_error fw_proto_read_holding_regs(uint16_t addr, uint16_t qty, uint16_t *out)
{ return fw_proto_read_input_regs(addr, qty, out); }
nmbs_error fw_proto_write_holding_regs(uint16_t addr, uint16_t qty, const uint16_t *in)
{ (void)addr; (void)qty; (void)in; return NMBS_ERROR_NONE; }

static uint8_t frame[] = {0, 1, 0, 0, 0, 6, 1, 4, 0, 0, 0, 1};
static void deliver(void *data, uint16_t size)
{
    struct pbuf p = {NULL, data, size, size};
    assert(on_recv(NULL, &client, &p, ERR_OK) == ERR_OK);
}
static void fixture(void)
{
    now = acknowledged = responses = aborted = freed = 0;
    congested = false;
    modbus_boot_server_init(NULL, 502);
    assert(on_accept(NULL, &client, ERR_OK) == ERR_OK);
}

int main(void)
{
    fixture();
    assert(on_accept(NULL, &other, ERR_OK) == ERR_ABRT && s_client == &client);
    deliver(frame, 5);
    modbus_boot_server_poll();
    assert(!acknowledged && !responses);
    deliver(frame + 5, sizeof(frame) - 5);
    deliver(frame, sizeof(frame));
    modbus_boot_server_poll();
    assert(acknowledged == sizeof(frame) && responses == 1 && s_rx_count == sizeof(frame));
    modbus_boot_server_poll();
    assert(acknowledged == 2 * sizeof(frame) && responses == 2 && s_rx_count == 0);

    fixture();
    congested = true;
    deliver(frame, sizeof(frame));
    deliver(frame, sizeof(frame));
    modbus_boot_server_poll();
    assert(s_tx_len == 11 && s_rx_count == sizeof(frame) && !responses);
    modbus_boot_server_poll();
    assert(s_tx_len == 11 && s_rx_count == sizeof(frame) && !responses);
    congested = false;
    modbus_boot_server_poll();
    assert(responses == 2 && !s_tx_len && !s_rx_count);

    fixture();
    deliver(frame, 1);
    modbus_boot_server_poll();
    now = FRAME_TIMEOUT_MS;
    modbus_boot_server_poll();
    assert(s_client == NULL && aborted == 1);
    assert(on_accept(NULL, &other, ERR_OK) == ERR_OK);
    now += CLIENT_IDLE_MS;
    modbus_boot_server_poll();
    assert(s_client == NULL && aborted == 2);

    fixture();
    uint8_t bad[] = {0, 1, 0, 0, 0, 2, 1, 16};
    deliver(bad, sizeof(bad));
    deliver(frame, sizeof(frame));
    modbus_boot_server_poll();
    assert(s_client == NULL && aborted == 1 && !responses);

    fixture();
    struct pbuf oversized = {NULL, frame, RX_BUF_SIZE + 1, RX_BUF_SIZE + 1};
    assert(on_recv(NULL, &client, &oversized, ERR_OK) == ERR_MEM);
    assert(!freed && !s_rx_count && !acknowledged);
    assert(on_recv(NULL, &client, NULL, ERR_OK) == ERR_ABRT);
    assert(on_accept(NULL, &other, ERR_OK) == ERR_OK);
    puts("TCP framing, backpressure, timeout and reconnect tests passed");
    return 0;
}
