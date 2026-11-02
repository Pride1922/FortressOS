#include "iwlwifi.h"
#include "string.h"

/* Pure wire-command builder shared by production and host contract tests. */
void iwlwifi_build_mac_context(iwl_mac_ctx_cmd_t *out,
                              const iwl_mac_context_params_t *params) {
    memset(out, 0, sizeof(*out));
    out->id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    out->action       = params->action;
    out->mac_type     = IWL_FW_MAC_TYPE_BSS_STA;
    out->tsf_id       = 0;
    memcpy(out->node_addr, params->local_mac, 6);
    memcpy(out->bssid_addr, params->bssid, 6);
    out->cck_rates    = (params->channel <= 14) ? 0x0Fu : 0;
    out->ofdm_rates   = (params->channel <= 14) ? 0x15u : 0xFFu;
    out->short_slot   = (1u << 4); /* IWM_MAC_FLG_SHORT_SLOT (0x10) */
    /* No PTK/GTK is installed in Intel firmware: wlan_mgr authenticates and
     * decrypts both unicast and group frames in software. ACCEPT_GRP alone
     * does not request encrypted group delivery without firmware decryption.
     * Intel fw/api/mac.h defines the independent bypass bits 3 and 4. */
    out->filter_flags = IWL_MAC_FILTER_ACCEPT_GRP |
                        IWL_MAC_FILTER_IN_CONTROL_AND_MGMT |
                        IWL_MAC_FILTER_DIS_DECRYPT |
                        IWL_MAC_FILTER_DIS_GRP_DECRYPT |
                        (params->associated ? 0u : IWL_MAC_FILTER_IN_BEACON);
    out->qos_flags    = (1u << 0); /* IWM_MAC_QOS_FLG_UPDATE_EDCA */

    /* EDCA AC parameters indexed by TX FIFO: 0=BK, 1=BE, 2=VI, 3=VO */
    static const uint16_t cw_min[4] = { 15, 15, 7, 3 };
    static const uint16_t cw_max[4] = { 1023, 1023, 15, 7 };
    static const uint8_t  aifsn[4]  = { 7, 3, 2, 2 };
    static const uint16_t txop[4]   = { 0, 0, 3008, 1504 };
    for (int f = 0; f < 4; f++) {
        out->ac[f].cw_min     = cw_min[f];
        out->ac[f].cw_max     = cw_max[f];
        out->ac[f].aifsn      = aifsn[f];
        out->ac[f].fifos_mask = (uint8_t)(1u << f);
        out->ac[f].edca_txop  = txop[f];
    }

    if (params->associated) {
        uint32_t bi = (params->beacon_interval >= 20) ? params->beacon_interval : 100u;
        uint32_t dtim_off = bi * 1024u;
        out->sta.is_assoc                 = 1u;
        out->sta.dtim_time                = params->system_timestamp + dtim_off;
        out->sta.dtim_tsf                 = params->tsf + (uint64_t)dtim_off;
        out->sta.bi                       = bi;
        out->sta.bi_reciprocal            = 0xFFFFFFFFu / bi;
        out->sta.dtim_interval            = bi;
        out->sta.dtim_reciprocal          = 0xFFFFFFFFu / bi;
        out->sta.listen_interval          = 10u;
        out->sta.assoc_id                 = params->assoc_id;
        out->sta.assoc_beacon_arrive_time = params->system_timestamp;
    }
}
