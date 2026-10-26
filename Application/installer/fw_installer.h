/**
 * @file  fw_installer.h
 * @brief Copy verified staging image into the application region.
 */
#ifndef FW_INSTALLER_H
#define FW_INSTALLER_H

#include <stdbool.h>
#include <stdint.h>
#include "metadata.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INST_IDLE,
    INST_ERASING,
    INST_COPYING,
    INST_VERIFYING,
    INST_DONE,
    INST_ERROR,
} installer_state_t;

bool              fw_installer_start(metadata_t *meta);
installer_state_t fw_installer_poll(metadata_t *meta);
installer_state_t fw_installer_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_INSTALLER_H */
