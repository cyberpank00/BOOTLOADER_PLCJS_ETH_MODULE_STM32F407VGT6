# AGENTS.md

Ethernet bootloader for the PLCJS module family (STM32F407VGT6, KSZ8863 switch,
firmware update over Modbus TCP). This file is the orientation map for agents;
user-facing documentation lives in `README.md` / `README_RU.md`, and the
update-utility protocol spec in `FW_UTIL_PROG_SPEC/`.

**This bootloader serves every module variant.** The code is identical across
boards; variants differ only in the `PRODUCT_ID` / `HW_REVISION` identity
constants baked in at build time.

**This repo owns the contracts the whole workspace depends on.** `flash_map.h`,
`app_validate.h` (`fw_header_t`) and `scripts/variants.csv` are the
authoritative definitions; every application firmware mirrors them by hand. A
change here is a breaking change for four other repos — see *Multi-repo*.

## Build

Toolchain: **`arm-none-eabi-gcc`** via `arm-none-eabi-toolchain.cmake` — note
this differs from the application firmwares, which use `starm-clang`. There are
no CMake presets here; the build is driven by scripts.

All variants at once (reads `scripts/variants.csv`, one isolated build dir per
variant, artefacts copied to `dist/`):

```powershell
./scripts/build_all.ps1                      # every variant, Release
./scripts/build_all.ps1 -Variant 4rtd -Clean # one variant, fresh
./scripts/build_all.ps1 -Config Debug
```

Single variant by hand:

```
cmake -B build -G Ninja --toolchain arm-none-eabi-toolchain.cmake -DPRODUCT_ID=0x504C1202 -DHW_REVISION=0x010101
cmake --build build
```

`PRODUCT_ID` and `HW_REVISION` are CMake cache variables that become the
`PRODUCT_ID_DEFAULT` / `HW_REVISION_DEFAULT` compile definitions consumed by
`flash_map.h`. Defaults are the 12DI values (`0x504C1201`, `0x010101`).

`scripts/make_metadata.ps1` produces the OTA metadata (image size + CRC32) that
accompanies a `.bin` — the header does **not** carry them.

No host-side unit tests. "Verified" means it compiles and, for anything touching
the update path, that a real OTA cycle was run against hardware.

### Node helpers (`tools/`)
- `fw_update.mjs` — reference OTA client: PDP discovery, then
  BEGIN → blocks → FINALIZE → INSTALL. The Qt tool's `FwWorker` is a port of this.
- `device_id.mjs` — computes a board's MAC / link-local label address from its
  UID (`--stlink --variant 12do`).

## Module map (`Application/`)

| Module | Responsibility |
|---|---|
| `boot/boot_main.c` | The state machine. Start here. |
| `boot/boot_entry.c` | Entry decision: stay in the bootloader or jump to the app. |
| `boot/boot_jump.c` | Vector-table relocation and the jump into the application. |
| `boot/boot_state.h` | `boot_state_t` / `boot_error_t` enums, reported over Modbus. |
| `flash/flash_map.h` | **Authoritative** Flash/RAM layout and identity defaults. |
| `flash/flash_if.c` | Erase/program primitives. |
| `flash/metadata.c` | Persistent update state in sector 5. |
| `validate/app_validate.c` | Vector sanity, header identity check, CRC32 over the image. |
| `installer/fw_installer.c` | Copies staging → application. |
| `modbus/fw_update_proto.c` | The OTA register interface. **Protocol documented in the header comment of `fw_update_proto.h`.** |
| `modbus/modbus_boot_server.c` | Modbus TCP server used while in the bootloader. |
| `discovery/` | PDP responder — advertises `inBoot` so tools can find a bootloader by MAC. |
| `net_id/` | MAC / link-local IPv4 from the MCU UID — identical algorithm to the apps, so the address does not change across the app↔bootloader transition. |
| `net_id/netbiosns.c` | NetBIOS name responder. |
| `crc/crc32.c` | CRC32 used for image verification. |
| `led/led_indication.c` | Bootloader-specific LED patterns. |
| `third_party/nanomodbus/` | Vendored protocol library. |

## Flash and RAM layout (authoritative)

| Sector | Address | Size | Usage |
|---|---|---|---|
| 0–4 | `0x08000000` | 128 KB | Bootloader |
| 5 | `0x08020000` | 128 KB | Update metadata |
| 6–7 | `0x08040000` | 256 KB | Application |
| 8–9 | `0x08080000` | 256 KB | Staging |
| 10 | `0x080C0000` | 128 KB | Application settings (NVM) |
| 11 | `0x080E0000` | 128 KB | Nominally a 3rd staging sector — **currently unused** |

Max firmware image: 256 KB (staging is treated as contiguous sectors 8–9 only;
sector 11 is non-contiguous and not used).

**Sector 11 is not free.** The 4RTD firmware stores its *write-once* calibration
there (`Application/calstore/` in that repo). Extending staging to use sector 11
would silently destroy factory calibration on those boards. Do not repurpose it
without coordinating with `PLCJS_ETH_MODULE_4RTD_D4MG_...`.

Linker: bootloader `FLASH` origin is `0x08000000`, length 128 K (applications
link at `0x08040000`). `RAM` length is `0x1FFF0`, not 128 K — the top 16 bytes
hold the no-init boot-request cell. **Both this script and every application
linker script must reserve it identically.**

## Invariants

### Contracts mirrored by hand in other repos
Change any of these and you must update all four application firmwares in the
same breath, or OTA/discovery breaks in the field:

- **`fw_header_t`** (`Application/validate/app_validate.h`) — packed, 28 bytes.
  Mirrored as `fw_header_t` in each firmware's `Application/fw_header/fw_header.h`.
- **`FW_HEADER_OFFSET = 0x200`** — the header sits at `APP_FLASH_BASE + 0x200`,
  right after the vector table. Each application's linker script pads
  `.fw_header` to exactly this offset.
- **`BOOT_REQUEST_FLAG_ADDR = 0x2001FFF0`, `BOOT_REQUEST_MAGIC = 0xB007CAFE`** —
  the app writes the magic into no-init RAM and resets to request that the
  bootloader stay active. One-shot: the bootloader clears it on entry. Survives a
  warm reset (`NVIC_SystemReset`), not a power cycle.
- **`FW_IMAGE_MAGIC = 0x504C434A`** ("PLCJ").
- **Flash map** — sector boundaries above.
- **PDP wire format** — 38-byte IDENTIFY, fixed 16-byte name field, UDP/20556.
  Shared with every firmware and with `Pdp.cpp` in ModbusTool.

CRC32 and image size are deliberately **not** in the header; they travel in OTA
metadata. Do not "improve" this by adding them.

### OTA acceptance rule
An image is accepted iff:
- `product_id` matches **exactly**, and
- `hw_revision` **major byte only** matches: `(hdr >> 8) == (expected >> 8)`.

Mind the two encodings: `fw_header_t.hw_revision` is 16-bit
`(major << 8) | minor`, while the bootloader's own `HW_REVISION_DEFAULT` and
`variants.csv` are 24-bit `(major << 16) | (minor << 8) | patch`. The comparison
uses the major field of each; keep the shift correct when touching this code.

### Version policy — bump the minor on every change

**Mandatory.** Every change to bootloader behaviour ships with
`BOOTLOADER_VERSION` in `Application/flash/flash_map.h` incremented by one minor
(`0x00000102` → `0x00000103`). It is reported over Modbus (IR `0x0002`), so it is
the only way to identify which bootloader is on a board that will not boot its
application — an un-bumped change is a defect.

- Minor bump: any bootloader-only change.
- Major bump: only for a change that breaks the app-facing contract
  (`fw_header_t`, register interface, flash map, boot-request cell).
- Pure documentation-only commits do not need a bump.

Encoding is `(major << 8) | minor`, mirroring `fw_version` in `fw_header_t`.
A chronological version-review / changelog file is planned; once it exists, add
an entry there in the same commit as the bump.

Note the bootloader version is independent of application `fw_version` — do not
try to keep them aligned.

### Update sequence
`CMD_BEGIN_UPDATE` (with size/CRC/identity params) → repeated write-block →
`CMD_FINALIZE_UPDATE` (completeness + CRC over staging) → `CMD_INSTALL_UPDATE`
(copy staging → application) → `CMD_REBOOT`. Block payload is capped at
`FW_MAX_BLOCK_SIZE = 240` bytes (120 registers).

Identity is validated at BEGIN, and the image is validated again after install
(`BOOT_VERIFY_APP`). Never shorten that to a single check: a mid-install power
loss must leave the device recoverable in the bootloader, not booting a
half-written application.

## Gotchas

- **Never brick the recovery path.** Any change to `boot_entry.c`,
  `boot_jump.c` or the linker script can make boards unreachable with no
  software remedy — they would need SWD. Review these with extra care and prefer
  additive changes.
- The bootloader must leave the KSZ8863 usable for the application, and vice
  versa: the switch survives warm resets by design (`ETHRST` stays high, board
  pull-up covers the reset window). Do not add an unconditional switch reset.
- `net_id/` must stay algorithmically identical to the applications' copy, or a
  device changes MAC/IP when it enters the bootloader and tools lose it
  mid-update.
- Discovery advertises `inBoot`; tools rely on it to distinguish a bootloader
  from a running application at the same address.
- Metadata lives in sector 5 and persists across resets — a stale session can
  affect the next boot decision. Check `metadata.c` before changing state logic.
- `variants.csv` is the single list of shipped variants; adding a board means
  adding a row there **and** creating the matching firmware repo identity.

## Multi-repo workspace

Sibling repos under `E:\STM_Programming\`:

| Repo | product_id | IR125 | Notes |
|---|---|---|---|
| `PLCJS_ETH_MODULE_12DI_D4MG_...` | `0x504C1201` | `0x12D1` | 12 discrete inputs. Reference variant for shared app subsystems. |
| `PLCJS_ETH_MODULE_12DQ_D4MG_...` | `0x504C1202` | `0x12D0` | 12 discrete outputs. |
| `PLCJS_ETH_MODULE_4RTD_D4MG_...` | `0x504C0403` | `0x04D1` | 4x RTD. **Uses sector 11 for write-once calibration.** |
| `PLCJS_Module_ModbusTool` | — | — | Qt6 client; `FwWorker`/`BootloaderProtocol` implement this repo's OTA protocol, `Pdp.cpp` the discovery protocol. |

`scripts/variants.csv` additionally declares `4aic`, `4aiv` and `4ao`, which have
no firmware repo yet.

Changing the OTA register interface in `fw_update_proto.h` requires updating
`src/protocol/BootloaderProtocol.*` and `src/tabs/FwWorker.*` in ModbusTool, plus
`tools/fw_update.mjs` here. There is no shared code between them — three
independent implementations of the same protocol.

## Maintaining this file

`AGENTS.md` is a living document, not a one-time write. Update it **in the same
commit** as the change it describes — a stale map is worse than no map, because
it actively misleads. Touch it when:

- a contract mirrored in other repos changes (`fw_header_t`, `FW_HEADER_OFFSET`,
  `BOOT_REQUEST_*`, flash map, PDP wire format) — update the *Invariants* section
  here **and** the corresponding section in every application firmware;
- the OTA register interface (`fw_update_proto.h`) or update sequence changes;
- the build procedure, toolchain or linker contract changes;
- `BOOTLOADER_VERSION` is bumped and the version-policy text needs the new
  example value;
- a variant is added to or removed from `scripts/variants.csv`;
- the flash-sector-11 / 4RTD-calibration conflict rule changes.

Pure refactors with no behavioural change do not require an update, but when in
doubt, update — the cost is a few lines of text, the cost of a stale invariant
is a bricked board.
