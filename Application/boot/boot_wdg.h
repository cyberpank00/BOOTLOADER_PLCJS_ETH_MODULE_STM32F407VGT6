/**
 * @file  boot_wdg.h
 * @brief Independent watchdog for the bootloader's network/update mode.
 *
 * Started only when the bootloader stays (update/recovery mode), never on the
 * plain path that jumps straight into the application. An IWDG cannot be stopped
 * once running, so after the handoff the application's own MX_IWDG_Init() takes
 * over the counter (it re-programs prescaler/reload and keeps refreshing it); the
 * application must therefore start its IWDG early, which all firmwares do.
 *
 * Timeout: LSI/128 * 4096 = 16.4 s nominal (11.1 s worst case, LSI 47 kHz) - longer
 * than the slowest Flash sector erase (~2 s, two sectors per call ~4 s); the
 * counter is refreshed from the main loop and between erased sectors. While a
 * debugger halts the core the counter is frozen (DBGMCU), so breakpoints do not
 * reset the board.
 */
#ifndef BOOT_WDG_H
#define BOOT_WDG_H

#ifdef __cplusplus
extern "C" {
#endif

void boot_wdg_start(void);
void boot_wdg_refresh(void);   /* no-op until boot_wdg_start() */

#ifdef __cplusplus
}
#endif

#endif /* BOOT_WDG_H */
