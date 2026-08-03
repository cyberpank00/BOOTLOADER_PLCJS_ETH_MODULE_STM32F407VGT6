/**
  ******************************************************************************
  * @file    discovery.h
  * @brief   PLCJS Discovery Protocol (PDP) responder for the bootloader —
  *          UDP/20556 broadcast.
  *
  * Same wire protocol as the application responder, so one tool finds both a
  * running application and a device sitting in the bootloader (IDENTIFY reports
  * in_bootloader = 1). Supports IDENTIFY, SET_NET (applied live, not persisted),
  * FLASH_LED and REBOOT. SET_NAME / FACTORY are not applicable here (the
  * bootloader has no persistent settings store) and return an error status.
  ******************************************************************************
  */
#ifndef APPLICATION_DISCOVERY_H
#define APPLICATION_DISCOVERY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Open the UDP discovery responder. Call once after the netif is up. */
void discovery_init(void);

/** True (once) if a discovery REBOOT command arrived; the super-loop consumes
 *  this and performs the reset outside the UDP receive callback. */
bool discovery_take_pending_reboot(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_DISCOVERY_H */
