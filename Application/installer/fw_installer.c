/**
 * @file  fw_installer.c
 * @brief Copy verified staging image into the application Flash region.
 */

#include "fw_installer.h"
#include "flash_map.h"
#include "flash_if.h"
#include "crc32.h"
#include "app_validate.h"
#include "boot_state.h"
#include <string.h>

#define COPY_CHUNK  256u

static installer_state_t s_state;
static uint32_t          s_offset;

void fw_installer_start(metadata_t *meta)
{
    meta->install_in_progress = 1u;
    meta->app_valid           = 0u;
    metadata_save(meta);

    s_state  = INST_ERASING;
    s_offset = 0u;
}

installer_state_t fw_installer_poll(metadata_t *meta)
{
    switch (s_state) {
    case INST_ERASING: {
        uint32_t count = APP_LAST_SECTOR - APP_FIRST_SECTOR + 1u;
        if (!flash_if_erase_sectors(APP_FIRST_SECTOR, count)) {
            meta->last_error = BOOT_ERR_FLASH_ERASE;
            s_state = INST_ERROR;
            break;
        }
        s_state  = INST_COPYING;
        s_offset = 0u;
        break;
    }

    case INST_COPYING: {
        uint32_t remaining = meta->image_size - s_offset;
        uint32_t chunk     = (remaining > COPY_CHUNK) ? COPY_CHUNK : remaining;

        if (!flash_if_write(APP_FLASH_BASE + s_offset,
                            (const uint8_t *)(STAGING_FLASH_BASE + s_offset),
                            chunk)) {
            meta->last_error = BOOT_ERR_FLASH_WRITE;
            s_state = INST_ERROR;
            break;
        }

        s_offset += chunk;
        if (s_offset >= meta->image_size) {
            s_state = INST_VERIFYING;
        }
        break;
    }

    case INST_VERIFYING: {
        uint32_t crc = crc32_calc((const uint8_t *)APP_FLASH_BASE,
                                   meta->image_size);
        if (crc != meta->image_crc32) {
            meta->last_error = BOOT_ERR_APP_VALIDATE;
            s_state = INST_ERROR;
            break;
        }
        if (!app_validate_vectors(APP_FLASH_BASE)) {
            meta->last_error = BOOT_ERR_APP_VALIDATE;
            s_state = INST_ERROR;
            break;
        }
        if (!app_validate_header(APP_FLASH_BASE, PRODUCT_ID_DEFAULT,
                                 (uint16_t)HW_REVISION_DEFAULT)) {
            meta->last_error = BOOT_ERR_PRODUCT_MISMATCH;
            s_state = INST_ERROR;
            break;
        }

        meta->app_valid           = 1u;
        meta->install_requested   = 0u;
        meta->install_in_progress = 0u;
        meta->staging_valid       = 0u;
        meta->app_fw_version      = meta->fw_version;
        meta->app_image_size      = meta->image_size;
        meta->app_image_crc32     = meta->image_crc32;
        meta->boot_state          = (uint32_t)BOOT_READY_TO_BOOT;
        meta->last_error          = (uint32_t)BOOT_ERR_NONE;
        metadata_save(meta);

        s_state = INST_DONE;
        break;
    }

    case INST_ERROR:
        meta->install_in_progress = 0u;
        meta->app_valid           = 0u;
        metadata_save(meta);
        break;

    case INST_IDLE:
    case INST_DONE:
    default:
        break;
    }

    return s_state;
}

installer_state_t fw_installer_get_state(void)
{
    return s_state;
}
