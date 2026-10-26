# FW Utility Programmer Specification

## Summary

Create a graphical firmware programming utility for the STM32F407 Ethernet module.
The tool must reuse the existing Modbus TCP OTA workflow implemented in
`tools/fw_update.mjs` and provide an operator-friendly UI without requiring a
console.

## Goals

- Read bootloader status from the module
- Select a `.bin` firmware image
- Upload firmware over Modbus TCP
- Show progress and errors clearly
- Support abort and reboot actions

## Non-goals

- Changing the bootloader protocol
- Replacing the current OTA format
- Adding a new firmware transport

## Functional Requirements

### Connection

- Connect to a target by IP address and TCP port
- Use the same Modbus TCP register map and command sequence as `fw_update.mjs`
- Select a target MAC through PDP (UDP/20556). Several bootloaders require an
  explicit selection; an observed IP conflict is an error.
- Before any modifying BL command, read MAC via the same TCP connection.
  Repeat after reconnect and SET_NET; a magic response alone is insufficient.
- Protected writes require BL 1.8+. Older BL status remains readable, with no
  automatic fallback for writes. Node and Qt must enforce the same policy.
- Allow the operator to retry a failed connection

### Status View

- Show:
  - boot state
  - last error
  - app valid flag
  - app version
  - product ID
  - hardware revision
  - received blocks / total blocks
  - image size
  - image CRC32
  - command status
  - staging valid flag
  - selected MAC, and the verified connected device identity

### Firmware Upload

- Load a local `.bin` file
- Compute and display image size and CRC32
- Send `BEGIN_UPDATE`
- Transfer blocks with progress feedback
- Send `FINALIZE_UPDATE`
- Send `INSTALL_UPDATE`
- Report success or failure in human-readable form

Validate the 28-byte image header at offset `0x200` before BEGIN: product ID
exactly matches, and `(header_hw >> 8) == (board_hw >> 16)`. Board HW is 24-bit;
image/OTA HW is 16-bit. FW version is `(major<<8)|minor`, taken from the image.
Reject invalid headers rather than substituting a default identity.

Block size must be a multiple of four, at most 240; count is
`ceil(image_size/block_size)`. One FC16 at `0x0100` carries index(2 registers),
length(1), and `ceil(length/2)` data registers. The last block contains exactly
the remainder. Identical retries are idempotent; changed duplicates fail.
The BL compares Flash read-back and scans the whole bitmap at FINALIZE.

FINALIZE saves a verified image and waits for explicit INSTALL, also after reset.
Once INSTALL is durably started, a reset restarts erase/copy from verified staging.
After a reset during reception, current clients issue BEGIN and resend the image.
ABORT is unavailable during installation; it never restores an erased application.
Report Modbus exception 6 as busy. Do not infer successful installation from a
timeout/disconnect: require BL OK or the selected MAC's application with the
expected product/version.

The BL retains one TCP client. New connections cannot evict an active client;
idle timeout is 30 s, partial ADU timeout 5 s, keepalive is enabled. Reconnection
does not itself cancel OTA. Allow 15 s request timeouts for Flash operations.
PDP SET_NET/REBOOT require an exact nonzero target MAC and are blocked during
installation. IDENTIFY is still broadcast-capable.

Current IR offsets (32-bit values high word first): HW `0x000A..0x000B`,
error `0x000C`, block count `0x000D..0x000E`, received `0x000F..0x0010`,
size `0x0011..0x0012`, CRC `0x0013..0x0014`, command status `0x0015`,
staging valid `0x0016`, app product ID `0x0017..0x0018`, MAC `0x0019..0x001B`.
For MAC `02:00:12:34:56:78`, the three registers are `0200 1234 5678`.
The full authoritative map is the comment in `fw_update_proto.h`.

### Control Actions

- `Status`
- `Abort`
- `Reboot`
- `Select firmware`
- `Flash`

## UI Requirements

- Single-window desktop application
- Input fields for target IP, port and selected MAC
- File picker for firmware image
- Progress bar for block transfer
- Log panel for actions and errors
- Read-only status panel for bootloader state

## Error Handling

- Show network errors clearly
- Translate bootloader error codes to readable text
- Keep the UI responsive during upload
- Preserve logs for the current session

## Acceptance Criteria

- The operator can read bootloader status without a console
- The operator can flash a valid firmware image successfully
- The tool shows progress while uploading
- The tool reports invalid CRC and interrupted-transfer failures correctly
- The tool can abort reception, and refuses cancellation during installation
- Wrong MAC and multiple-BL/IP-conflict cases cannot initiate an update
- Duplicate, missing, short, out-of-range and last partial blocks are covered
- Connection loss does not produce a false success report
- Host tests complement, but do not replace, real OTA and power-cut tests

## Reference

The GUI must be based on the behavior of:

- `tools/fw_update.mjs`
- the current bootloader register map
- the tested app ↔ bootloader warm-reset workflow
