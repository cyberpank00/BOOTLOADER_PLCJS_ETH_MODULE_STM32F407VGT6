/**
 * @file  metadata.h
 * @brief Persistent metadata stored in Flash sector 4 (64 KB).
 *
 * After any reset the bootloader reads this structure to understand:
 *   - whether a firmware update session was in progress
 *   - whether an installation was interrupted
 *   - whether the application image is valid
 */
#ifndef METADATA_H
#define METADATA_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define META_MAGIC              0x4D455441u  /* "META" */
#define META_STRUCT_VERSION     1u

/* Maximum number of blocks we can track with a bitmap.
 * 256 KB / 240 B  ~= 1093 blocks => 137 bytes of bitmap.
 * We round up to 256 bytes for alignment.
 */
#define META_BITMAP_BYTES       256u
#define META_MAX_BLOCKS         (META_BITMAP_BYTES * 8u)

typedef struct __attribute__((packed)) {
    uint32_t    magic;
    uint16_t    struct_version;
    uint16_t    reserved0;

    /* Boot / update state */
    uint32_t    boot_state;          /* boot_state_t    */
    uint32_t    last_error;          /* boot_error_t    */
    uint8_t     app_valid;
    uint8_t     install_requested;
    uint8_t     install_in_progress;
    uint8_t     staging_valid;

    /* Image parameters (from BEGIN_UPDATE) */
    uint32_t    image_size;
    uint32_t    image_crc32;
    uint32_t    fw_version;
    uint32_t    product_id;
    uint16_t    hw_revision;
    uint16_t    block_size;
    uint32_t    block_count;
    uint32_t    received_block_count;

    /* Installed application info */
    uint32_t    app_fw_version;
    uint32_t    app_image_size;
    uint32_t    app_image_crc32;

    uint16_t    attempt_counter;
    uint16_t    reserved1;
    uint8_t     reserved2[16];

    /* Received-block bitmap */
    uint8_t     block_bitmap[META_BITMAP_BYTES];

    /* CRC32 over everything above */
    uint32_t    crc32;
} metadata_t;

bool     metadata_load(metadata_t *out);
bool     metadata_save(const metadata_t *m);
void     metadata_reset(metadata_t *m);

void     metadata_bitmap_set(metadata_t *m, uint32_t block_index);
bool     metadata_bitmap_get(const metadata_t *m, uint32_t block_index);
bool     metadata_all_blocks_received(const metadata_t *m);
bool     metadata_params_valid(const metadata_t *m);

#ifdef __cplusplus
}
#endif

#endif /* METADATA_H */
