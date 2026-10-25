"""Private executor kernel binding; operates only on a copied workspace."""
from pkg_boot_admission_overlay import overlay as base
from pkg_install_overlay import overlay as executor

def overlay(work,replace):
    original={p:p.read_bytes() for p in (work/'src/fs').glob('*') if p.is_file()}
    result=base(work,replace)
    for p,data in original.items():p.write_bytes(data)
    executor(work,replace)
    replace('src/fs/ext4_transaction.inc','static int px_core_commit(ext4_engine_t *e) {','static int mx_raw_commit(ext4_engine_t *e) {')
    with (work/'tests/pkg_install_forward.inc').open('a') as f:f.write('\nstatic int px_core_commit(ext4_engine_t *);\n')
    with (work/'src/fs/ext4.c').open('a') as f:f.write('\n#include "../../tests/pkg_exec_kernel.inc"\n')
    replace('src/kernel/boot_info.c','boot_mode=pkg-test-v1','boot_mode=pkg-exec-v1')
    replace('src/kernel/boot_info.c','m->size==1136','m->size>=960 && m->size<=32768')
    replace('src/kernel/root_boot.c','pkg-test-v1','pkg-exec-v1')
    replace('src/kernel/main.c','    g_installed_boot=installed_root_requested(&boot_info);','    bool mx_requested=!strcmp(boot_info.cmdline,"boot_mode=pkg-exec-v1");\n    g_installed_boot=mx_requested || installed_root_requested(&boot_info);')
    replace('src/kernel/main.c','opt/fortress/pkg_boot_read_error','opt/fortress/pkg_exec_cut')
    replace('src/kernel/main.c','    bool installed_ready=!g_installed_boot || installed_root_boot(&boot_info);','    extern bool mx_ready(void);\n    bool installed_ready=mx_requested?mx_ready():!g_installed_boot || installed_root_boot(&boot_info);\n    if(mx_requested&&!installed_ready){serial_puts("PKGEXEC HALTED no-fallback\\n");hcf();}')
    replace('src/kernel/main.c','    const char *initial_program="/bin/login";','    const char *initial_program=mx_requested?"/bin/pkg-boot":"/bin/login";')
    replace('src/kernel/installed_root.c','bool installed_root_quiesce(void) {return !published || ext4_quiesce_installed_root(nvme_quiesce)==0;}','bool installed_root_quiesce(void) {extern bool mx_ready(void);return (!published&&!mx_ready()) || ext4_quiesce_installed_root(nvme_quiesce)==0;}')
    replace('src/kernel/accounts.c','#include "accounts.h"','#include "accounts.h"\nbool mx_ready(void);')
    replace('src/kernel/accounts.c','    if (size!=sizeof(account_request_t)','    if(mx_ready()&&(op==ACCOUNT_BEGIN||op==ACCOUNT_UPDATE||op==ACCOUNT_FINISH))return SYSCALL_EROFS;\n    if (size!=sizeof(account_request_t)')
    replace('src/kernel/syscall.c','int64_t gk_test(uint64_t);','int64_t gk_test(uint64_t);\nint64_t mx_transport(uintptr_t,uintptr_t,uint64_t);')
    replace('src/kernel/syscall.c','        case 201:result=gk_test(frame->rdi);break;','        case 201:result=gk_test(frame->rdi);break;\n        case 203:result=mx_transport(frame->rdi,frame->rsi,frame->rdx);break;')
    # Owner registration is still the established generation arena. Drain occurs
    # before lifecycle disposal; no owner key is inherited by a spawned TCB.
    replace('src/kernel/thread.c','void fd_close_cloexec(tcb_t *proc) {','void fd_close_cloexec(tcb_t *proc) {\n    extern void mx_exec_owner(tcb_t *);mx_exec_owner(proc);')
    with (work/'src/kernel/pkg_boot_private.c').open('a') as f:f.write('\nbool mx_copy_fault(void){return __atomic_exchange_n(&gk_fail_copyout,0,__ATOMIC_ACQ_REL)!=0;}\nvoid mx_exec_owner(tcb_t *t){if(t&&t->pkg_owner.generation)gk_check(pw_exec(gk_key(t)));}\n')
    for unit,path in [('plan','tests/pkg_plan_candidate.c')]:
        (work/f'src/kernel/pkgexec_{unit}.c').write_text(f'#include "../../{path}"\n')
    (work/'tests/pkg_boot_ring3.c').write_text((work/'tests/pkg_exec_ring3.c').read_text())
    return result
