/**
 * @file  fw_update_proto.h
 * @brief Firmware update protocol over Modbus TCP register interface.
 *
 * === Input Registers (FC 04, read-only, base 0x0000) ===
 *
 *  Addr   Regs  Description
 *  0x0000  2    Bootloader magic  (0xB00710AD)
 *  0x0002  2    Bootloader version
 *  0x0004  1    Boot state        (boot_state_t)
 *  0x0005  1    App valid         (0/1)
 *  0x0006  2    App FW version
 *  0x0008  2    Product ID
 *  0x000A  2    HW revision (major<<16 | minor<<8 | patch)
 *  0x000C  1    Last error code   (boot_error_t)
 *  0x000D  2    Total block count
 *  0x000F  2    Received blocks
 *  0x0011  2    Image size
 *  0x0013  2    Image CRC32
 *  0x0015  1    Command status    (0=idle,1=busy,2=ok,3=err)
 *  0x0016  1    Staging valid     (0/1)
 *  0x0017  2    Installed application's product ID (0 if invalid)
 *  0x0019  3    MAC bytes, high byte first (BL 1.8+)
 *               02:00:12:34:56:78 => 0x0200,0x1234,0x5678
 *
 * === Holding Registers (FC 03/16, base 0x0000) ===
 *
 *  Addr   Regs  Description
 *  0x0000  1    Command register
 *  0x0010  2    Param: image_size
 *  0x0012  2    Param: image_crc32
 *  0x0014  2    Param: fw_version
 *  0x0016  2    Param: product_id
 *  0x0018  1    Param: hw_revision (major<<8 | minor)
 *  0x0019  1    Param: block_size
 *  0x001A  2    Param: block_count
 *
 *  0x0100  2    Write-block: block index
 *  0x0102  1    Write-block: data length (bytes)
 *  0x0103 ..    Write-block: data (up to 120 regs = 240 bytes)
 *
 * === Commands (written to HR 0x0000) ===
 *
 *  1  CMD_BEGIN_UPDATE    — prepare for new image (uses params 0x0010-0x001B)
 *  2  CMD_FINALIZE_UPDATE — verify staging completeness + CRC
 *  3  CMD_INSTALL_UPDATE  — copy staging → application
 *  4  CMD_ABORT_UPDATE    — cancel current session
 *  5  CMD_REBOOT          — software reset
 *
 * All multi-register integers are high word first. FW/BL version = major<<8|minor.
 * FINALIZE verifies bitmap, exact image CRC, identity and application vectors;
 * it does not request installation. Reset afterwards waits for INSTALL.
 * INSTALL persists the recovery state before erasing the application. An
 * interrupted install restarts from verified staging, beginning with erase.
 * Mutating writes during installation/final verification return Modbus exception
 * 6 (device busy), without overwriting the installation's command status.
 *
 * block_size: 4..240, multiple of 4; block_count = ceil(image_size/block_size).
 * Each block is ONE FC16 at 0x0100 with 3+ceil(data_len/2) registers. Non-final
 * blocks have block_size bytes; the final block has exactly the remainder.
 * Identical duplicates succeed without reprogramming Flash/counting twice;
 * changed duplicates fail. Read-back must match before the bitmap bit is set.
 * The image size includes the 28-byte header at offset 0x200 (minimum 540).
 *
 * One TCP client: 30 s idle timeout, 5 s incomplete-frame timeout. Reconnect
 * does not cancel an update; MCU reset during reception requires a new BEGIN.
 * Clients select a MAC through PDP and read 0x0019..0x001B on the SAME TCP
 * connection before changing commands; repeat this after each reconnect.
 * Legacy status reads (25 registers) remain valid. Protected OTA requires
 * BL 1.8+ and fails closed if MAC registers are unavailable.
 */
#ifndef FW_UPDATE_PROTO_H
#define FW_UPDATE_PROTO_H

#include <stdint.h>
#include "metadata.h"
#include "nanomodbus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_MODBUS_MAGIC       0xB00710ADu

#define CMD_NONE                0u
#define CMD_BEGIN_UPDATE        1u
#define CMD_FINALIZE_UPDATE     2u
#define CMD_INSTALL_UPDATE      3u
#define CMD_ABORT_UPDATE        4u
#define CMD_REBOOT              5u

#define CMD_STATUS_IDLE         0u
#define CMD_STATUS_BUSY         1u
#define CMD_STATUS_OK           2u
#define CMD_STATUS_ERROR        3u

void     fw_proto_init(metadata_t *meta);
void     fw_proto_poll(metadata_t *meta);
bool     fw_proto_reboot_requested(void);
bool     fw_proto_install_requested(void);
bool     fw_proto_busy(void);
void     fw_proto_install_done(void);
void     fw_proto_install_error(void);

nmbs_error fw_proto_read_input_regs(uint16_t addr, uint16_t qty,
                                    uint16_t *out);
nmbs_error fw_proto_read_holding_regs(uint16_t addr, uint16_t qty,
                                      uint16_t *out);
nmbs_error fw_proto_write_holding_regs(uint16_t addr, uint16_t qty,
                                       const uint16_t *in);

#ifdef __cplusplus
}
#endif

#endif /* FW_UPDATE_PROTO_H */
