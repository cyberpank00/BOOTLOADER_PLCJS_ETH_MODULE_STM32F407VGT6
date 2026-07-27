# BOOTLOADER_PLCJS_ETH_MODULE_12DI_D4MG_STM32F407VGT6

Ethernet bootloader for the PLCJS 12-DI module based on `STM32F407VGT6`.

The bootloader exposes a Modbus TCP OTA update protocol, stores the new image in
an internal staging area, verifies CRC32, installs the image into the
application region, and then boots the main firmware.

## Current status

This bootloader is integrated and tested with the paired application project:

- bootloader flash base: `0x08000000`
- application flash base: `0x08040000`
- metadata sector: `0x08020000`
- staging area: `0x08080000 .. 0x080BFFFF` (256 KB contiguous window)
- settings sector used by the application: `0x080C0000`

Validated scenarios:

- warm reset cycle `app -> bootloader -> app`
- repeated handoff cycles without power cycling
- full OTA update via Modbus TCP
- invalid CRC rejection
- interrupted OTA session recovery
- long bootloader ping stability test

## Main features

- bare-metal `LwIP` (`NO_SYS = 1`) with Modbus TCP server on port `502`
- single-client OTA update flow
- image staging, CRC32 verification, and installation into app flash
- persistent metadata in a dedicated flash sector
- software request from the application to stay in bootloader after reset
- recovery path for interrupted installation and interrupted upload sessions

## Flash layout

Defined in `Application/flash/flash_map.h`.

| Region | Address | Size | Purpose |
|---|---:|---:|---|
| Bootloader | `0x08000000` | 128 KB | Sectors 0-4 |
| Metadata | `0x08020000` | 128 KB | Sector 5, OTA/update state |
| Application | `0x08040000` | 256 KB | Sectors 6-7 |
| Staging | `0x08080000` | 256 KB | Sectors 8-9 |
| App settings | `0x080C0000` | 128 KB | Sector 10, owned by main app |

The application image must contain a `fw_header_t` struct at offset `0x200` from
`APP_FLASH_BASE`. The bootloader reads `product_id` and `hw_revision` from the
binary itself (not only from what the updater tool claims) at three points:
`FINALIZE_UPDATE`, `INSTALL_UPDATE` verification, and `BOOT_VERIFY_APP`.
See [Firmware header](#firmware-header) below.

Identity constants (defaults for the 12-DI/D4MG variant, CMake-configurable):

| Constant | Default | CMake flag |
|---|---|---|
| `PRODUCT_ID_DEFAULT` | `0x12D1D4A0` | `-DPRODUCT_ID=0x...` |
| `HW_REVISION_DEFAULT` | `1` | `-DHW_REVISION=N` |
| `BOOTLOADER_VERSION` | `1.0.0` | — |
| `FW_MAX_BLOCK_SIZE` | `240` bytes | — |

## Network configuration

The current bootloader network settings are hardcoded in `LWIP/App/lwip.c`:

- IP: `192.168.142.99`
- netmask: `255.255.255.0`
- gateway: `192.168.142.1`
- Modbus TCP port: `502`
- Modbus unit id: `1`

Current limitations:

- static IP only
- no DHCP in bootloader
- no runtime IP change via Modbus TCP
- single TCP client only

## How bootloader entry works

Bootloader stay/boot decision is implemented in `Application/boot/boot_entry.c`.

The bootloader stays active when one of the following is true:

1. the application requested bootloader mode by writing `BOOT_REQUEST_MAGIC`
   into the reserved no-init RAM cell at `0x2001FFF0`
2. installation was interrupted
3. `install_requested` is set in metadata
4. firmware reception was in progress
5. application is not valid
6. application vectors fail quick validation

The software boot request is one-shot: the bootloader consumes the RAM flag and
clears it on entry.

## Bootloader state machine

Implemented in `Application/boot/boot_main.c`.

Main states:

- `BOOT_START`
- `BOOT_CHECK_ENTRY`
- `BOOT_WAIT_COMMAND`
- `BOOT_RECEIVE_FW`
- `BOOT_VERIFY_STAGING`
- `BOOT_INSTALL_FW`
- `BOOT_VERIFY_APP`
- `BOOT_READY_TO_BOOT`
- `BOOT_ERROR`

Normal OTA flow:

1. enter `BOOT_WAIT_COMMAND`
2. receive `BEGIN_UPDATE`
3. erase staging and switch to `BOOT_RECEIVE_FW`
4. receive all firmware blocks
5. `FINALIZE_UPDATE` verifies staging CRC32 **and** reads `fw_header_t` from the staging binary
6. `INSTALL_UPDATE` copies firmware to app region and re-validates the header
7. app image is validated (CRC32 + firmware header)
8. bootloader jumps to application

## Modbus TCP update protocol

The OTA register handlers are implemented in `Application/modbus/fw_update_proto.c`.

Commands written to holding register `HR[0x0000]`:

- `1` - `BEGIN_UPDATE`
- `2` - `FINALIZE_UPDATE`
- `3` - `INSTALL_UPDATE`
- `4` - `ABORT_UPDATE`
- `5` - `REBOOT`

Frequently used input registers:

- `IR[0x0000..0x0001]` - magic `0xB00710AD`
- `IR[0x0004]` - boot state
- `IR[0x0005]` - app valid
- `IR[0x000B]` - last error
- `IR[0x000C..0x000D]` - total block count
- `IR[0x000E..0x000F]` - received block count
- `IR[0x0010..0x0011]` - image size
- `IR[0x0012..0x0013]` - image CRC32
- `IR[0x0014]` - command status
- `IR[0x0015]` - staging valid

Command status values:

- `0` - `IDLE`
- `1` - `BUSY`
- `2` - `OK`
- `3` - `ERROR`

Known error codes:

- `1` - `PRODUCT_MISMATCH`
- `2` - `HW_REV_MISMATCH`
- `3` - `IMAGE_TOO_LARGE`
- `4` - `BLOCK_CRC`
- `5` - `IMAGE_CRC`
- `6` - `FLASH_ERASE`
- `7` - `FLASH_WRITE`
- `8` - `APP_VALIDATE`
- `9` - `UPDATE_TIMEOUT`
- `10` - `BLOCK_INDEX`
- `11` - `BAD_PARAMS`

## OTA client

The reference CLI client is `tools/fw_update.mjs`. The legacy Python version is
still available as `tools/fw_update.py`.

Basic usage:

```bash
node tools/fw_update.mjs status
node tools/fw_update.mjs update path/to/app.bin
node tools/fw_update.mjs abort
node tools/fw_update.mjs reboot
node tools/fw_update.mjs app-bootloader
```

Default client parameters:

- bootloader IP: `192.168.142.99`
- application IP: `192.168.142.98`
- port: `502`
- unit id: `1`

Useful options:

- `--boot-ip` / `--ip` - override bootloader target
- `--app-ip` - override application target

## Entering bootloader from the main application

The paired application enters bootloader mode by writing `0xB007` to holding
register `118`, then performing a warm reset while preserving a shared RAM flag.

Example with `pymodbus`:

```python
from pymodbus.client import ModbusTcpClient

client = ModbusTcpClient("192.168.142.98", port=502, timeout=5)
client.connect()
client.write_register(address=118, value=0xB007, device_id=1)
client.close()
```

In the validated setup:

- main application responds on `192.168.142.98`
- bootloader responds on `192.168.142.99`

## Recovery notes

These operational details were confirmed during testing:

- after a normal `BOOT_WAIT_COMMAND` session, `REBOOT` transfers control back to
  the main application
- after an interrupted or failed OTA session, use `ABORT_UPDATE` first, then
  `REBOOT`, before expecting the device to return to the application
- a failed `FINALIZE_UPDATE` due to CRC mismatch keeps `staging_valid = 0`
- an interrupted upload leaves the bootloader in a safe state and prevents boot
  into an incomplete application image

## Ethernet reset fix

The warm-reset path depends on correct Ethernet peripheral reinitialization.

The current bootloader implementation enables ETH clocks before forcing MAC
reset in `LWIP/Target/ethernetif.c`.
This avoids stale MAC/DMA state after `bootloader -> app` and `app -> bootloader`
transitions.

## Build

Project build system: `CMake + Ninja`.

Default build (12-DI/D4MG, `PRODUCT_ID=0x12D1D4A0`, `HW_REVISION=1`):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
```

Outputs:

- `BOOTLOADER_PLCJS.elf`
- `BOOTLOADER_PLCJS.hex`
- `BOOTLOADER_PLCJS.bin`

## Building for module variants

`PRODUCT_ID_DEFAULT` and `HW_REVISION_DEFAULT` are guarded with `#ifndef` in
`Application/flash/flash_map.h` so they can be overridden at build time.

Pass `-DPRODUCT_ID` and `-DHW_REVISION` to CMake to target a different module:

```bash
# 12 Digital Inputs / 4 Digital Outputs (default)
cmake -S . -B build/12di -DPRODUCT_ID=0x12D1D4A0 -DHW_REVISION=1

# 12 Digital Outputs
cmake -S . -B build/12do -DPRODUCT_ID=0x12D00000 -DHW_REVISION=1

# 4 RTD inputs
cmake -S . -B build/4rtd -DPRODUCT_ID=0x04D00000 -DHW_REVISION=1

# 8 Analog Inputs
cmake -S . -B build/8ai -DPRODUCT_ID=0x08A10000 -DHW_REVISION=1

# 8 Analog Outputs
cmake -S . -B build/8ao -DPRODUCT_ID=0x08A00000 -DHW_REVISION=1
```

Each resulting `BOOTLOADER_PLCJS.bin` will only accept OTA images whose
`fw_header_t.product_id` and `fw_header_t.hw_revision` match the compiled-in
values.  Flashing firmware for the wrong module variant is rejected at
`FINALIZE_UPDATE` before anything is written to the application region.

## Firmware header

Starting with this release, valid application images must embed a `fw_header_t`
struct at `APP_FLASH_BASE + 0x200` (i.e. at byte offset `0x200` in `app.bin`).

The header struct is defined in `Application/validate/app_validate.h`:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;               /* 0x504C434A "PLCJ"             */
    uint32_t product_id;          /* must match PRODUCT_ID_DEFAULT  */
    uint16_t hw_revision;         /* must match HW_REVISION_DEFAULT */
    uint16_t reserved0;
    uint32_t fw_version;          /* informational                  */
    uint32_t image_size;          /* total binary size in bytes     */
    uint32_t image_crc32;         /* CRC32 with this field = 0      */
    uint32_t vector_table_offset; /* always 0                       */
    uint32_t reserved1[2];
} fw_header_t;
```

The paired application project places this header automatically via a
dedicated linker section (`.fw_header`) and the `tools/gen_app_bin.py`
post-build script that patches `image_size` and `image_crc32`.

**Validation chain** (three independent checks):

| Point | Where | What is checked |
|---|---|---|
| `FINALIZE_UPDATE` | `fw_update_proto.c` | header in **staging** flash |
| `INST_VERIFYING` | `fw_installer.c` | header in **app** flash after copy |
| `BOOT_VERIFY_APP` | `boot_main.c` | header in **app** flash before jump |

If any check fails, the error code `PRODUCT_MISMATCH` (1) is returned and
the bootloader enters `BOOT_ERROR` state.

## Known limitations

- bootloader IP is fixed and hardcoded
- no watchdog is enabled in the bootloader
- no authentication or access control is implemented for OTA commands
- only one Modbus TCP client can be connected at a time
- staging area is limited to `256 KB`

## Related repositories

- bootloader repo: this repository
- paired main application repo: `PLCJS_ETH_MODULE_12DI_D4MG_STM32F407VGT6`
