/**
 * @file  app_validate.c
 * @brief Application image validation: vectors, header, CRC32.
 */

#include "app_validate.h"
#include "flash_map.h"
#include "crc32.h"
#include <string.h>

bool app_validate_vectors(uint32_t app_base)
{
    return app_validate_vectors_at(app_base, APP_FLASH_SIZE);
}

bool app_validate_vectors_at(uint32_t app_base, uint32_t image_size)
{
    if ((app_base != APP_FLASH_BASE && app_base != STAGING_FLASH_BASE) ||
        image_size < FW_HEADER_OFFSET + sizeof(fw_header_t) ||
        image_size > APP_FLASH_SIZE)
        return false;
    const uint32_t *vectors = (const uint32_t *)app_base;

    uint32_t msp           = vectors[0];
    uint32_t reset_handler  = vectors[1];

    /* MSP must point into RAM */
    if (msp <= RAM_BASE || msp > BOOT_REQUEST_FLAG_ADDR || (msp & 7u) != 0u) {
        return false;
    }

    /* Reset_Handler must point into the application Flash region */
    uint32_t target = reset_handler & ~1u;
    if ((reset_handler & 1u) == 0u || target < APP_FLASH_BASE ||
        target - APP_FLASH_BASE > image_size - 2u) {
        return false;
    }

    return true;
}

bool app_read_header(uint32_t app_base, fw_header_t *hdr)
{
    memcpy(hdr, (const void *)(app_base + FW_HEADER_OFFSET), sizeof(*hdr));
    return hdr->magic == FW_IMAGE_MAGIC;
}

bool app_validate_full(uint32_t app_base, uint32_t expected_size,
                       uint32_t expected_crc)
{
    return app_validate_image(app_base, expected_size, expected_crc);
}

bool app_validate_image(uint32_t app_base, uint32_t expected_size,
                        uint32_t expected_crc)
{
    if (!app_validate_vectors_at(app_base, expected_size)) {
        return false;
    }

    if (!app_validate_header(app_base, PRODUCT_ID_DEFAULT,
                             HW_REVISION_DEFAULT_U16)) {
        return false;
    }

    uint32_t crc = crc32_calc((const uint8_t *)app_base, expected_size);
    return crc == expected_crc;
}

bool app_validate_header(uint32_t app_base,
                         uint32_t expected_product_id,
                         uint16_t expected_hw_rev)
{
    fw_header_t hdr;
    if (!app_read_header(app_base, &hdr)) {
        return false;  /* magic mismatch -- no valid header */
    }
    if (hdr.product_id != expected_product_id || hdr.vector_table_offset != 0u) {
        return false;
    }
    /* Compare major byte only: (revision >> 8).
     * Minor byte (non-pinout HW changes) does not break FW compatibility. */
    if ((hdr.hw_revision >> 8u) != (expected_hw_rev >> 8u)) {
        return false;
    }
    return true;
}
