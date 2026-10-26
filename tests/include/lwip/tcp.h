#ifndef TEST_TCP_H
#define TEST_TCP_H
#include <stdint.h>
typedef int err_t;
typedef uint16_t u16_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_ABRT -2
#define ERR_VAL -3
#define TCP_WND 5840
#define TCP_WRITE_FLAG_COPY 1
#define SOF_KEEPALIVE 1
#define IP_ADDR_ANY ((void *)0)
#define ip_set_option(pcb, option) ((void)(pcb), (void)(option))
struct tcp_pcb { uint32_t keep_idle, keep_intvl, keep_cnt; };
struct pbuf { struct pbuf *next; void *payload; uint16_t len, tot_len; };
void tcp_arg(struct tcp_pcb *, void *);
void tcp_recv(struct tcp_pcb *, err_t (*)(void *, struct tcp_pcb *, struct pbuf *, err_t));
void tcp_err(struct tcp_pcb *, void (*)(void *, err_t));
void tcp_accept(struct tcp_pcb *, err_t (*)(void *, struct tcp_pcb *, err_t));
void tcp_abort(struct tcp_pcb *);
err_t tcp_close(struct tcp_pcb *);
struct tcp_pcb *tcp_new(void);
err_t tcp_bind(struct tcp_pcb *, const void *, uint16_t);
struct tcp_pcb *tcp_listen(struct tcp_pcb *);
void tcp_recved(struct tcp_pcb *, uint16_t);
err_t tcp_write(struct tcp_pcb *, const void *, uint16_t, uint8_t);
err_t tcp_output(struct tcp_pcb *);
uint8_t pbuf_free(struct pbuf *);
#endif
