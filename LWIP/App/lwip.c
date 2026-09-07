/**
 * @file  lwip.c
 * @brief Bare-metal LwIP initialisation (static IP, NO_SYS = 1).
 */

#include "lwip.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "netif/ethernet.h"
#include "ethernetif.h"

#include "net_id.h"
#include "netbiosns.h"
#include "flash_map.h"

struct netif gnetif;

/* Short variant token for the NetBIOS name, derived from the compiled-in
 * product id (the bootloader is universal; PRODUCT_ID_DEFAULT is set per
 * variant at build time). */
static const char* variant_token(void)
{
    switch (PRODUCT_ID_DEFAULT) {
    case 0x504C1201u: return "12DI";
    case 0x504C1202u: return "12DO";
    case 0x504C0403u: return "4RTD";
    case 0x504C0804u: return "8AIC";
    case 0x504C0404u: return "4AIC";
    case 0x504C0405u: return "4AIV";
    case 0x504C0406u: return "4AO";
    default:          return "MOD";
    }
}

/* Build "BL-<variant>-<MAC3>" (<= 14 chars, fits the 15-char NetBIOS limit)
 * so each board is uniquely identifiable by its MAC suffix. */
static void build_netbios_name(char* out /* >= 16 */)
{
    static const char hexd[] = "0123456789ABCDEF";
    uint8_t mac[6];
    net_id_get_mac(mac);

    char* p = out;
    *p++ = 'B'; *p++ = 'L'; *p++ = '-';
    for (const char* s = variant_token(); *s != '\0'; ) { *p++ = *s++; }
    *p++ = '-';
    *p++ = hexd[(mac[3] >> 4) & 0xF]; *p++ = hexd[mac[3] & 0xF];
    *p++ = hexd[(mac[4] >> 4) & 0xF]; *p++ = hexd[mac[4] & 0xF];
    *p++ = hexd[(mac[5] >> 4) & 0xF]; *p++ = hexd[mac[5] & 0xF];
    *p   = '\0';
}

void MX_LWIP_Init(void)
{
    ip4_addr_t ipaddr, netmask, gw;

    lwip_init();

    /* Factory / default addressing: AutoIP link-local (RFC 3927,
     * 169.254.mac[4].mac[5]), matching the application's factory scheme so a
     * bootloader-mode device sits on the same wire and is found by the
     * discovery tool (which resolves the boot IP by MAC). */
    uint8_t ll[4];
    net_id_get_linklocal(ll);
    IP4_ADDR(&ipaddr,  ll[0], ll[1], ll[2], ll[3]);
    IP4_ADDR(&netmask, 255, 255, 0, 0);
    ip4_addr_set_zero(&gw);

    netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL,
              &ethernetif_init, &ethernet_input);
    netif_set_default(&gnetif);
    netif_set_up(&gnetif);

    /* Advertise a NetBIOS name so the (static-IP) bootloader is identifiable
     * on the LAN, e.g. "BL-12DI-B05DF7". */
    {
        static char nbname[16];
        build_netbios_name(nbname);
        netbiosns_set_name(nbname);
        netbiosns_init();
    }
}
