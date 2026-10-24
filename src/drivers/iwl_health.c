#include "iwl_health.h"
#include "string.h"

void iwl_health_init(iwl_health_t *h) {
    if (!h) return;
    memset(h, 0, sizeof(*h));
    h->state = IWL_HEALTH_OK;
}

void iwl_health_on_fw_error(iwl_health_t *h, uint32_t intr) {
    if (!h) return;
    h->fw_errors++;
    h->last_err_intr = intr;
    h->consec_errors++;
    if (h->state == IWL_HEALTH_DEAD) return;
    if (h->consec_errors >= IWL_HEALTH_MAX_CONSEC_ERRORS) {
        h->state = IWL_HEALTH_DEAD;
    } else {
        h->state = IWL_HEALTH_ERROR;
    }
}

void iwl_health_on_cmd_result(iwl_health_t *h, iwl_cmd_status_t status) {
    if (!h) return;
    switch (status) {
    case IWL_CMD_OK:
        h->cmd_ok++;
        h->consec_timeouts = 0;
        h->consec_errors = 0;
        if (h->state == IWL_HEALTH_ERROR) {
            /* Firmware answered a command after an error event: it is alive. */
            h->state = IWL_HEALTH_OK;
            h->generation++;
            h->recoveries++;
        }
        break;
    case IWL_CMD_REJECTED:
        /* A response proves the firmware is alive even if it refused the command. */
        h->cmd_rejects++;
        h->consec_timeouts = 0;
        break;
    case IWL_CMD_TIMEOUT:
        h->cmd_timeouts++;
        h->consec_timeouts++;
        if (h->state != IWL_HEALTH_DEAD &&
            h->consec_timeouts >= IWL_HEALTH_MAX_CONSEC_TIMEOUTS) {
            h->state = IWL_HEALTH_DEAD;
        }
        break;
    }
}

bool iwl_health_cmd_allowed(iwl_health_t *h) {
    if (!h) return false;
    if (h->state == IWL_HEALTH_DEAD) {
        h->cmd_fast_failed++;
        return false;
    }
    return true;
}

bool iwl_health_begin_recovery(iwl_health_t *h, bool *reset_tracking) {
    if (reset_tracking) *reset_tracking = false;
    if (!h) return false;
    if (h->state == IWL_HEALTH_DEAD) return false;
    if (h->state == IWL_HEALTH_ERROR) {
        /* Firmware object state is unknown after an error event: rebuild from scratch. */
        h->state = IWL_HEALTH_OK;
        h->generation++;
        h->recoveries++;
        h->consec_timeouts = 0;
        if (reset_tracking) *reset_tracking = true;
    }
    return true;
}
