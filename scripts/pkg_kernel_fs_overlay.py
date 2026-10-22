"""Exact private kernel EXT4/grant overlay; production files stay untouched."""
from test_pkg_service_integration import function


def overlay(work,replace):
    with (work/'Makefile').open('a') as f:f.write('\nCFLAGS += -I.\n')
    replace('src/fs/ext4_transaction.inc','static int e4_semantic_commit(ext4_engine_t *e) {','static int pg_life_core_commit(ext4_engine_t *e) {')
    replace('src/fs/ext4.c','#include "ext4.h"','#include "ext4.h"\nstruct ext4_engine;\nstatic int kernel_fs_preview(struct ext4_engine *);\nstatic int e4_semantic_commit(struct ext4_engine *);')
    replace('src/fs/ext4_mount_journal.inc','    e4_engine_dispose(engine);engine=NULL;',
            '    r=kernel_fs_preview(engine);if(r)goto fail;\n    e4_engine_dispose(engine);engine=NULL;')
    base=(work/'tests/pkg_grant_fs_host.c').read_text()
    life=(work/'tests/pkg_grant_lifecycle.inc').read_text()
    capacity=(work/'tests/pkg_account_capacity.inc').read_text()
    preview=(work/'tests/pkg_account_preview.inc').read_text()
    compete=(work/'tests/pkg_grant_compete.inc').read_text()
    generated='''
#include "thread.h"
#include "percpu.h"
#include "serial.h"
#include "pkg_call_candidate.h"
#include "pkg_grant_candidate.h"
#include "../../user/tools/digest.h"
#define assert(x) do {if(!(x)){serial_raw_puts("KERNEL FS ASSERT ");serial_raw_print_dec(__LINE__);__builtin_trap();}}while(0)
static uint8_t marker[512],ledger[1024],table[8192];
static pg_binding binding;
static uint32_t ids[5],marker_ino,marker_generation,life_target;
static unsigned life_commits;
static uint64_t compete_floor[2];
'''
    for sig in ('static int eligible(', 'static void p64(', 'static uint64_t u64(', 'static void hash(',
                'static void seal_records(', 'static uint8_t *raw_inode(', 'static int object(', 'static int record_io('):
        generated+=function(base,sig)+'\n'
    for sig in ('static int life_stage(', 'static int life_read(', 'static int life_validate('):generated+=function(life,sig)+'\n'
    # Constructor already performs the global ownership audit. Repeating its
    # quadratic scan with IRQs disabled starves unrelated real TLB shootdowns.
    # Retain the exact role/inode/map checks in serialized admission; perform
    # the full global audit only in constructor/final quiescent ownership.
    generated+=function(life,'static int life_validate(').replace('life_validate(', 'kernel_fs_locked_validate(').replace('||e4_orphan_validate(e)','')+'\n'
    generated+=function(life,'static int e4_semantic_commit(').replace('e4_semantic_commit(', 'kernel_fs_tagged(').replace('    if(!life_active)return pg_life_core_commit(e);\n','')+'\n'
    for sig in ('static int capacity_certificate(',):generated+=function(capacity,sig)+'\n'
    for sig in ('static int capacity_preview(', 'static int capacity_preview_budget('):generated+=function(preview,sig)+'\n'
    for sig in ('static void compete_free(', 'static bool compete_fits(', 'static int compete_empty(', 'static int compete_admit('):
        generated+=function(compete,sig).replace('life_validate(e,&v)', 'kernel_fs_locked_validate(e,&v)')+'\n'
    generated+=function(life,'static int life_create(')+'\n'
    generated+='#include "../../tests/pkg_kernel_fs.inc"\n'
    (work/'tests/pkg_kernel_fs_generated.inc').write_text(generated)
    with (work/'src/fs/ext4.c').open('a') as f:f.write('\n#include "../../tests/pkg_kernel_fs_generated.inc"\n')
    # Link the same bounded wire validator, separate from the lifecycle TU.
    (work/'src/kernel/pkg_grant_private.c').write_text('#include "../../tests/pkg_grant_candidate.c"\n')
    replace('src/kernel/pkg_call_private.c','static unsigned call_kernel_exits,call_kernel_reaps;',
            'int64_t kernel_fs_test(uint64_t);\nstatic unsigned call_kernel_exits,call_kernel_reaps;')
    replace('src/kernel/pkg_call_private.c','    if(op==8){','    if(op>=20){__atomic_fetch_or(&call_kernel_cpus,1ULL<<cpu_current()->id,__ATOMIC_RELAXED);return kernel_fs_test(op);}\n    if(op==8){')
