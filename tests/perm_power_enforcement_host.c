#include <setjmp.h>
#define TEST_PERMISSIONS_ENFORCEMENT
#define main pipe_fixture_main
#include "pipe_host.c"
#undef main
static jmp_buf power_return;
static unsigned sync_calls,power_calls;
bool usb_mount_freeze_and_sync(void) {sync_calls++;return true;}
void power_reboot(void) {power_calls++;longjmp(power_return,1);}
void power_shutdown(void) {power_calls++;longjmp(power_return,1);}
int main(void) {
    current.tid=41;current.is_user=true;creds_init_root(&current.creds);
    current.creds.uid=current.creds.euid=current.creds.suid=1001;current.creds.cap_effective=0;
    assert(!process_record_begin(41,0,false,0,0));assert(process_record_bind_creds(41,&current.creds));process_record_commit(41);
    assert(sys_reboot(99)==SYSCALL_EINVAL && !sync_calls && !power_calls);
    assert(sys_reboot(REBOOT_CMD_RESTART)==SYSCALL_EPERM && !sync_calls && !power_calls);
    assert(sys_reboot(REBOOT_CMD_POWEROFF)==SYSCALL_EPERM && !sync_calls && !power_calls);
    creds_t next=current.creds;next.cap_effective=CAP_SYS_BOOT;
    assert(process_record_publish_creds(41,&current.creds,&next));
    if (!setjmp(power_return)) sys_reboot(REBOOT_CMD_RESTART);
    assert(sync_calls==1 && power_calls==1);
    process_record_exit(41,0);process_record_forget(41);
    puts("PASS actual reboot capability: denial before freeze/sync/power, explicit CAP_SYS_BOOT admission");return 0;
}
