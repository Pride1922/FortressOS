"""Pending recovery and durable escrow, only in a copied private kernel."""
from pkg_boot_admission_overlay import overlay as base_overlay
from test_pkg_service_integration import function

def overlay(work,replace):
    result=base_overlay(work,replace)
    replace('src/kernel/boot_info.c','m->size==1136','m->size>=1248 && m->size<=33920')
    path='tests/pkg_boot_admission.inc'
    replace(path,'#include "pkg_selector_candidate.h"','#include "pkg_selector_candidate.h"\n#include "pkg_pending_candidate.h"\n#include "thread.h"\n#include "process_table.h"')
    replace(path,'static ps_workspace jb_workspace;','static int pn_load(ext4_mount_t *);\nstatic int pn_step(ext4_engine_t *);\nbool gk_root_admitted(void);')
    replace(path,'#include "pkg_boot_read_generated.inc"','#include "pkg_boot_read_generated.inc"\n#include "pkg_current_fs.inc"\n#include "pkg_pending_fs.inc"')
    # Superseded by exact variable-name refinement in pn_names.
    text=(work/path).read_text();replace(path,function(text,'static int jb_reserved_names('),'')
    replace(path,'    if(!r)r=jb_reserved_names(fs);','')
    replace(path,'if(engine->orphan_count || engine->orphan_recovery_required)','if(engine->tainted || engine->orphan_count || engine->orphan_recovery_required)')
    replace(path,'    if(!r && ps_validate(jb_selector,jb_selector_n,jb_bundle,jb_bundle_n,&jb_policy,&jb_workspace,&jb_result))r=-VFS_EACCES;',
            '    if(!r)r=pn_load(fs);\n    if(!r)jb_result=pn_workspace.baseline_result;')
    replace(path,'    memcpy(&jb_policy,(const void *)b->pkg_policy_vaddr,sizeof jb_policy);',
'''    memcpy(&jb_policy,(const void *)b->pkg_policy_vaddr,sizeof jb_policy);
    if(b->pkg_policy_size<1248 || b->pkg_policy_size>33920)goto refused;
    memcpy(&pn_grant_floor,(const uint8_t *)b->pkg_policy_vaddr+1136,8);
    memcpy(&pn_state_floor,(const uint8_t *)b->pkg_policy_vaddr+1144,8);
    pn_baseline_n=b->pkg_policy_size-1152;
    memcpy(pn_baseline,(const uint8_t *)b->pkg_policy_vaddr+1152,pn_baseline_n);''')
    replace(path,'    jb_engine->fs->engine=jb_engine;',
'''    jb_engine->fs->engine=jb_engine;
    jb_engine->fs->journal_mounted=true;jb_engine->fs->detached=true;pn_bound=jb_engine;''')
    replace(path,'PKGBOOT ADMITTED IDLE=1 detached','PKGBOOT ADMITTED RECOVERY detached')
    replace(path,'.phase=PKG_TX_IDLE,.sequence=1,.account_sequence=1,.credential_generation=1',
            '.phase=pn_view.phase,.sequence=pn_view.sequence,.account_sequence=1,.credential_generation=1,.remaining={pn_view.budget.remaining[0],pn_view.budget.remaining[1]},.charged={pn_view.budget.charged[0],pn_view.budget.charged[1]}')
    text=(work/path).read_text();old=function(text,'const pkg_transaction_backend *gk_backend(')
    replace(path,old,'''static int jb_recover(void *context,pkg_transaction_status *out){
    for(unsigned step=0;step<9;step++){
        tcb_t *t=thread_current();creds_t actor;
        if(!t || !process_record_creds(t->tid,&actor) || actor.euid || !(actor.cap_effective&CAP_SYS_ADMIN) || !pw_recovery_step((pw_key){t->pkg_owner.generation,t->pkg_owner.slot}))return -VFS_EACCES;
        if(context!=&jb_identity || !gk_root_admitted())return -VFS_EROFS;
        uint64_t flags=spin_lock_irqsave(&e4_lock);int r=jb_validate(jb_engine);
        bool done=!r && pn_view.action==PP_DONE;
        if(!r && !done){jb_authorized=true;r=pn_step(jb_engine);jb_authorized=false;}
        spin_unlock_irqrestore(&e4_lock,flags);
        if(r)return r;
        if(done)return jb_status(context,out);
    }
    return -VFS_EIO;
}
const pkg_transaction_backend *gk_backend(void){static const pkg_transaction_backend b={&jb_identity,jb_status,jb_recover};return &b;}''')
    replace('src/fs/ext4_transaction.inc','static int e4_semantic_commit(ext4_engine_t *e) {','static int pn_core_commit(ext4_engine_t *e) {')
    replace('src/fs/ext4.c','#include "ext4_mutate.inc"','static int e4_semantic_commit(struct ext4_engine *);\n#include "ext4_mutate.inc"')
    for unit in ('current','pending'):
        (work/f'src/kernel/pkgboot_{unit}.c').write_text(f'#include "../../tests/pkg_{unit}_candidate.c"\n')
    # Status may describe active recovery; repeat recover must be byte-idempotent.
    replace('tests/pkg_boot_ring3.c','need(!call(PB_STATUS,0,&r)&&r.phase==PKG_TX_IDLE&&r.sequence==1);',
'''need(!call(PB_STATUS,0,&r)&&r.sequence>=2);
        const char state[]="PKGBOOT STATUS phase=",seq[]=" sequence=";
        raw(SYS_WRITE,1,(uintptr_t)state,sizeof state-1);number(r.phase);
        raw(SYS_WRITE,1,(uintptr_t)seq,sizeof seq-1);number(r.sequence);raw(SYS_WRITE,1,(uintptr_t)"\\n",1);''')
    replace('tests/pkg_boot_ring3.c','need(!call(PB_RECOVER,session,&r)&&r.phase==PKG_TX_IDLE&&r.sequence==1);','need(!call(PB_RECOVER,session,&r)&&r.phase==PKG_TX_IDLE&&r.sequence>=3&&!r.remaining[0]&&!r.remaining[1]&&!r.charged[0]&&!r.charged[1]);')
    result['pending_profile']='bounded RESERVED abort / PENDING completion / RETIRING cleanup; durable observed escrow'
    return result
