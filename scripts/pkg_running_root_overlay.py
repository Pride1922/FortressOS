"""Actual root publication and actual global account service, private build."""
from pkg_native_integration_overlay import overlay as integration

def overlay(work,replace):
    result=integration(work,replace)
    def every(name,before,after,count):
        text=(work/name).read_text();assert text.count(before)==count,(name,before,text.count(before));replace(name,text,text.replace(before,after))
    # Remove the isolated account instance. All calls now use the real service.
    (work/'src/kernel/pkgboot_private_accounts.c').write_text('#include "accounts.h"\nbool jn_account_barrier;\n')
    p='tests/pkg_native_integration.inc';every(p,'jn_accounts_prepare','accounts_prepare',2)
    every(p,'jn_accounts_control','accounts_control',2)
    p='src/kernel/accounts.c'
    replace(p,'#include "accounts.h"','#include "accounts.h"\nint jh_account_gate(ext4_mount_t *);\nint jh_account_op(void *,unsigned);\nint jh_account_sync(void *);')
    replace(p,'    namespace=ext4_prepared_root(fs);','    int gate=jh_account_gate(fs);if(gate)return gate;\n    namespace=ext4_prepared_root(fs);')
    replace(p,'    storage.validate=coherent;','    storage.sync=jh_account_sync;storage.validate=coherent;')
    replace(p,'    if (size!=sizeof(account_request_t)','    int gate=jh_account_op(storage.context,op);if(gate)return syscall_from_vfs_error(gate);\n    if (size!=sizeof(account_request_t)')
    # Definitions are late in the shared private EXT4 TU; calls stay under the
    # existing filesystem lock and after the existing semantic credit fences.
    replace('src/fs/ext4.c','static int e4_semantic_commit(struct ext4_engine *);','static int e4_semantic_commit(struct ext4_engine *);\nstatic int jh_commit(struct ext4_engine *);')
    every('tests/pkg_pending_fs.inc','return pn_core_commit(e);','return jh_commit(e);',2)
    replace('src/fs/ext4_mount_journal.inc','static int e4_journal_drain(ext4_mount_t *fs,bool finish) {','static int jh_core_drain(ext4_mount_t *fs,bool finish) {')
    with (work/'tests/pkg_boot_admission.inc').open('a') as f:f.write('\n#include "pkg_running_root.inc"\n')
    replace('src/kernel/main.c','    g_installed_boot=installed_root_requested(&boot_info);',
            '    bool pkg_running_test=!strcmp(boot_info.cmdline,"boot_mode=pkg-test-v1");\n    g_installed_boot=pkg_running_test || installed_root_requested(&boot_info);')
    replace('src/kernel/main.c','    bool installed_ready=!g_installed_boot || installed_root_boot(&boot_info);',
            '    extern bool jh_finish_boot(void);extern void jh_boot_fault(bool);\n    if(pkg_running_test)jh_boot_fault(qemu_fw_cfg_has_key("opt/fortress/pkg_account_flush_loss"));\n    bool installed_ready=pkg_running_test?jh_finish_boot():!g_installed_boot || installed_root_boot(&boot_info);\n    if(pkg_running_test && !installed_ready){serial_puts("PKGROOT HALTED no-live-fallback\\n");hcf();}')
    # Quiesce the actual published root during the normal shutdown path.
    replace('src/kernel/installed_root.c','bool installed_root_quiesce(void) {return !published || ext4_quiesce_installed_root(nvme_quiesce)==0;}',
            'bool installed_root_quiesce(void) {extern bool jh_is_running(void);return (!published && !jh_is_running()) || ext4_quiesce_installed_root(nvme_quiesce)==0;}')
    (work/'tests/pkg_boot_ring3.c').write_text((work/'tests/pkg_running_root_ring3.c').read_text())
    result['running_root']='actual root publication, global SYS_ACCOUNTCTL, installed login, clean freeze and controller quiesce'
    return result
