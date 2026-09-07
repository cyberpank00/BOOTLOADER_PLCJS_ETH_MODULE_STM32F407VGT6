# BOOTLOADER_PLCJS_ETH_MODULE_STM32F407VGT6

Universal bootloader for all PLCJS ETH module variants (12di, 12do, 4rtd,
4aic, 4aiv, 4ao, ...). The firmware is identical for every board; variants
differ only in the compiled-in `PRODUCT_ID` / `HW_REVISION`
(see [Building for module variants](#building-for-module-variants)).

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
| `PRODUCT_ID_DEFAULT` | `0x504C1201` | `-DPRODUCT_ID=0x...` |
| `HW_REVISION_DEFAULT` | `0x010101` | `-DHW_REVISION=0x...` |
| `BOOTLOADER_VERSION` | `1.4` | — |
| `FW_MAX_BLOCK_SIZE` | `240` bytes | — |

## Network configuration

The bootloader comes up on an **AutoIP link-local** address (RFC 3927),
derived deterministically from the chip UID in `LWIP/App/lwip.c` — the same
scheme the applications use:

- IP: `169.254.<mac[4]>.<mac[5]>` (host octets clamped to 1..254)
- netmask: `255.255.0.0`
- gateway: none
- Modbus TCP port: `502`, unit id: `1`

Because the address is not fixed, the bootloader is located **by MAC** over the
discovery protocol (see below). Its address can be reassigned live for a session
with a discovery `SET_NET` (not persisted — the bootloader has no settings
store). `tools/fw_update.mjs` does this automatically.

Current limitations:

- no DHCP in bootloader
- SET_NET changes are live-only (lost on reset)
- single TCP client only

## Discovery protocol (PDP) and factory addressing

Both the bootloader and the applications answer the PLCJS Discovery Protocol
(**UDP broadcast, port 20556**), modelled on Siemens/PROFINET DCP, so a module
is found and addressed **by MAC** even when its IP/subnet is unknown or wrong.
See `Application/discovery/discovery.c`.

- `IDENTIFY` (0x01/0x81) — product id, hw, fw, net mode, `in_bootloader`, IP,
  mask, name. Responses are broadcast, so they cross subnet mismatches on one
  L2 segment.
- `SET_NET` (0x02) — assign static / DHCP / link-local (applied live).
- `SET_NAME` (0x03), `FLASH_LED` (0x04), `REBOOT` (0x05), `FACTORY` (0x06).
  (The bootloader returns an error for SET_NAME/FACTORY — no persistent store.)

> **Firewall:** responses are UDP broadcast, so the host must allow **inbound
> UDP:20556** or discovery finds nothing. One-off (admin PowerShell):
> `New-NetFirewallRule -DisplayName "PLCJS PDP" -Direction Inbound -Protocol UDP -LocalPort 20556 -Action Allow`

Factory / unconfigured state is link-local, so a brand-new module never
collides with the customer network; assign it a real IP with the discovery tool
(`ModbusTool` "Обнаружение" tab) or a plain Modbus client on the labelled
`169.254.x.y` address.

### Precomputing the label identity (`tools/device_id.mjs`)

The MAC / link-local IP printed on a device label can be computed from the
96-bit UID **before flashing**, matching `Application/net_id/net_id.c` exactly:

```
node tools/device_id.mjs --stlink --variant 12di      # read UID via ST-Link
node tools/device_id.mjs <24-hex-UID> --variant 12do  # offline from a UID dump
node tools/device_id.mjs <24-hex-UID> --csv           # MAC,link-local[,name]
```

Example output: `MAC 02:00:E3:B0:5D:F7`, `link-local 169.254.93.247`,
`NetBIOS BL-12DI-B05DF7`.

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

Because the bootloader now defaults to link-local, the client **auto-resolves**
its address: `app-bootloader` learns the target MAC from the running app, then
after the reset finds the bootloader by MAC via broadcast IDENTIFY. If the
discovered (link-local) bootloader is on a different subnet than the host, the
client reassigns it live to the desired IP with a discovery `SET_NET` and
continues. `update` resolves the boot IP the same way. (Requires inbound
UDP:20556 in the host firewall — see the Discovery section.)

Default client parameters:

- desired/assign bootloader IP: `192.168.1.2` (used as the SET_NET target)
- application IP: `192.168.1.10`
- port: `502`, unit id: `1`

Useful options:

- `--boot-ip` / `--ip` - bootloader IP to use / assign
- `--app-ip` - application target
- `--nic <ip>` - local NIC to broadcast discovery from (multi-homed hosts)
- `--mac <aa:bb:..>` - target MAC to disambiguate when several are present

## Entering bootloader from the main application

The paired application enters bootloader mode by writing `0xB007` to holding
register `118`, then performing a warm reset while preserving a shared RAM flag.

Example with `pymodbus`:

```python
from pymodbus.client import ModbusTcpClient

client = ModbusTcpClient("192.168.1.10", port=502, timeout=5)
client.connect()
client.write_register(address=118, value=0xB007, device_id=1)
client.close()
```

In the validated setup:

- main application responds on `192.168.1.10`
- bootloader responds on `192.168.1.2`

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

The bootloader firmware is **identical for every board type** — all variants
share the same MCU core (Ethernet PHY + status LED on PC8), and the differing
I/O section is not touched by the bootloader. Variants differ *only* in two
compile-time identity constants:

- `PRODUCT_ID` — unique per board type (OTA requires an exact match)
- `HW_REVISION` — PCB revision, `(major << 16) | (minor << 8) | patch`
  (OTA compares the **major** byte only)

Both are guarded with `#ifndef` in `Application/flash/flash_map.h`, so a plain
`cmake` build uses the defaults, while `-DPRODUCT_ID` / `-DHW_REVISION`
override them for a specific variant.

### PRODUCT_ID scheme

`0x504C_CCTT` — `0x504C` = ASCII `"PL"` (PLCJS family marker), `CC` = channel
count (BCD), `TT` = I/O type code.

| Variant | Channels | I/O type | PRODUCT_ID | HW_REVISION |
|---------|----------|----------|------------|-------------|
| `12di`  | 12 | Digital Input   (01) | `0x504C1201` | `0x010101` |
| `12do`  | 12 | Digital Output  (02) | `0x504C1202` | `0x010101` |
| `4rtd`  | 4  | RTD temperature (03) | `0x504C0403` | `0x020100` (HW2.1) |
| `4aic`  | 4  | Analog In current (04) | `0x504C0404` | `0x010101` |
| `4aiv`  | 4  | Analog In voltage (05) | `0x504C0405` | `0x010101` |
| `4ao`   | 4  | Analog Output   (06) | `0x504C0406` | `0x010101` |
| `8aic`  | 8  | Analog In current (04) | `0x504C0804` | `0x010101` |

The table lives in [`scripts/variants.csv`](scripts/variants.csv) and is the
single source of truth. Add a new board = add one row.

### Build all variants (recommended)

```powershell
# Build every variant into dist/ (BOOTLOADER_PLCJS_<name>_hw<MM.mm.pp>.hex)
./scripts/build_all.ps1

# One variant only
./scripts/build_all.ps1 -Variant 4rtd

# Fresh (wipe each build dir first), Debug build
./scripts/build_all.ps1 -Clean -Config Debug
```

Each variant is configured into its own `build/<name>-<config>` directory (an
isolated CMake cache, so the identity constants never leak between variants),
and the resulting `.hex` / `.bin` are copied to `dist/` with a descriptive name.

### Build a single variant manually

```bash
cmake -S . -B build/4rtd -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=arm-none-eabi-toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DPRODUCT_ID=0x504C0403 -DHW_REVISION=0x020100
cmake --build build/4rtd
```

Each resulting `BOOTLOADER_PLCJS.bin` only accepts OTA images whose
`fw_header_t.product_id` matches exactly and whose `hw_revision` major byte
matches. Firmware for the wrong module variant is rejected at
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
