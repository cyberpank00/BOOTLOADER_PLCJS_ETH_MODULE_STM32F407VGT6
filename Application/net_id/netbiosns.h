/**
  ******************************************************************************
  * @file    netbiosns.h
  * @brief   Minimal NetBIOS name-service responder (UDP/137).
  *
  * Lets the bootloader answer NetBIOS name queries so it shows up by name on
  * managed switches / network tools even though it uses a static IP (no DHCP,
  * so DHCP option-12 hostname is not available). NetBIOS names are limited to
  * 15 characters.
  ******************************************************************************
  */
#ifndef APPLICATION_NETBIOSNS_H
#define APPLICATION_NETBIOSNS_H

#ifdef __cplusplus
extern "C" {
#endif

/** Set the advertised NetBIOS name (copied; truncated/upper-cased to 15 chars). */
void netbiosns_set_name(const char* name);

/** Open the UDP/137 responder. Call after the netif is up. */
void netbiosns_init(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_NETBIOSNS_H */
