/* Production MAC_CONTEXT builder; firmware ABI expected bytes independently
 * pinned to Intel's v1 interface. No mock DMA/RF result is implied. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "iwlwifi.h"
_Static_assert(offsetof(iwl_mac_ctx_cmd_t, filter_flags)==52, "firmware filter offset");
int main(void) {
    iwl_mac_context_params_t p={.action=IWL_FW_CTXT_ACTION_ADD,.associated=false,
        .channel=3,.assoc_id=6,.beacon_interval=100,.system_timestamp=1234,.tsf=5678,
        .local_mac={0xf4,0xd1,8,0xc9,0x6b,0xe3},.bssid={2,3,4,5,6,7}};
    for (unsigned cycle=0;cycle<4;++cycle) {
        p.action=cycle ? IWL_FW_CTXT_ACTION_MODIFY : IWL_FW_CTXT_ACTION_ADD;
        p.associated=(cycle&1)!=0;
        p.channel=cycle>=2 ? 36 : 3;
        iwl_mac_ctx_cmd_t command; memset(&command,0xa5,sizeof(command));
        iwlwifi_build_mac_context(&command,&p);
        const uint8_t *wire=(const void *)&command;
        /* accept group + management, bypass unicast AND group hardware crypto;
         * foreign beacon delivery is requested only while unassociated. */
        assert(wire[52]==(p.associated ? 0x1e : 0x5e));
        assert(!wire[53] && !wire[54] && !wire[55]);
        assert(!(command.filter_flags & ((1u<<0)|(1u<<8)|(1u<<11))));
        assert(command.id_and_color==0 && command.action==p.action && command.mac_type==5);
        assert(!memcmp(command.node_addr,p.local_mac,6) && !memcmp(command.bssid_addr,p.bssid,6));
        assert(command.cck_rates==(p.channel<=14 ? 15u : 0u));
        assert(command.ofdm_rates==(p.channel<=14 ? 0x15u : 0xffu));
        assert(command.short_slot==16 && command.qos_flags==1);
        assert(!command.reserved_for_node_addr && !command.reserved_for_bssid_addr);
        assert(!command.protection_flags && !command.cck_short_preamble);
        assert(command.ac[4].cw_min==0 && command.ac[4].edca_txop==0);
        for (unsigned ac=0;ac<4;++ac) assert(command.ac[ac].fifos_mask==(1u<<ac));
        if (p.associated) {
            assert(command.sta.is_assoc==1 && command.sta.assoc_id==6);
            assert(command.sta.bi==100 && command.sta.dtim_interval==100);
            assert(command.sta.dtim_time==1234+102400 && command.sta.dtim_tsf==5678+102400);
            assert(command.sta.assoc_beacon_arrive_time==1234 && command.sta.listen_interval==10);
        } else {
            uint8_t zero[48]={0}; assert(!memcmp(command.raw,zero,sizeof(zero)));
        }
    }
    p.associated=true; p.beacon_interval=0;
    iwl_mac_ctx_cmd_t command; iwlwifi_build_mac_context(&command,&p);
    assert(command.sta.bi==100);
    puts("Intel MAC command PASS: ADD/MODIFY before/after association, raw group/unicast crypto, exact wire flags, addresses, reserved bytes and channel/timing preservation");
}
