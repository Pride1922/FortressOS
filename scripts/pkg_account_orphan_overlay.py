"""Private mid-callback account cancellation; never edits production sources."""
from test_pkg_service_integration import function


def apply(work, replace):
    p='tests/pkg_account_capacity.inc'
    replace(p,'static bool capacity_active;',
            'static int account_orphan_capacity(ext4_mount_t *,e4_ns_workspace_t *,uint64_t [2]);\nstatic bool capacity_active;')
    replace(p,'    memcpy(out,left,sizeof left);return 0;',
            '    r=account_orphan_capacity(fs,w,left);if(r)return r;\n    memcpy(out,left,sizeof left);return 0;')
    p='tests/pkg_cancel_recovery.inc'
    replace('tests/integration.c','static bool cancel_preview_permit(struct ext4_engine *);',
            'static bool cancel_preview_permit(struct ext4_engine *);\nstatic bool account_orphan_permit(struct ext4_engine *);\nstatic int account_orphan_after(int,unsigned);\nstatic int account_orphan_remove(void *,unsigned);')
    replace(p,function((work/p).read_text(),'static bool cancel_preview_permit(ext4_engine_t *e){'),
            'static bool cancel_preview_permit(ext4_engine_t *e){return account_orphan_permit(e);}')
    replace(p,'if(!r&&cancel_armed&&++cancel_commits==cancel_at)assert(pc_cancel_current(thread_current()));',
            'if(!r&&cancel_armed&&++cancel_commits==cancel_at){assert(pc_cancel_current(thread_current()));account_cancel_writes=writes_seen;}')
    # Declared before cancel_dispatch, defined by the prior account fixture.
    replace('tests/integration.c','static int account_cancel_after(int,unsigned);',
            'static unsigned account_cancel_writes;\nstatic int account_cancel_after(int,unsigned);')
    p='tests/pkg_mutation_integration.inc'
    replace(p,function((work/p).read_text(),'static int mutation_account_remove(void *p,unsigned role){'),
            'static int mutation_account_remove(void *p,unsigned role){return account_orphan_remove(p,role);}')
    p='tests/pkg_account_cancel.inc'
    replace(p,function((work/p).read_text(),'static int account_cancel_after(int result,unsigned before){'),
            'static int account_cancel_after(int result,unsigned before){return account_orphan_after(result,before);}')
    replace(p,'int main(int argc,char **argv){','int account_callback_previous_main(int argc,char **argv){')
    replace('tests/integration.c','#include "pkg_account_cancel.inc"',
            '#include "pkg_account_cancel.inc"\n#include "pkg_account_orphan.inc"')
