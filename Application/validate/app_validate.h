/**
 * @file  app_validate.h
 * @brief Application image validation.
 */
#ifndef APP_VALIDATE_H
#define APP_VALIDATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Firmware image header.  Must be placed at a known offset inside the
 * application image so that the bootloader can locate it.
 *
 * In the default layout the header lives at APP_FLASH_BASE + 0x200
 * (after the 512-byte vector table).
 *
 * hw_revision encoding: (major << 8) | minor
 *   major — increments on MCU pinout changes (requires new FW major)
 *   minor — increments on non-pinout HW changes (FW-compatible)
 *   patch  — cosmetic PCB changes, not stored in firmware
 *
 * fw_version encoding: (major << 8) | minor
 *   major — must equal hw_revision major for OTA acceptance
 *   minor — increments on firmware-only changes
 *
 * Bootloader OTA acceptance rule:
 *   product_id exact match  AND  (header.hw_revision >> 8) == (HW_REVISION_DEFAULT >> 8)
 *
 * CRC32 and image_size are NOT stored in the header — they come from OTA metadata.
 */
#define FW_HEADER_OFFSET    0x200u

typedef struct __attribute__((packed)) {
    uint32_t magic;              /* FW_IMAGE_MAGIC                       */
    uint32_t product_id;         /* Module type identifier (exact match) */
    uint16_t hw_revision;        /* (major << 8) | minor                 */
    uint16_t reserved0;
    uint32_t fw_version;         /* (major << 8) | minor                 */
    uint32_t vector_table_offset;/* usually 0                            */
    uint32_t reserved1[2];
} fw_header_t;

/** Quick check: MSP and Reset_Handler point to valid memory ranges. */
bool app_validate_vectors(uint32_t app_base);

/** Full validation: vectors + header + CRC32 of the whole image. */
bool app_validate_full(uint32_t app_base, uint32_t expected_size,
                       uint32_t expected_crc);

/** Read the firmware header from the given base address. */
bool app_read_header(uint32_t app_base, fw_header_t *hdr);

/**
 * Check that the firmware header embedded in the image at @p app_base
 * carries the expected product identity.
 *
 * product_id must match exactly.
 * hw_revision is checked by major byte only: (header >> 8) == (expected >> 8).
 *
 * Returns true  when magic, product_id, and hw_revision major all match.
 * Returns false when the magic is wrong (no header) or any field mismatches.
 */
bool app_validate_header(uint32_t app_base,
                         uint32_t expected_product_id,
                         uint16_t expected_hw_rev);

#ifdef __cplusplus
}
#endif

#endif /* APP_VALIDATE_H */
