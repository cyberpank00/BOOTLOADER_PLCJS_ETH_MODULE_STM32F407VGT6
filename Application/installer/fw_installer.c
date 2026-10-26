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
static uint32_t          s_erase_idx;

static void installer_fail(metadata_t *meta, boot_error_t error)
{
    s_state = INST_ERROR;
    meta->boot_state = BOOT_ERROR;
    meta->last_error = error;
    meta->install_requested = 0u;
    meta->install_in_progress = 0u;
    if (!metadata_save(meta)) meta->last_error = BOOT_ERR_FLASH_WRITE;
}

bool fw_installer_start(metadata_t *meta)
{
    if (!meta->staging_valid || !metadata_params_valid(meta) ||
        !app_validate_image(STAGING_FLASH_BASE, meta->image_size, meta->image_crc32)) {
        installer_fail(meta, BOOT_ERR_APP_VALIDATE);
        return false;
    }
    metadata_t next = *meta;
    next.install_requested = 1u;
    next.install_in_progress = 1u;
    next.app_valid = 0u;
    next.boot_state = BOOT_INSTALL_FW;
    next.last_error = BOOT_ERR_NONE;
    if (!metadata_save(&next)) {
        s_state = INST_ERROR;
        meta->boot_state = BOOT_ERROR;
        meta->last_error = BOOT_ERR_FLASH_WRITE;
        return false;
    }
    *meta = next;

    s_state     = INST_ERASING;
    s_offset    = 0u;
    s_erase_idx = 0u;
    return true;
}

installer_state_t fw_installer_poll(metadata_t *meta)
{
    switch (s_state) {
    case INST_ERASING: {
        /* Erase one app sector per poll so the super-loop can service the
         * network between sectors (a 2-sector erase in one call blocks ~3-4 s
         * and would time out the updater's status poll). */
        uint32_t count = APP_LAST_SECTOR - APP_FIRST_SECTOR + 1u;
        if (!flash_if_erase_sectors(APP_FIRST_SECTOR + s_erase_idx, 1u)) {
            installer_fail(meta, BOOT_ERR_FLASH_ERASE);
            break;
        }
        s_erase_idx++;
        if (s_erase_idx >= count) {
            s_state  = INST_COPYING;
            s_offset = 0u;
        }
        break;
    }

    case INST_COPYING: {
        if (!metadata_params_valid(meta) || s_offset >= meta->image_size) {
            installer_fail(meta, BOOT_ERR_BAD_PARAMS);
            break;
        }
        uint32_t remaining = meta->image_size - s_offset;
        uint32_t chunk     = (remaining > COPY_CHUNK) ? COPY_CHUNK : remaining;

        if (!flash_if_write(APP_FLASH_BASE + s_offset,
                            (const uint8_t *)(STAGING_FLASH_BASE + s_offset),
                            chunk)) {
            installer_fail(meta, BOOT_ERR_FLASH_WRITE);
            break;
        }

        s_offset += chunk;
        if (s_offset >= meta->image_size) {
            s_state = INST_VERIFYING;
        }
        break;
    }

    case INST_VERIFYING: {
        if (!app_validate_full(APP_FLASH_BASE, meta->image_size, meta->image_crc32)) {
            installer_fail(meta, BOOT_ERR_APP_VALIDATE);
            break;
        }

        metadata_t next = *meta;
        next.app_valid           = 1u;
        next.install_requested   = 0u;
        next.install_in_progress = 0u;
        next.staging_valid       = 0u;
        /* Read version from the installed image header (ground truth). */
        {
            fw_header_t hdr;
            next.app_fw_version = app_read_header(APP_FLASH_BASE, &hdr)
                                   ? hdr.fw_version : meta->fw_version;
        }
        next.app_image_size      = meta->image_size;
        next.app_image_crc32     = meta->image_crc32;
        next.boot_state          = (uint32_t)BOOT_READY_TO_BOOT;
        next.last_error          = (uint32_t)BOOT_ERR_NONE;
        if (!metadata_save(&next)) {
            installer_fail(meta, BOOT_ERR_FLASH_WRITE);
            break;
        }
        *meta = next;

        s_state = INST_DONE;
        break;
    }

    case INST_ERROR:
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
