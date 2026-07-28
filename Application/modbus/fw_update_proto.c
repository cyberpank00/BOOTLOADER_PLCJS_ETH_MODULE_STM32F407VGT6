/**
 * @file  fw_update_proto.c
 * @brief Firmware update protocol — Modbus register handlers.
 */

#include "fw_update_proto.h"
#include "flash_map.h"
#include "flash_if.h"
#include "crc32.h"
#include "app_validate.h"
#include "boot_state.h"
#include <string.h>

/* ---- Private helpers ---------------------------------------------------- */
static inline uint32_t regs_to_u32(const uint16_t *r)
{
    return ((uint32_t)r[0] << 16) | (uint32_t)r[1];
}

static inline void u32_to_regs(uint32_t v, uint16_t *r)
{
    r[0] = (uint16_t)(v >> 16);
    r[1] = (uint16_t)(v & 0xFFFFu);
}

/* Extract firmware bytes from Modbus uint16_t register array.
 *
 * Modbus TCP sends register values big-endian over the wire (high byte first).
 * nanoMODBUS stores them as native uint16_t (value = (hi << 8) | lo).
 * On little-endian STM32 the in-memory layout is [lo, hi] per register.
 * A direct (uint8_t *) cast would therefore deliver bytes in swapped pairs.
 *
 * This helper reverses the process: reg[i] high-byte → dest[2i],
 *                                   reg[i] low-byte  → dest[2i+1].
 * This matches the byte order seen by the Modbus master.
 */
static void regs_to_bytes(const uint16_t *regs, uint8_t *dest, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint16_t reg = regs[i / 2u];
        dest[i] = (uint8_t)((i & 1u) ? (reg & 0xFFu) : (reg >> 8u));
    }
}

/* ---- Module state ------------------------------------------------------- */
static metadata_t *s_meta;
static uint16_t    s_cmd_status;
static uint16_t    s_params[32];        /* HR 0x0000 – 0x001F */
static uint16_t    s_block_buf[128];    /* HR 0x0100 – 0x017F */
static bool        s_reboot_req;
static bool        s_install_req;

void fw_proto_init(metadata_t *meta)
{
    s_meta       = meta;
    s_cmd_status = CMD_STATUS_IDLE;
    s_reboot_req = false;
    s_install_req = false;
    memset(s_params, 0, sizeof(s_params));
    memset(s_block_buf, 0, sizeof(s_block_buf));
}

/* ---- Command execution -------------------------------------------------- */
static void exec_begin_update(void)
{
    uint32_t image_size  = regs_to_u32(&s_params[0x10]);
    uint32_t image_crc   = regs_to_u32(&s_params[0x12]);
    uint32_t fw_ver      = regs_to_u32(&s_params[0x14]);
    uint32_t product_id  = regs_to_u32(&s_params[0x16]);
    /* hw_rev is sent as (major << 8) | minor (uint16); patch is cosmetic — not sent. */
    uint16_t hw_rev      = s_params[0x18];
    uint16_t block_size  = s_params[0x19];
    uint32_t block_count = regs_to_u32(&s_params[0x1A]);

    if (product_id != PRODUCT_ID_DEFAULT) {
        s_meta->last_error = BOOT_ERR_PRODUCT_MISMATCH;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    /* Accept any minor/patch variant of the same major revision.
     * hw_rev top byte = major sent; HW_REVISION_DEFAULT >> 16 = major expected. */
    if ((hw_rev >> 8u) != (HW_REVISION_DEFAULT >> 16u)) {
        s_meta->last_error = BOOT_ERR_HW_REV_MISMATCH;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    if (image_size == 0u || image_size > STAGING_FLASH_SIZE) {
        s_meta->last_error = BOOT_ERR_IMAGE_TOO_LARGE;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    if (block_size == 0u || block_size > FW_MAX_BLOCK_SIZE) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    if (block_count == 0u || block_count > META_MAX_BLOCKS) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    /* Erase staging area */
    uint32_t nsectors = STAGING_LAST_SECTOR - STAGING_FIRST_SECTOR + 1u;
    if (!flash_if_erase_sectors(STAGING_FIRST_SECTOR, nsectors)) {
        s_meta->last_error = BOOT_ERR_FLASH_ERASE;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    s_meta->image_size          = image_size;
    s_meta->image_crc32         = image_crc;
    s_meta->fw_version          = fw_ver;
    s_meta->product_id          = product_id;
    s_meta->hw_revision         = hw_rev;
    s_meta->block_size          = block_size;
    s_meta->block_count         = block_count;
    s_meta->received_block_count = 0u;
    s_meta->staging_valid        = 0u;
    s_meta->install_requested    = 0u;
    s_meta->boot_state           = (uint32_t)BOOT_RECEIVE_FW;
    s_meta->last_error           = (uint32_t)BOOT_ERR_NONE;
    memset(s_meta->block_bitmap, 0, sizeof(s_meta->block_bitmap));
    metadata_save(s_meta);

    s_cmd_status = CMD_STATUS_OK;
}

static void exec_write_block(uint16_t reg_qty)
{
    if (s_meta->boot_state != (uint32_t)BOOT_RECEIVE_FW) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    uint32_t block_idx = regs_to_u32(&s_block_buf[0]);
    uint16_t data_len  = s_block_buf[2];

    if (block_idx >= s_meta->block_count) {
        s_meta->last_error = BOOT_ERR_BLOCK_INDEX;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    if (data_len == 0u || data_len > FW_MAX_BLOCK_SIZE) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    /* Ensure the master actually delivered enough data registers for the
     * declared data_len in this same FC16 transaction: 3 header registers
     * (block_idx hi/lo + data_len) plus ceil(data_len/2) data registers.
     * Without this check a short frame would cause stale buffer contents to
     * be flashed (later caught by the finalize CRC, but silently corrupting
     * the block until then). */
    uint16_t need_regs = 3u + (uint16_t)((data_len + 1u) / 2u);
    if (reg_qty < need_regs) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    uint32_t offset = block_idx * (uint32_t)s_meta->block_size;
    if (offset + data_len > STAGING_FLASH_SIZE) {
        s_meta->last_error = BOOT_ERR_IMAGE_TOO_LARGE;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    /* Extract firmware bytes from Modbus register buffer.
     * Registers are stored as big-endian uint16_t by nanoMODBUS;
     * on little-endian STM32 a raw cast would swap bytes in each pair.
     * regs_to_bytes() restores the original byte order sent by the master. */
    uint8_t data_bytes[FW_MAX_BLOCK_SIZE];
    regs_to_bytes(&s_block_buf[3], data_bytes, data_len);

    if (!flash_if_write(STAGING_FLASH_BASE + offset, data_bytes, data_len)) {
        s_meta->last_error = BOOT_ERR_FLASH_WRITE;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    if (!metadata_bitmap_get(s_meta, block_idx)) {
        metadata_bitmap_set(s_meta, block_idx);
        s_meta->received_block_count++;
    }

    s_cmd_status = CMD_STATUS_OK;
}

static void exec_finalize(void)
{
    if (!metadata_all_blocks_received(s_meta)) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    uint32_t crc = crc32_calc((const uint8_t *)STAGING_FLASH_BASE,
                               s_meta->image_size);
    if (crc != s_meta->image_crc32) {
        s_meta->last_error = BOOT_ERR_IMAGE_CRC;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    /* Validate firmware header embedded in the staging binary.
     * This ensures the binary itself carries the correct product identity,
     * independent of what the updater tool claimed in BEGIN_UPDATE. */
    if (!app_validate_header(STAGING_FLASH_BASE, PRODUCT_ID_DEFAULT,
                             (uint16_t)HW_REVISION_DEFAULT)) {
        s_meta->last_error = BOOT_ERR_PRODUCT_MISMATCH;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    s_meta->staging_valid     = 1u;
    s_meta->install_requested = 1u;
    s_meta->boot_state        = (uint32_t)BOOT_VERIFY_STAGING;
    s_meta->last_error        = (uint32_t)BOOT_ERR_NONE;
    metadata_save(s_meta);

    s_cmd_status = CMD_STATUS_OK;
}

static void exec_install(void)
{
    if (!s_meta->staging_valid || !s_meta->install_requested) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    s_install_req = true;
    s_cmd_status  = CMD_STATUS_BUSY;
}

static void exec_abort(void)
{
    s_meta->boot_state           = (uint32_t)BOOT_WAIT_COMMAND;
    s_meta->install_requested    = 0u;
    s_meta->staging_valid        = 0u;
    s_meta->received_block_count = 0u;
    s_meta->last_error           = (uint32_t)BOOT_ERR_NONE;
    memset(s_meta->block_bitmap, 0, sizeof(s_meta->block_bitmap));
    metadata_save(s_meta);

    s_cmd_status = CMD_STATUS_OK;
}

/* ---- Protocol poll ------------------------------------------------------ */
void fw_proto_poll(metadata_t *meta)
{
    s_meta = meta;
    /* No periodic work — commands are processed in write callback. */
}

bool fw_proto_reboot_requested(void)  { return s_reboot_req; }
bool fw_proto_install_requested(void) { return s_install_req; }

void fw_proto_install_done(void)
{
    s_install_req = false;
    s_cmd_status  = CMD_STATUS_OK;
}

void fw_proto_install_error(void)
{
    s_install_req = false;
    s_cmd_status  = CMD_STATUS_ERROR;
}

/* ---- Modbus register callbacks ------------------------------------------ */
nmbs_error fw_proto_read_input_regs(uint16_t addr, uint16_t qty,
                                    uint16_t *out)
{
    uint16_t ir[0x20];
    memset(ir, 0, sizeof(ir));

    u32_to_regs(BOOT_MODBUS_MAGIC,         &ir[0x00]);
    u32_to_regs(BOOTLOADER_VERSION,         &ir[0x02]);
    ir[0x04] = (uint16_t)s_meta->boot_state;
    ir[0x05] = s_meta->app_valid;
    /* Read app version directly from the installed fw_header_t (ground truth).
     * Fall back to metadata if the header is unreadable; report 0 when no app. */
    {
        uint32_t app_ver = 0u;
        if (s_meta->app_valid) {
            fw_header_t hdr;
            app_ver = app_read_header(APP_FLASH_BASE, &hdr)
                      ? hdr.fw_version : s_meta->app_fw_version;
        }
        u32_to_regs(app_ver, &ir[0x06]);
    }
    /* Bootloader identity: always the compile-time constant, so the tool can
     * compare it against the product_id embedded in the image to be flashed. */
    u32_to_regs(PRODUCT_ID_DEFAULT,           &ir[0x08]);
    /* HW revision — always report compile-time constant (3 bytes: major.minor.patch).
     * Uses 2 registers (32-bit) so all three bytes are visible over Modbus.
     * Register layout from 0x0A onward shifted by 1 vs. the old 16-bit layout. */
    u32_to_regs(HW_REVISION_DEFAULT,          &ir[0x0A]); /* ir[0x0A-0B] */
    ir[0x0C] = (uint16_t)s_meta->last_error;              /* was 0x0B */
    u32_to_regs(s_meta->block_count,          &ir[0x0D]); /* was 0x0C */
    u32_to_regs(s_meta->received_block_count, &ir[0x0F]); /* was 0x0E */
    u32_to_regs(s_meta->image_size,           &ir[0x11]); /* was 0x10 */
    u32_to_regs(s_meta->image_crc32,          &ir[0x13]); /* was 0x12 */
    ir[0x15] = s_cmd_status;                              /* was 0x14 */
    ir[0x16] = s_meta->staging_valid;                     /* was 0x15 */
    /* Installed application product_id, read straight from its fw_header_t
     * (ground truth). 0 when no valid application is present. */
    {
        uint32_t app_pid = 0u;
        if (s_meta->app_valid) {
            fw_header_t hdr;
            if (app_read_header(APP_FLASH_BASE, &hdr)) {
                app_pid = hdr.product_id;
            }
        }
        u32_to_regs(app_pid, &ir[0x17]);                  /* ir[0x17-0x18] */
    }

    if (addr + qty > 0x20u) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    memcpy(out, &ir[addr], qty * sizeof(uint16_t));
    return NMBS_ERROR_NONE;
}

nmbs_error fw_proto_read_holding_regs(uint16_t addr, uint16_t qty,
                                      uint16_t *out)
{
    if (addr < 0x20u && addr + qty <= 0x20u) {
        memcpy(out, &s_params[addr], qty * sizeof(uint16_t));
        return NMBS_ERROR_NONE;
    }
    return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
}

nmbs_error fw_proto_write_holding_regs(uint16_t addr, uint16_t qty,
                                       const uint16_t *in)
{
    /* Block data region (0x0100+) */
    if (addr >= 0x0100u) {
        uint16_t off = addr - 0x0100u;
        if (off + qty > 128u) {
            return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
        }
        memcpy(&s_block_buf[off], in, qty * sizeof(uint16_t));

        /* Trigger WRITE_BLOCK when we write starting from 0x0100.
         * Master must send block_idx (2 regs) + data_len (1 reg) +
         * at least 1 data reg in the same FC16 transaction. */
        if (addr == 0x0100u && qty >= 4u) {
            exec_write_block(qty);
        }
        return NMBS_ERROR_NONE;
    }

    /* Parameter region (0x0000 – 0x001F) */
    if (addr + qty > 0x20u) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    memcpy(&s_params[addr], in, qty * sizeof(uint16_t));

    /* Command register written? */
    if (addr == 0x0000u) {
        uint16_t cmd = s_params[0];
        s_params[0]  = CMD_NONE;

        switch (cmd) {
        case CMD_BEGIN_UPDATE:   exec_begin_update();  break;
        case CMD_FINALIZE_UPDATE: exec_finalize();     break;
        case CMD_INSTALL_UPDATE:  exec_install();      break;
        case CMD_ABORT_UPDATE:    exec_abort();        break;
        case CMD_REBOOT:          s_reboot_req = true; break;
        default:
            return NMBS_EXCEPTION_ILLEGAL_DATA_VALUE;
        }
    }

    return NMBS_ERROR_NONE;
}
