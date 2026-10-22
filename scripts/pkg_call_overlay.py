"""Exact copied-source overlays for the private retained-call candidate."""
from test_pkg_service_integration import function


def tcb_overlay(work,replace):
    replace('src/kernel/thread.h','typedef struct tcb {','struct pc_call;\ntypedef struct tcb {')
    replace('src/kernel/thread.h','    handoff_registration_t handoff;',
            '    handoff_registration_t handoff;\n    struct pc_call *package_call; /* private copied-kernel candidate; owner only */')


def integration_overlay(work,replace):
    tcb_overlay(work,replace)
    p='tests/integration.c'
    replace(p,'tcb_t *thread_current(void){return NULL;}',
            'static _Thread_local tcb_t *call_host_current;\ntcb_t *thread_current(void){return call_host_current;}\n#include "pkg_call_candidate.h"')
    replace('tests/pkg_service_integration.inc','void pg_integration_detach(uint64_t pid){',
            'void pg_integration_detach(uint64_t pid){\n    pc_cancel_pid(pid);')
    p='tests/pkg_mutation_integration.inc'
    replace(p,'#include "pkg_mutation_candidate.c"','#include "pkg_mutation_candidate.c"\n#include "pkg_call_candidate.c"')
    replace(p,'static _Thread_local mutation_call *mutation_current;',
            '#define mutation_current ((mutation_call *)pc_context(thread_current()))\nstatic bool call_namespace_orphan(mutation_call *);\nstatic void call_bootstrap(void);\nstatic void call_namespace_checks(void);\nstatic void call_cancel_package(pw_key,uint64_t,uint32_t);\nstatic void call_worker_enter(unsigned);\nstatic void call_worker_exit(void);')
    replace(p,'uint32_t target;bool ready,create;','uint32_t target,namespace_ino,namespace_generation;bool ready,create,namespace;')
    replace(p,'fs->engine->orphan_count||fs->engine->tainted',
            '(fs->engine->orphan_count&&!call_namespace_orphan(call))||fs->engine->tainted')
    old=function((work/p).read_text(),'static int mutation_enter(pm_key key,mutation_call *call){')
    replace(p,old,'''static int mutation_enter(pm_key key,mutation_call *call){
    spin_debug_assert_unheld();if(mutation_current)return -VFS_EAGAIN;
    memset(call,0,sizeof(*call));call->key=key;
    call->workspace=kcalloc(1,sizeof(*call->workspace));
    if(!call->workspace){assert(pm_finish(key));return -VFS_ENOMEM;}
    if(!pc_start(thread_current(),key,mutation_mount,call)){
        kfree(call->workspace);assert(pm_finish(key));return -VFS_EAGAIN;
    }
    call->admission=pc_view(thread_current());return 0;
}''')
    replace(p,'call->admission=pc_view(thread_current());return 0;',
            'if(!pc_pin(thread_current())){assert(pc_end(thread_current()));kfree(call->workspace);return -VFS_EAGAIN;}\n    call->admission=pc_view(thread_current());return 0;')
    old=function((work/p).read_text(),'static void mutation_leave(mutation_call *call){')
    replace(p,old,'''static void mutation_leave(mutation_call *call){
    spin_debug_assert_unheld();assert(mutation_current==call);
    assert(pc_end(thread_current()));kfree(call->workspace);
}''')
    replace(p,'static int mutation_dispatch(ext4_engine_t *e){','static int call_budget_dispatch(ext4_engine_t *e){')
    replace(p,'call->target!=mutation_ordinary_ino)return -VFS_EACCES;',
            '(!call->namespace&&call->target!=mutation_ordinary_ino))return -VFS_EACCES;')
    old=function((work/p).read_text(),'static int64_t mutation_control(pm_key key,tcb_t *proc,unsigned op,account_request_t *req){')
    replace(p,old,'''static int64_t mutation_control(pm_key key,tcb_t *proc,unsigned op,account_request_t *req){
    tcb_t *previous=call_host_current;call_host_current=proc;
    mutation_call call;int r=mutation_enter(key,&call);int64_t result;
    if(r)result=syscall_from_vfs_error(r);
    else if(call.admission->kind!=PM_ACCOUNT){mutation_leave(&call);result=SYSCALL_EACCES;}
    else {result=accounts_control(op,(uintptr_t)req,sizeof(*req),&proc->creds,proc->tid);
        pg_integration_relax();mutation_leave(&call);}
    call_host_current=previous;return result;
}''')
    replace(p,'mutation_worker *w=arg;mutation_call call;assert(!mutation_enter(w->key,&call));',
            'mutation_worker *w=arg;call_worker_enter((unsigned)w->byte);mutation_call call;assert(!mutation_enter(w->key,&call));')
    replace(p,'mutation_leave(&call);return NULL;','mutation_leave(&call);call_worker_exit();return NULL;')
    replace(p,'assert(pm_configure(mutation_mount));integrated=life_active=true;pm_key key;',
            'call_bootstrap();assert(pm_configure(mutation_mount));integrated=life_active=true;pm_key key;')
    replace(p,'uint32_t unit=life_target;','uint32_t unit=life_target;call_cancel_package(owner,session,unit);')
    replace(p,'unsigned count,pins;bool pending;pm_counts(&count,&pins,&pending);',
            'call_namespace_checks();assert(!pc_count());\n    unsigned count,pins;bool pending;pm_counts(&count,&pins,&pending);')
    replace(p,'PASS mutation filesystem block=', 'PASS call-binding namespace filesystem block=')
    replace('tests/integration.c','#include "pkg_mutation_integration.inc"',
            '#include "pkg_mutation_integration.inc"\n#include "pkg_call_integration.inc"')
