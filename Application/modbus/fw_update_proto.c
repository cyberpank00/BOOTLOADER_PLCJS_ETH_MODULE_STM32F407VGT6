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
#include "net_id.h"
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
static uint32_t    s_activity;          /* counts client write requests */
static uint32_t    s_activity_seen;
static uint32_t    s_activity_tick;
static bool        s_act_init;

void fw_proto_init(metadata_t *meta)
{
    s_meta       = meta;
    s_act_init   = false;
    s_cmd_status = meta->install_in_progress ? CMD_STATUS_BUSY :
                   meta->last_error != BOOT_ERR_NONE ? CMD_STATUS_ERROR : CMD_STATUS_IDLE;
    s_reboot_req = false;
    s_install_req = false;
    memset(s_params, 0, sizeof(s_params));
    memset(s_block_buf, 0, sizeof(s_block_buf));
}

/* ---- Command execution -------------------------------------------------- */
static bool commit_metadata(const metadata_t *next)
{
    if (!metadata_save(next)) {
        s_meta->last_error = BOOT_ERR_FLASH_WRITE;
        s_cmd_status = CMD_STATUS_ERROR;
        return false;
    }
    *s_meta = *next;
    return true;
}

bool fw_proto_busy(void)
{
    return s_meta == NULL || s_install_req || s_reboot_req ||
           s_meta->boot_state == BOOT_PREPARE_UPDATE ||
           s_meta->boot_state == BOOT_INSTALL_FW ||
           s_meta->boot_state == BOOT_VERIFY_APP ||
           s_meta->boot_state == BOOT_READY_TO_BOOT;
}

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
    if (image_size < FW_HEADER_OFFSET + sizeof(fw_header_t) || image_size > STAGING_FLASH_SIZE) {
        s_meta->last_error = BOOT_ERR_IMAGE_TOO_LARGE;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    if (block_size == 0u || block_size > FW_MAX_BLOCK_SIZE || (block_size & 3u) != 0u) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    if (block_count != (image_size + block_size - 1u) / block_size ||
        block_count > META_MAX_BLOCKS) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    metadata_t next = *s_meta;
    next.image_size          = image_size;
    next.image_crc32         = image_crc;
    next.fw_version          = fw_ver;
    next.product_id          = product_id;
    next.hw_revision         = hw_rev;
    next.block_size          = block_size;
    next.block_count         = block_count;
    next.received_block_count = 0u;
    next.staging_valid        = 0u;
    next.install_requested    = 0u;
    next.install_in_progress  = 0u;
    next.boot_state           = (uint32_t)BOOT_PREPARE_UPDATE;
    next.last_error           = (uint32_t)BOOT_ERR_NONE;
    memset(next.block_bitmap, 0, sizeof(next.block_bitmap));
    if (!commit_metadata(&next)) return;

    /* Erase staging area */
    uint32_t nsectors = STAGING_LAST_SECTOR - STAGING_FIRST_SECTOR + 1u;
    if (!flash_if_erase_sectors(STAGING_FIRST_SECTOR, nsectors)) {
        s_meta->boot_state = BOOT_ERROR;
        s_meta->last_error = BOOT_ERR_FLASH_ERASE;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    s_meta->boot_state = BOOT_RECEIVE_FW;
    s_cmd_status = CMD_STATUS_OK;
}

static void exec_write_block(uint16_t reg_qty)
{
    if (s_meta->boot_state != (uint32_t)BOOT_RECEIVE_FW || !metadata_params_valid(s_meta)) {
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
    uint32_t remaining = s_meta->image_size - offset;
    uint32_t expected = remaining < s_meta->block_size ? remaining : s_meta->block_size;
    if (data_len != expected || reg_qty != need_regs) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    /* Extract firmware bytes from Modbus register buffer.
     * Registers are stored as big-endian uint16_t by nanoMODBUS;
     * on little-endian STM32 a raw cast would swap bytes in each pair.
     * regs_to_bytes() restores the original byte order sent by the master. */
    uint8_t data_bytes[FW_MAX_BLOCK_SIZE];
    regs_to_bytes(&s_block_buf[3], data_bytes, data_len);

    if (metadata_bitmap_get(s_meta, block_idx)) {
        bool same = memcmp((const void *)(STAGING_FLASH_BASE + offset), data_bytes, data_len) == 0;
        s_meta->last_error = same ? BOOT_ERR_NONE : BOOT_ERR_BAD_PARAMS;
        s_cmd_status = same ? CMD_STATUS_OK : CMD_STATUS_ERROR;
        return;
    }
    if (!flash_if_write(STAGING_FLASH_BASE + offset, data_bytes, data_len)) {
        s_meta->last_error = BOOT_ERR_FLASH_WRITE;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    if (memcmp((const void *)(STAGING_FLASH_BASE + offset), data_bytes, data_len) != 0) {
        s_meta->last_error = BOOT_ERR_BLOCK_CRC;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    metadata_bitmap_set(s_meta, block_idx);
    s_meta->received_block_count++;
    s_meta->last_error = BOOT_ERR_NONE;
    s_cmd_status = CMD_STATUS_OK;
}

static void exec_finalize(void)
{
    if ((s_meta->boot_state != BOOT_RECEIVE_FW && s_meta->boot_state != BOOT_VERIFY_STAGING) ||
        !metadata_all_blocks_received(s_meta)) {
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
                             HW_REVISION_DEFAULT_U16)) {
        s_meta->last_error = BOOT_ERR_PRODUCT_MISMATCH;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    if (!app_validate_vectors_at(STAGING_FLASH_BASE, s_meta->image_size)) {
        s_meta->last_error = BOOT_ERR_APP_VALIDATE;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }

    metadata_t next = *s_meta;
    next.staging_valid       = 1u;
    next.install_requested   = 0u;
    next.install_in_progress = 0u;
    next.boot_state          = (uint32_t)BOOT_VERIFY_STAGING;
    next.last_error          = (uint32_t)BOOT_ERR_NONE;
    if (!commit_metadata(&next)) return;

    s_cmd_status = CMD_STATUS_OK;
}

static void exec_install(void)
{
    if (!s_meta->staging_valid ||
        (s_meta->boot_state != BOOT_VERIFY_STAGING && s_meta->boot_state != BOOT_ERROR)) {
        s_meta->last_error = BOOT_ERR_BAD_PARAMS;
        s_cmd_status = CMD_STATUS_ERROR;
        return;
    }
    s_install_req = true;
    s_cmd_status  = CMD_STATUS_BUSY;
}

static bool abort_session(uint32_t err)
{
    metadata_t next = *s_meta;
    next.boot_state           = (uint32_t)BOOT_WAIT_COMMAND;
    next.install_requested    = 0u;
    next.install_in_progress  = 0u;
    next.staging_valid        = 0u;
    next.received_block_count = 0u;
    next.block_count          = 0u;
    next.image_size           = 0u;
    next.image_crc32          = 0u;
    next.last_error           = err;
    memset(next.block_bitmap, 0, sizeof(next.block_bitmap));
    return commit_metadata(&next);
}

static void exec_abort(void)
{
    if (!abort_session((uint32_t)BOOT_ERR_NONE)) return;

    s_cmd_status = CMD_STATUS_OK;
}

/* ---- Protocol poll ------------------------------------------------------ */
void fw_proto_poll(metadata_t *meta)
{
    s_meta = meta;
    /* No periodic work — commands are processed in write callback. */
}

bool fw_proto_session_timeout(uint32_t now_ms)
{
    const bool active = s_meta != NULL &&
        (s_meta->boot_state == (uint32_t)BOOT_RECEIVE_FW ||
         s_meta->boot_state == (uint32_t)BOOT_VERIFY_STAGING ||
         s_meta->staging_valid);

    /* Any client write restarts the window; no session means nothing to expire. */
    if (!s_act_init || s_activity != s_activity_seen || !active) {
        s_act_init      = true;
        s_activity_seen = s_activity;
        s_activity_tick = now_ms;
        return false;
    }
    if ((uint32_t)(now_ms - s_activity_tick) < FW_SESSION_TIMEOUT_MS) return false;

    s_activity_tick = now_ms;
    if (!abort_session((uint32_t)BOOT_ERR_UPDATE_TIMEOUT)) return false;
    return s_meta->app_valid &&
           app_validate_full(APP_FLASH_BASE, s_meta->app_image_size, s_meta->app_image_crc32);
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
    uint8_t mac[6];
    net_id_get_mac(mac);
    for (uint32_t i = 0; i < 3u; i++)
        ir[0x19u + i] = ((uint16_t)mac[2u * i] << 8u) | mac[2u * i + 1u];

    if (qty == 0u || addr + qty > 0x20u) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    memcpy(out, &ir[addr], qty * sizeof(uint16_t));
    return NMBS_ERROR_NONE;
}

nmbs_error fw_proto_read_holding_regs(uint16_t addr, uint16_t qty,
                                      uint16_t *out)
{
    if (qty != 0u && addr < 0x20u && addr + qty <= 0x20u) {
        memcpy(out, &s_params[addr], qty * sizeof(uint16_t));
        return NMBS_ERROR_NONE;
    }
    return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
}

nmbs_error fw_proto_write_holding_regs(uint16_t addr, uint16_t qty,
                                       const uint16_t *in)
{
    s_activity++;
    if (fw_proto_busy()) return NMBS_EXCEPTION_SERVER_DEVICE_BUSY;
    if (qty == 0u) return NMBS_EXCEPTION_ILLEGAL_DATA_VALUE;
    /* Block data region (0x0100+) */
    if (addr >= 0x0100u) {
        uint16_t off = addr - 0x0100u;
        if (off != 0u || qty < 4u || qty > 123u) {
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
