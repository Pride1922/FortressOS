#ifndef FORTRESS_IWL_HEALTH_H
#define FORTRESS_IWL_HEALTH_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Intel iwlwifi firmware health tracking (pure logic, no hardware access).
 *
 * Replaces the former sticky `s_last_err_intr` lockout. A firmware error is an
 * event that is counted and recovered from on the next connect attempt; it never
 * silently short-circuits host commands. The only terminal condition is DEAD,
 * reached after repeated errors/timeouts with no successful command in between,
 * which fails closed quickly instead of stalling every command for a second.
 */

typedef enum {
    IWL_HEALTH_OK = 0,      /* Firmware responding normally */
    IWL_HEALTH_ERROR,       /* HW_ERR/SW_ERR seen; recovery pending on next connect */
    IWL_HEALTH_DEAD         /* Unrecoverable without reboot/firmware reload */
} iwl_health_state_t;

typedef enum {
    IWL_CMD_OK = 0,         /* Firmware acknowledged success */
    IWL_CMD_REJECTED,       /* Firmware responded with failure status (alive) */
    IWL_CMD_TIMEOUT         /* No response within the deadline */
} iwl_cmd_status_t;

#define IWL_HEALTH_MAX_CONSEC_TIMEOUTS  4u
#define IWL_HEALTH_MAX_CONSEC_ERRORS    3u

typedef struct {
    iwl_health_state_t state;
    uint32_t generation;            /* Incremented on every recovery */
    uint32_t consec_timeouts;       /* Reset by any firmware response */
    uint32_t consec_errors;         /* Reset by any successful command */
    uint32_t last_err_intr;         /* Last CSR_INT value that carried an error bit */

    /* Telemetry counters (monotonic) */
    uint32_t fw_errors;
    uint32_t cmd_ok;
    uint32_t cmd_rejects;
    uint32_t cmd_timeouts;
    uint32_t cmd_fast_failed;
    uint32_t recoveries;
} iwl_health_t;

void iwl_health_init(iwl_health_t *h);

/* A HW_ERR/SW_ERR interrupt bit was observed. */
void iwl_health_on_fw_error(iwl_health_t *h, uint32_t intr);

/* Record the outcome of a host command that waited for a response. */
void iwl_health_on_cmd_result(iwl_health_t *h, iwl_cmd_status_t status);

/* True if the command layer should attempt to send commands at all. */
bool iwl_health_cmd_allowed(iwl_health_t *h);

/*
 * Called at the start of every connect attempt. Returns false if the device is DEAD
 * (caller must fail closed). Sets *reset_tracking when firmware-side objects
 * (station, binding, MAC context, time event) must be considered lost and host
 * bookkeeping reset before rebuilding them.
 */
bool iwl_health_begin_recovery(iwl_health_t *h, bool *reset_tracking);

#endif /* FORTRESS_IWL_HEALTH_H */
