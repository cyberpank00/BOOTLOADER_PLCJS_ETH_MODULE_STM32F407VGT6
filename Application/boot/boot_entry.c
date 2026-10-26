/**
 * @file  boot_entry.c
 * @brief Boot entry decision logic.
 */

#include "boot_entry.h"
#include "flash_map.h"
#include "app_validate.h"
#include "stm32f4xx_hal.h"

bool boot_entry_software_request(void)
{
    /* The application requests bootloader mode by writing BOOT_REQUEST_MAGIC
     * into a no-init RAM cell and resetting. Consume it (one-shot) so a later
     * power cycle boots the application normally. */
    volatile uint32_t *flag = (volatile uint32_t *)BOOT_REQUEST_FLAG_ADDR;
    if (*flag == BOOT_REQUEST_MAGIC) {
        *flag = 0u;
        return true;
    }
    return false;
}

bool boot_entry_should_stay(const metadata_t *meta)
{
    /* 1. Software request from the application (magic in no-init RAM). The
     *    physical button is no longer used to enter the bootloader — in the
     *    application PC6 is dedicated to factory reset. */
    if (boot_entry_software_request()) {
        return true;
    }

    /* 2. Installation was interrupted — must finish or retry */
    if (meta->install_in_progress) {
        return true;
    }

    /* 3. Explicit request to enter bootloader */
    if (meta->install_requested || meta->staging_valid ||
        meta->boot_state == BOOT_ERROR) {
        return true;
    }

    /* 4. Firmware reception was in progress */
    if (meta->boot_state == BOOT_RECEIVE_FW ||
        meta->boot_state == BOOT_PREPARE_UPDATE) {
        return true;
    }

    /* 5. Application not marked valid */
    if (!meta->app_valid) {
        return true;
    }

    /* 6. Validate the installed image before every boot. */
    if (!app_validate_full(APP_FLASH_BASE, meta->app_image_size, meta->app_image_crc32)) {
        return true;
    }

    return false;
}
