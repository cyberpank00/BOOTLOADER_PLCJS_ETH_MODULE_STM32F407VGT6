/**
 * @file  ethernetif.c
 * @brief Bare-metal Ethernet interface for LwIP (NO_SYS = 1).
 *
 * Uses STM32 HAL ETH v2 API with RxAllocate/RxLink/TxFree callbacks.
 */

#include "main.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"
#include "ethernetif.h"
#include "eth_custom_phy_interface.h"
#include "net_id.h"
#include <string.h>

#define IFNAME0 's'
#define IFNAME1 't'

ETH_HandleTypeDef          heth;
ETH_TxPacketConfigTypeDef  TxConfig;

static ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]
                            __attribute__((section(".bss"), aligned(4)));
static ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]
                            __attribute__((section(".bss"), aligned(4)));

static user_phy_Object_t   phyObj;
static volatile uint8_t    s_frame_received;

/* ---- Rx slot tracking (maps DMA buffer pointer back to its pbuf) --------
 *
 * HAL ETH v2 splits Rx into two callbacks:
 *   RxAllocateCallback -- called by HAL to get a buffer for the DMA to fill.
 *                         We allocate a pbuf and return its payload pointer.
 *   RxLinkCallback     -- called after DMA fills the buffer; we must build a
 *                         pbuf chain from the raw buffer pointer + length.
 *
 * Because the HAL only passes the raw buffer pointer (not the pbuf*) to
 * RxLinkCallback, we keep a small lookup table of (pbuf*, buffer*) pairs so
 * we can recover the pbuf* from the buffer* without any fragile arithmetic.
 *
 * The pbuf is owned by the table until RxLinkCallback claims it, then it is
 * owned by the netif input path, which calls pbuf_free() after processing.
 * This ensures zero leaks from the pbuf pool.
 */
typedef struct { struct pbuf *p; uint8_t *buff; } rx_slot_t;
static rx_slot_t s_rx_slots[ETH_RX_DESC_CNT];

/* ---- PHY I/O wrappers -------------------------------------------------- */
static int32_t phy_io_init(void)    { return 0; }
static int32_t phy_io_deinit(void)  { return 0; }

static int32_t phy_io_read(uint32_t addr, uint32_t reg, uint32_t *val)
{
    if (HAL_ETH_ReadPHYRegister(&heth, addr, reg, val) == HAL_OK)
        return 0;
    return -1;
}

static int32_t phy_io_write(uint32_t addr, uint32_t reg, uint32_t val)
{
    if (HAL_ETH_WritePHYRegister(&heth, addr, reg, val) == HAL_OK)
        return 0;
    return -1;
}

static int32_t phy_io_tick(void)
{
    return (int32_t)HAL_GetTick();
}

/* ---- HAL ETH Rx callbacks ---------------------------------------------- */
void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
    for (uint32_t i = 0; i < ETH_RX_DESC_CNT; i++) {
        if (s_rx_slots[i].p == NULL) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, ETH_RX_BUF_SIZE, PBUF_POOL);
            if (p != NULL) {
                s_rx_slots[i].p    = p;
                s_rx_slots[i].buff = (uint8_t *)p->payload;
                *buff = (uint8_t *)p->payload;
            } else {
                *buff = NULL;
            }
            return;
        }
    }
    *buff = NULL; /* all slots busy */
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd,
                             uint8_t *buff, uint16_t Length)
{
    struct pbuf **ppStart = (struct pbuf **)pStart;
    struct pbuf **ppEnd   = (struct pbuf **)pEnd;
    struct pbuf *p = NULL;

    /* Recover the pbuf that owns this DMA buffer */
    for (uint32_t i = 0; i < ETH_RX_DESC_CNT; i++) {
        if (s_rx_slots[i].buff == buff) {
            p = s_rx_slots[i].p;
            s_rx_slots[i].p    = NULL;
            s_rx_slots[i].buff = NULL;
            break;
        }
    }
    if (p == NULL) return;

    p->len     = Length;
    p->tot_len = Length;
    p->next    = NULL;

    if (*ppStart == NULL) {
        *ppStart = p;
    } else {
        /* Append to chain, update tot_len of all preceding pbufs */
        for (struct pbuf *q = *ppStart; q != NULL; q = q->next)
            q->tot_len += Length;
        (*ppEnd)->next = p;
    }
    *ppEnd = p;
}

/* ---- HAL ETH Tx callback ----------------------------------------------- */
void HAL_ETH_TxFreeCallback(uint32_t *buff)
{
    /* pData was set to NULL in low_level_output — nothing to free here.
     * Pbuf lifetime is managed by the LwIP stack in main-loop context,
     * avoiding pool corruption from IRQ-context pbuf_free calls. */
    (void)buff;
}

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
    (void)handlerEth;
    s_frame_received = 1;
}

/* ---- Low-level init ----------------------------------------------------- */
static void low_level_init(struct netif *netif)
{
    uint8_t mac[6];

    net_id_get_mac(mac);

    heth.Instance            = ETH;
    heth.Init.MACAddr        = mac;
    heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
    heth.Init.RxDesc         = DMARxDscrTab;
    heth.Init.TxDesc         = DMATxDscrTab;
    heth.Init.RxBuffLen      = ETH_RX_BUF_SIZE;

    HAL_ETH_Init(&heth);

    memset(&TxConfig, 0, sizeof(TxConfig));
    TxConfig.Attributes   = ETH_TX_PACKETS_FEATURES_CSUM
                          | ETH_TX_PACKETS_FEATURES_CRCPAD;
    TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
    TxConfig.CRCPadCtrl   = ETH_CRC_PAD_INSERT;

    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, mac, 6);
    netif->mtu   = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

    /* PHY init */
    user_phy_IOCtx_t io = {
        .Init     = phy_io_init,
        .DeInit   = phy_io_deinit,
        .ReadReg  = phy_io_read,
        .WriteReg = phy_io_write,
        .GetTick  = phy_io_tick,
    };
    USER_PHY_RegisterBusIO(&phyObj, &io);
    USER_PHY_Init(&phyObj);

    HAL_ETH_Start_IT(&heth);
}

/* ---- Transmit ----------------------------------------------------------- */

/* Static TX buffer: the pbuf chain is copied here before handing off to the
 * DMA.  This decouples pbuf lifetime from the DMA transfer entirely:
 *   - No pbuf_ref() needed — the caller (LwIP) frees the pbuf normally.
 *   - TxFreeCallback becomes a no-op (pData = NULL).
 *   - No pbuf_free() from IRQ context → no pool corruption under
 *     SYS_LIGHTWEIGHT_PROT = 0.
 * Cost: one memcpy per TX frame + ETH_MAX_PACKET_SIZE (1528 B) of RAM. */
static uint8_t s_tx_buf[ETH_MAX_PACKET_SIZE];

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    (void)netif;

    /* Flatten the pbuf chain into the static DMA buffer */
    uint16_t total = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if ((uint32_t)total + q->len > sizeof(s_tx_buf))
            return ERR_BUF;
        memcpy(s_tx_buf + total, q->payload, q->len);
        total += q->len;
    }

    ETH_BufferTypeDef buf = { .buffer = s_tx_buf, .len = total, .next = NULL };

    TxConfig.Length   = total;
    TxConfig.TxBuffer = &buf;
    TxConfig.pData    = NULL;   /* TxFreeCallback is a no-op */

    /* Wait for any previous TX to finish (typically < 100 us) */
    uint32_t t = HAL_GetTick();
    while (HAL_ETH_GetState(&heth) != HAL_ETH_STATE_STARTED) {
        if (HAL_GetTick() - t > 50u) return ERR_IF;
    }

    return (HAL_ETH_Transmit_IT(&heth, &TxConfig) == HAL_OK) ? ERR_OK : ERR_IF;
}

/* ---- Receive ------------------------------------------------------------ */
void ethernetif_input(struct netif *netif)
{
    if (!s_frame_received) return;
    s_frame_received = 0;

    struct pbuf *p = NULL;
    HAL_ETH_ReadData(&heth, (void **)&p);

    while (p != NULL) {
        if (netif->input(p, netif) != ERR_OK) {
            pbuf_free(p);
        }
        p = NULL;
        HAL_ETH_ReadData(&heth, (void **)&p);
    }
}

err_t ethernetif_init(struct netif *netif)
{
    netif->name[0]    = IFNAME0;
    netif->name[1]    = IFNAME1;
    netif->output     = etharp_output;
    netif->linkoutput = low_level_output;
    low_level_init(netif);
    return ERR_OK;
}

/* ---- HAL ETH MSP (clock, GPIO, IRQ) ------------------------------------ */
void HAL_ETH_MspInit(ETH_HandleTypeDef *ethHandle)
{
    (void)ethHandle;
    GPIO_InitTypeDef gi = {0};

    /* Enable peripheral clocks FIRST so the reset propagates with the
     * clock running -- required after warm resets where the MAC/DMA may
     * retain stale state from a previous session. */
    __HAL_RCC_ETH_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* Force hardware reset of the Ethernet MAC/DMA peripheral. */
    __HAL_RCC_ETHMAC_FORCE_RESET();
    for (volatile int i = 0; i < 100; i++) {}  /* brief settling delay */
    __HAL_RCC_ETHMAC_RELEASE_RESET();
    for (volatile int i = 0; i < 100; i++) {}  /* brief settling delay */

    /* RMII pins:
     * PA1  = ETH_REF_CLK, PA2 = ETH_MDIO, PA7 = ETH_CRS_DV
     * PB11 = ETH_TX_EN, PB12 = ETH_TXD0, PB13 = ETH_TXD1
     * PC1  = ETH_MDC, PC4 = ETH_RXD0, PC5 = ETH_RXD1
     */
    gi.Mode      = GPIO_MODE_AF_PP;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = GPIO_AF11_ETH;

    gi.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &gi);

    gi.Pin = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOB, &gi);

    gi.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &gi);

    HAL_NVIC_SetPriority(ETH_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);
}

void HAL_ETH_MspDeInit(ETH_HandleTypeDef *ethHandle)
{
    (void)ethHandle;
    __HAL_RCC_ETH_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13);
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);
    HAL_NVIC_DisableIRQ(ETH_IRQn);
}
