/**
 * @file  boot_main.c
 * @brief Bootloader state machine and super-loop.
 */

#include "boot_main.h"

#include "boot_state.h"
#include "boot_entry.h"
#include "boot_jump.h"
#include "flash_map.h"
#include "metadata.h"
#include "app_validate.h"
#include "led_indication.h"
#include "modbus_boot_server.h"
#include "fw_update_proto.h"
#include "fw_installer.h"

#include "stm32f4xx_hal.h"
#include "lwip.h"
#include "ethernetif.h"
#include "lwip/timeouts.h"

/* ---- Module state ------------------------------------------------------- */
static metadata_t  s_meta;
static boot_state_t s_state;

/* ---- Ethernet poll (bare-metal LwIP) ------------------------------------ */
extern struct netif gnetif;

static void ethernet_poll(void)
{
    ethernetif_input(&gnetif);
    sys_check_timeouts();
}

/* ---- State machine ------------------------------------------------------ */
static void sm_start(void)
{
    led_indication_init();
    metadata_load(&s_meta);
    s_state = BOOT_CHECK_ENTRY;
}

static void sm_check_entry(void)
{
    if (boot_entry_should_stay(&s_meta)) {
        /* If an installation was interrupted, resume it. */
        if (s_meta.install_in_progress) {
            s_meta.boot_state = (uint32_t)BOOT_INSTALL_FW;
            s_state = BOOT_INSTALL_FW;
            led_indication_set(LED_PATTERN_INSTALLING);
        } else if (s_meta.install_requested && s_meta.staging_valid) {
            s_meta.boot_state = (uint32_t)BOOT_INSTALL_FW;
            s_state = BOOT_INSTALL_FW;
            led_indication_set(LED_PATTERN_INSTALLING);
        } else {
            s_meta.boot_state = (uint32_t)BOOT_WAIT_COMMAND;
            s_state = BOOT_WAIT_COMMAND;
            led_indication_set(LED_PATTERN_IDLE);

            MX_LWIP_Init();
            modbus_boot_server_init(&s_meta, 502u);
        }
    } else {
        s_state = BOOT_READY_TO_BOOT;
    }
}

static void sm_wait_command(void)
{
    ethernet_poll();
    modbus_boot_server_poll();
    fw_proto_poll(&s_meta);

    if (fw_proto_reboot_requested()) {
        HAL_NVIC_SystemReset();
    }

    /* Transition to RECEIVE_FW is done inside fw_update_proto via BEGIN_UPDATE. */
    if (s_meta.boot_state == (uint32_t)BOOT_RECEIVE_FW) {
        s_state = BOOT_RECEIVE_FW;
        led_indication_set(LED_PATTERN_RECEIVING);
    }
}

static void sm_receive_fw(void)
{
    ethernet_poll();
    modbus_boot_server_poll();
    fw_proto_poll(&s_meta);

    if (fw_proto_reboot_requested()) {
        HAL_NVIC_SystemReset();
    }

    if (s_meta.boot_state == (uint32_t)BOOT_VERIFY_STAGING) {
        s_state = BOOT_VERIFY_STAGING;
    }

    if (s_meta.boot_state == (uint32_t)BOOT_WAIT_COMMAND) {
        s_state = BOOT_WAIT_COMMAND;
        led_indication_set(LED_PATTERN_IDLE);
    }
}

static void sm_verify_staging(void)
{
    ethernet_poll();
    modbus_boot_server_poll();
    fw_proto_poll(&s_meta);

    if (fw_proto_reboot_requested()) {
        HAL_NVIC_SystemReset();
    }

    if (fw_proto_install_requested()) {
        s_state = BOOT_INSTALL_FW;
        s_meta.boot_state = (uint32_t)BOOT_INSTALL_FW;
        led_indication_set(LED_PATTERN_INSTALLING);
        fw_installer_start(&s_meta);
    }
}

static void sm_install_fw(void)
{
    installer_state_t ist = fw_installer_poll(&s_meta);

    switch (ist) {
    case INST_DONE:
        fw_proto_install_done();
        s_state = BOOT_VERIFY_APP;
        break;
    case INST_ERROR:
        fw_proto_install_error();
        s_meta.boot_state = (uint32_t)BOOT_ERROR;
        s_state = BOOT_ERROR;
        led_indication_set(LED_PATTERN_ERROR);
        break;
    default:
        break;
    }
}

static void sm_verify_app(void)
{
    /* Verify the firmware header in the installed image carries the
     * correct product identity (redundant check after installation). */
    if (!app_validate_header(APP_FLASH_BASE, PRODUCT_ID_DEFAULT,
                             (uint16_t)HW_REVISION_DEFAULT)) {
        s_meta.app_valid   = 0u;
        s_meta.last_error  = BOOT_ERR_PRODUCT_MISMATCH;
        s_meta.boot_state  = (uint32_t)BOOT_ERROR;
        metadata_save(&s_meta);
        s_state = BOOT_ERROR;
        led_indication_set(LED_PATTERN_ERROR);
        return;
    }
    if (app_validate_full(APP_FLASH_BASE,
                          s_meta.app_image_size,
                          s_meta.app_image_crc32)) {
        s_state = BOOT_READY_TO_BOOT;
    } else {
        s_meta.app_valid   = 0u;
        s_meta.last_error  = BOOT_ERR_APP_VALIDATE;
        s_meta.boot_state  = (uint32_t)BOOT_ERROR;
        metadata_save(&s_meta);
        s_state = BOOT_ERROR;
        led_indication_set(LED_PATTERN_ERROR);
    }
}

static void sm_ready_to_boot(void)
{
    boot_jump_to_app(APP_FLASH_BASE);
}

static void sm_error(void)
{
    ethernet_poll();
    modbus_boot_server_poll();
    fw_proto_poll(&s_meta);

    if (fw_proto_reboot_requested()) {
        HAL_NVIC_SystemReset();
    }

    /* Allow restarting an update from the error state. */
    if (s_meta.boot_state == (uint32_t)BOOT_RECEIVE_FW) {
        s_state = BOOT_RECEIVE_FW;
        led_indication_set(LED_PATTERN_RECEIVING);
    }
}

/* ---- Public entry point ------------------------------------------------- */
void boot_run(void)
{
    s_state = BOOT_START;

    for (;;) {
        uint32_t now = HAL_GetTick();
        led_indication_poll(now);

        switch (s_state) {
        case BOOT_START:          sm_start();          break;
        case BOOT_CHECK_ENTRY:    sm_check_entry();    break;
        case BOOT_WAIT_COMMAND:   sm_wait_command();   break;
        case BOOT_PREPARE_UPDATE: /* handled in proto */break;
        case BOOT_RECEIVE_FW:     sm_receive_fw();     break;
        case BOOT_VERIFY_STAGING: sm_verify_staging();  break;
        case BOOT_INSTALL_FW:     sm_install_fw();      break;
        case BOOT_VERIFY_APP:     sm_verify_app();      break;
        case BOOT_READY_TO_BOOT:  sm_ready_to_boot();   break;
        case BOOT_ERROR:          sm_error();            break;
        }
    }
}
