# Hardware-in-the-loop helpers

Small scripts used to verify the bootloader on real boards. They need an ST-Link
(fault injection only), `node`, and the host NIC on the modules' subnet; the
host firewall must allow inbound UDP 20556 (PDP replies are broadcast).

| Script | Purpose |
|---|---|
| `pdp_scan.mjs <nic> [ms]` | Broadcast IDENTIFY, print every answering module as JSON (`inBL`, ip, versions, name). |
| `pdp_setnet.mjs <nic> <mac> <ip> [mask] [gw]` | Send a targeted PDP `SET_NET` and print `OK` / `REJECTED` / `no reply`. Since bootloader 1.9 and application firmware 12DI 1.11 / 12DO 1.11 / 4RTD 2.11 / 8AIC 1.6 / 8AOC 1.4 the device answers after a ~1 s ARP probe: `REJECTED` = address in use or device busy. |
| `hil_session_timeout.ps1` | Kill an OTA mid-transfer and check the module returns to its application after `FW_SESSION_TIMEOUT_MS` (120 s). |
| `fault_inject.ps1` | Program a corrupt / erased / half-written application over ST-Link and check the bootloader refuses to start it. |

Findings worth remembering:

- An SWD reset (`-rst` or NRST `-hardRst`) is serviced only after a running Flash
  erase/program completes, so these scripts cannot imitate a real power loss in
  the middle of an erase or program (44/44 attempts landed outside it). A relay on
  the board supply is needed for that.
- `pdp_setnet.mjs` sent to an *application* changes its stored static address
  (the apps persist `SET_NET`); sent to the bootloader it is live only.
- With a VPN/TUN client running, TCP to the modules' subnet can be hijacked
  (connection opens, no reply). PDP (UDP broadcast) still works.
