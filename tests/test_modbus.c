#include <assert.h>
#include <stdio.h>
#include "../Application/third_party/nanomodbus/nanomodbus.c"

static uint8_t input[1024];
static size_t input_len, cursor;
static unsigned calls, writes;

static int read_byte(uint8_t *out, int32_t timeout, void *arg)
{
    (void)timeout;
    (void)arg;
    if (cursor == input_len) return 0;
    *out = input[cursor++];
    return 1;
}

static int write_byte(uint8_t value, int32_t timeout, void *arg)
{
    (void)value;
    (void)timeout;
    (void)arg;
    writes++;
    return 1;
}

static void sleep_ms(uint32_t ms, void *arg) { (void)ms; (void)arg; }

static nmbs_error on_registers(uint16_t addr, uint16_t qty, const uint16_t *regs)
{
    (void)addr;
    (void)qty;
    (void)regs;
    calls++;
    return NMBS_ERROR_NONE;
}

static nmbs_error on_coils(uint16_t addr, uint16_t qty, const nmbs_bitfield coils)
{
    (void)addr;
    (void)qty;
    (void)coils;
    calls++;
    return NMBS_ERROR_NONE;
}

static nmbs_t server(void)
{
    nmbs_t n;
    nmbs_platform_conf p = {NMBS_TRANSPORT_TCP, read_byte, write_byte, sleep_ms, NULL};
    nmbs_callbacks cb = {0};
    cb.write_multiple_registers = on_registers;
    cb.write_multiple_coils = on_coils;
    assert(nmbs_server_create(&n, 1, &p, &cb) == NMBS_ERROR_NONE);
    return n;
}

static void frame(uint8_t fc, uint16_t qty, uint8_t bytes, uint16_t length)
{
    memset(input, 0, sizeof(input));
    input[4] = (uint8_t)(length >> 8);
    input[5] = (uint8_t)length;
    input[6] = 1;
    input[7] = fc;
    input[10] = (uint8_t)(qty >> 8);
    input[11] = (uint8_t)qty;
    input[12] = bytes;
    input_len = 13u + bytes;
    cursor = calls = writes = 0;
}

int main(void)
{
    nmbs_t n = server();
    frame(16, 123, 246, 253);
    assert(nmbs_server_poll(&n) == NMBS_ERROR_NONE && calls == 1);
    frame(15, 8, 1, 8);
    assert(nmbs_server_poll(&n) == NMBS_ERROR_NONE && calls == 1);
    for (unsigned bytes = 0; bytes <= 255; bytes++) {
        frame(16, 123, (uint8_t)bytes, 253);
        nmbs_server_poll(&n);
        assert(calls == (bytes == 246));
    }
    frame(16, 1, 2, 2);
    assert(nmbs_server_poll(&n) < 0 && calls == 0);
    frame(16, 1, 2, 10);
    assert(nmbs_server_poll(&n) < 0 && calls == 0);
    frame(16, 1, 2, 255);
    assert(nmbs_server_poll(&n) < 0 && calls == 0);
    frame(16, 1, 2, 9);
    input_len--;
    assert(nmbs_server_poll(&n) < 0 && calls == 0);
    n.msg.buf_idx = sizeof(n.msg.buf);
    assert(recv(&n, 1) == NMBS_ERROR_TRANSPORT);
    puts("Modbus bounds and framing tests passed");
    return 0;
}
