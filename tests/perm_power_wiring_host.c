/* Actual reboot syscall + registry snapshot. Host power/IRQ adapters only. */
#include <setjmp.h>
#define main pipe_fixture_main
#include "pipe_host.c"
#undef main
static jmp_buf power_return;
static unsigned capability_calls, sync_calls, power_calls;
static bool sync_ok;
static creds_t expected_actor;
void permission_capability_test(const creds_t *actor, uint64_t cap) {
    assert(cap == CAP_SYS_BOOT && !memcmp(actor, &expected_actor, sizeof(*actor)));
    assert(!sync_calls && !power_calls);
    capability_calls++;
}
void permission_signal_test(const creds_t *actor,const creds_t *target,unsigned sig) {
    (void)actor; (void)target; (void)sig;
}
bool usb_mount_freeze_and_sync(void) { assert(capability_calls == 1); sync_calls++; return sync_ok; }
void power_reboot(void) { assert(sync_calls == 1); power_calls++; longjmp(power_return,1); }
void power_shutdown(void) { assert(sync_calls == 1); power_calls++; longjmp(power_return,1); }
int main(void) {
    current.tid=41; current.is_user=true;
    creds_init_root(&expected_actor);
    expected_actor.uid=expected_actor.euid=expected_actor.suid=1001;
    expected_actor.cap_effective=0;
    assert(!process_record_begin(41,0,false,0,0));
    assert(process_record_bind_creds(41,&expected_actor));
    process_record_commit(41);
    assert(sys_reboot(99)==SYSCALL_EINVAL && !capability_calls && !sync_calls);
    sync_ok=false;
    assert(sys_reboot(REBOOT_CMD_RESTART)==SYSCALL_EIO);
    assert(capability_calls==1 && sync_calls==1 && !power_calls);
    for(unsigned command=0;command<2;command++) {
        capability_calls=sync_calls=power_calls=0; sync_ok=true;
        if (!setjmp(power_return)) sys_reboot(command ? REBOOT_CMD_POWEROFF : REBOOT_CMD_RESTART);
        assert(capability_calls==1 && sync_calls==1 && power_calls==1);
    }
    process_record_abort(41);
    capability_calls=sync_calls=power_calls=0;
    assert(sys_reboot(REBOOT_CMD_RESTART)==SYSCALL_ESRCH && !capability_calls && !sync_calls);
    puts("power wiring: real zero-capability actor, command validation, flush failure and both power paths PASS (host adapters only)");
    return 0;
}
