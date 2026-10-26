#ifndef TEST_ETHERNETIF_H
#define TEST_ETHERNETIF_H
struct netif { int unused; };
void ethernetif_input(struct netif *netif);
#endif
