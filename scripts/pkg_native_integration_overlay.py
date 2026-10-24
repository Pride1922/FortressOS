"""Native lifecycle and isolated actual account service on an enrolled root."""
from pkg_pending_boot_overlay import overlay as pending_overlay
from test_pkg_service_integration import function

def overlay(work,replace):
    result=pending_overlay(work,replace);path='tests/pkg_boot_admission.inc'
    replace(path,'static bool jb_authorized,jb_read_fault;','static bool jb_authorized,jb_read_fault;\nstatic bool jn_boundary(unsigned);\nstatic int jn_attach_root(void);\nint64_t jn_test(uint64_t);\nint jn_root_ready(ext4_mount_t *);\nextern bool jn_account_barrier;\nstatic bool jn_ack_lost(void);\nstatic bool jn_flush_lost(void);')
    replace(path,'return nvme_write_sector(l,p);','bool ok=nvme_write_sector(l,p);return jn_ack_lost()?false:ok;')
    replace(path,'return jb_authorized && nvme_flush();','if(!jb_authorized && !jn_account_barrier)return false;bool ok=nvme_flush();return jn_flush_lost()?false:ok;')
    replace(path,'    __atomic_store_n(&jb_admitted,1,__ATOMIC_RELEASE);serial_puts(',
            '    r=jn_attach_root();if(r)goto refused;\n    __atomic_store_n(&jb_admitted,1,__ATOMIC_RELEASE);\n    serial_puts(')
    replace(path,'    for(unsigned step=0;step<9;step++){','    unsigned completed=0;\n    for(unsigned step=0;step<9;step++){\n        if(jn_boundary(completed)){__atomic_store_n(&jn_wait,0,__ATOMIC_RELEASE);return -VFS_EINTR;}')
    replace(path,'        if(!r && !done){jb_authorized=true;r=pn_step(jb_engine);jb_authorized=false;}',
            '        if(!r && !done){jb_authorized=true;r=pn_step(jb_engine);jb_authorized=false;if(!r){completed++;__atomic_add_fetch(&jn_steps,1,__ATOMIC_RELEASE);}}')
    replace(path,'int64_t gk_storage_test(uint64_t op){if(op==13)return jb_writes;if(op==14)return jb_flushes;return SYSCALL_EINVAL;}',
            'int64_t gk_storage_test(uint64_t op){if(op==13)return jb_writes;if(op==14)return jb_flushes;return jn_test(op);}')
    # Definitions precede recover; account gate calls the established validator.
    replace(path,'static int jb_status(void *context,pkg_transaction_status *out){',
            '#include "pkg_native_integration.inc"\nstatic bool jn_ack_lost(void){if(!jn_ack_loss)return false;jn_ack_loss=false;return true;}\nstatic bool jn_flush_lost(void){if(!jn_flush_ack_loss)return false;jn_flush_ack_loss=false;return true;}\nstatic int jb_status(void *context,pkg_transaction_status *out){')
    replace('src/fs/ext4_mount_journal.inc','int ext4_publish_root(ext4_mount_t *fs) {',
            'int ext4_publish_root(ext4_mount_t *fs) {\n    extern int jn_publish_gate(ext4_mount_t *);int gate=jn_publish_gate(fs);if(gate)return gate;')
    with (work/path).open('a') as f:f.write('\nint jn_publish_gate(ext4_mount_t *fs){if(!jb_engine || fs!=jb_engine->fs)return 0;int r=jn_root_ready(fs);return r?r:__atomic_load_n(&jn_account_state,__ATOMIC_ACQUIRE)==2?0:-VFS_EAGAIN;}\n')
    # Compile an isolated instance of the real accounts.c; no fake account
    # record conversion and no replacement of the live login service.
    source=(work/'src/kernel/accounts.c').read_text()
    source=source.replace('    namespace=ext4_prepared_root(fs);','    int gate=jn_root_ready(fs);if(gate)return gate;\n    namespace=ext4_prepared_root(fs);',1)
    source=source.replace('    storage.validate=coherent;','    storage.sync=jn_sync;storage.validate=coherent;',1)
    source=source.replace('    if (size!=sizeof(account_request_t)', '    int gate=jn_account_operation(op);if(gate)return syscall_from_vfs_error(gate);\n    if (size!=sizeof(account_request_t)',1)
    prefix='''#include "accounts.h"
int jn_root_ready(ext4_mount_t *);
int jn_account_operation(unsigned);
bool jn_account_barrier;
static int jn_sync(void *fs){jn_account_barrier=true;int r=ext4_sync(fs);jn_account_barrier=false;return r;}
'''
    for name in ('prepare','provision_detached','disable','abandon','process_exit','control','seed'):
        prefix+=f'#define accounts_{name} jn_accounts_{name}\n'
    (work/'src/kernel/pkgboot_private_accounts.c').write_text(prefix+source)
    replace('src/kernel/syscall.c','int64_t gk_test(uint64_t);','int64_t gk_test(uint64_t);\nint64_t jn_account_transport(unsigned,uintptr_t,size_t);')
    replace('src/kernel/syscall.c','        case 201:result=gk_test(frame->rdi);break;',
            '        case 201:result=gk_test(frame->rdi);break;\n        case 202:result=jn_account_transport(frame->rdi,frame->rsi,frame->rdx);break;')
    (work/'tests/pkg_boot_ring3.c').write_text((work/'tests/pkg_native_integration_ring3.c').read_text())
    result['integration']='authenticated private detached VFS root; package-gated isolated actual accounts.c; private signal/fault controls'
    return result
