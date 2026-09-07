/**
 * @file  flash_map.h
 * @brief Internal Flash memory layout for bootloader + application.
 *
 * STM32F407VGT6 — 1 MB internal Flash, 12 sectors.
 *
 * Sector  Address       Size   Usage
 * ------  ----------    -----  ----------------------
 *  0      0x0800 0000   16 KB  Bootloader
 *  1      0x0800 4000   16 KB  Bootloader
 *  2      0x0800 8000   16 KB  Bootloader
 *  3      0x0800 C000   16 KB  Bootloader
 *  4      0x0801 0000   64 KB  Bootloader
 *  5      0x0802 0000  128 KB  Metadata (update state)
 *  6      0x0804 0000  128 KB  Application
 *  7      0x0806 0000  128 KB  Application
 *  8      0x0808 0000  128 KB  Staging
 *  9      0x080A 0000  128 KB  Staging
 * 10      0x080C 0000  128 KB  App Settings (NVM)
 * 11      0x080E 0000  128 KB  Staging (3rd sector)
 *
 * Application: 256 KB (sectors 6-7)
 * Staging:     384 KB (sectors 8-9, 11)
 *              NOTE: staging sectors are non-contiguous (8,9 + 11).
 *              For simplicity we use only sectors 8-9 (256 KB) as
 *              contiguous staging.  Max firmware image = 256 KB.
 */
#ifndef FLASH_MAP_H
#define FLASH_MAP_H

#include <stdint.h>

/* ---- Bootloader region (sectors 0-4, 128 KB) ---------------------------- */
#define BOOT_FLASH_BASE         0x08000000u
#define BOOT_FLASH_SIZE         (128u * 1024u)
#define BOOT_FLASH_END          (BOOT_FLASH_BASE + BOOT_FLASH_SIZE)

/* ---- Metadata region (sector 5, 128 KB) --------------------------------- */
#define META_FLASH_SECTOR       5u
#define META_FLASH_BASE         0x08020000u
#define META_FLASH_SIZE         (128u * 1024u)

/* ---- Application region (sectors 6-7, 256 KB) --------------------------- */
#define APP_FLASH_BASE          0x08040000u
#define APP_FLASH_SIZE          (256u * 1024u)
#define APP_FLASH_END           (APP_FLASH_BASE + APP_FLASH_SIZE)
#define APP_FIRST_SECTOR        6u
#define APP_LAST_SECTOR         7u

/* ---- Staging region (sectors 8-9, 256 KB) ------------------------------- */
#define STAGING_FLASH_BASE      0x08080000u
#define STAGING_FLASH_SIZE      (256u * 1024u)
#define STAGING_FLASH_END       (STAGING_FLASH_BASE + STAGING_FLASH_SIZE)
#define STAGING_FIRST_SECTOR    8u
#define STAGING_LAST_SECTOR     9u

/* ---- App Settings (sector 10, 128 KB — used by main app) ---------------- */
#define SETTINGS_FLASH_SECTOR   10u
#define SETTINGS_FLASH_BASE     0x080C0000u
#define SETTINGS_FLASH_SIZE     (128u * 1024u)

/* ---- RAM ---------------------------------------------------------------- */
#define RAM_BASE                0x20000000u
#define RAM_SIZE                (128u * 1024u)

/* ---- Firmware image header ---------------------------------------------- */
#define FW_IMAGE_MAGIC          0x504C434Au  /* "PLCJ" */

/* PRODUCT_ID_DEFAULT and HW_REVISION_DEFAULT can be overridden at build time
 * by passing -DPRODUCT_ID_DEFAULT=<val> and -DHW_REVISION_DEFAULT=<val> to
 * the compiler (e.g. via CMake: -DPRODUCT_ID=0x12D1D4A0 -DHW_REVISION=0x010101).
 *
 * HW_REVISION encoding: (major << 16) | (minor << 8) | patch
 *   major — full board rework / MCU pinout change  → FW major must match
 *   minor — non-pinout component / schematic change → FW-compatible
 *   patch — cosmetic change (tracks, holes, silkscreen, etc.)
 *   e.g. hw:01.02.03 → 0x010203
 *
 * OTA acceptance: product_id exact match + (hw_rev >> 16) == (HW_REVISION_DEFAULT >> 16)
 *   (major byte only; minor/patch variants are always firmware-compatible) */
#ifndef PRODUCT_ID_DEFAULT
#define PRODUCT_ID_DEFAULT      0x504C1201u  /* "PL" + 12 ch + DI(01) — 12di */
#endif
#ifndef HW_REVISION_DEFAULT
#define HW_REVISION_DEFAULT     0x010101u  /* hw:01.01.01 */
#endif
/* BL version encoding: (major << 8) | minor  — mirrors fw_version in fw_header_t */
#define BOOTLOADER_VERSION      0x00000104u  /* 1.4 */

/* Maximum firmware block size for Modbus transfer (bytes). */
#define FW_MAX_BLOCK_SIZE       240u

/* ---- Software bootloader-entry request ---------------------------------- *
 * The application sets this magic word in a no-init RAM cell (reserved at the
 * top of SRAM in the linker script) and then issues a system reset. The
 * bootloader checks the cell early in its entry decision and, if the magic is
 * present, stays in the bootloader (and clears the cell so it is one-shot).
 * The RAM cell survives a warm reset (NVIC_SystemReset) but not a power cycle.
 * The address must match the reservation in BOTH linker scripts. */
#define BOOT_REQUEST_FLAG_ADDR  0x2001FFF0u
#define BOOT_REQUEST_MAGIC      0xB007CAFEu

#endif /* FLASH_MAP_H */
