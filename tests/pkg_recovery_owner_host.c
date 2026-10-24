#define main retained_writer_lifetime_main
#include "pkg_writer_lifetime_host.c"
#undef main
int main(void){
    assert(!retained_writer_lifetime_main());
    pw_key owner=birth(800001,0),child=birth(800002,1),op;uint64_t session;
    assert(pw_begin(owner,true,&session));assert(!pw_recovery_step(owner));
    assert(pw_enter(owner,session,&op));assert(pw_recovery_step(owner));assert(!pw_recovery_step(child));
    creds_t old=credentials[0],drop=old;drop.euid=1000;drop.cap_effective=0;
    assert(process_record_publish_creds(800001,&old,&drop));assert(!pw_recovery_step(owner));
    assert(process_record_publish_creds(800001,&drop,&old));assert(pw_recovery_step(owner));
    assert(pw_exec(owner));assert(!pw_recovery_step(owner));assert(pw_complete(op));
    assert(pw_begin(owner,true,&session)&&pw_enter(owner,session,&op));assert(pw_recovery_step(owner));
    assert(pw_close(owner,session));assert(!pw_recovery_step(owner));assert(pw_complete(op));
    assert(pw_begin(owner,true,&session)&&pw_enter(owner,session,&op));death(800001,owner);assert(!pw_recovery_step(owner));
    assert(!pw_begin(child,true,&session));assert(pw_complete(op));assert(pw_begin(child,true,&session));assert(pw_close(child,session));death(800002,child);
    puts("PASS recovery step owner/credential/exec/close/exit checks, pinned operation completion and fresh reuse");return 0;
}
