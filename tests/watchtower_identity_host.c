#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../user/watchtower/watchtower_identity.h"
#include "creds.h"
static userdb_t db;
static bool load_ok=true,check_ok=true;
bool db_load(userdb_t *out,bool shadow) {assert(!shadow);if (!load_ok) return false;*out=db;return true;}
bool db_check(void) {return check_ok;}
static void reject(const char *user,const char *group) {
    spawn_identity_t out;memset(&out,0xa5,sizeof(out));spawn_identity_t before=out;
    assert(!watchtower_resolve_identity(&db,user,group,&out));
    assert(!memcmp(&out,&before,sizeof(out)));
}
int main(void) {
    const char passwd[]="root:x:0:0:root:/:/bin/sh\noperator:x:1000:1000::/run/user/1000:/bin/sh\nwide:x:4294967295:1000::/:/bin/sh\n";
    const char groups[]="root:x:0:\noperator:x:1000:\nwheel:x:10:operator\nvideo:x:44:operator\nwide:x:4294967295:\n";
    assert(db_passwd(&db,passwd,sizeof(passwd)-1));
    assert(db_group(&db,groups,sizeof(groups)-1));
    spawn_identity_t out;
    assert(watchtower_prepare_identity("operator","operator",&out));
    spawn_identity_t saved=out;
    load_ok=false;assert(!watchtower_prepare_identity("root","root",&out));
    assert(!memcmp(&out,&saved,sizeof(out)));
    load_ok=true;check_ok=false;assert(!watchtower_prepare_identity("root","root",&out));
    assert(!memcmp(&out,&saved,sizeof(out)));check_ok=true;
    assert(watchtower_prepare_identity("root","root",&out) && !out.uid);
    assert(watchtower_resolve_identity(&db,"operator","operator",&out));
    assert(out.uid==1000 && out.gid==1000 && out.ngroups==3);
    assert(out.groups[0]==1000 && out.groups[1]==10 && out.groups[2]==44);
    assert(watchtower_resolve_identity(&db,"1000","44",&out));
    assert(out.gid==44 && out.ngroups==2 && out.groups[0]==44 && out.groups[1]==10 && !out.groups[2]);
    assert(watchtower_resolve_identity(&db,"4294967295","4294967295",&out));
    assert(out.uid==UINT32_MAX && out.gid==UINT32_MAX);
    reject("missing","root");reject("root","missing");reject("1001","root");
    reject("root","1001");reject("4294967296","root");reject("-1","root");reject("","root");
    reject("root","");reject("root","4294967296");
    memcpy(db.users[2].name,"4294967296",11);reject("4294967296","root");
    /* Maximum supplementary set is retained, never clamped. */
    db.ngroups=16;
    for (unsigned i=0;i<16;i++) {
        db.groups[i].gid=i;snprintf(db.groups[i].name,32,"g%u",i);
        memcpy(db.groups[i].members,"operator",9);
    }
    db.users[1].gid=0;
    assert(watchtower_resolve_identity(&db,"operator","g0",&out) && out.ngroups==16);
    db.users[1].gid=1000;reject("operator","g0");
    creds_t actor,target,before;
    creds_init_root(&actor);
    uint32_t gs[16];for (unsigned i=0;i<16;i++) gs[i]=i;
    assert(!creds_service_identity(&actor,UINT32_MAX,UINT32_MAX,gs,16,&target));
    assert(target.uid==UINT32_MAX && target.euid==UINT32_MAX && target.suid==UINT32_MAX);
    assert(target.gid==UINT32_MAX && target.egid==UINT32_MAX && target.sgid==UINT32_MAX);
    assert(!target.cap_effective && target.ngroups==16 && !memcmp(target.groups,gs,sizeof(gs)));
    for (unsigned bit=0;bit<2;bit++) {
        actor.cap_effective=CAP_ALL & ~(bit ? CAP_SETUID : CAP_SETGID);
        before=target;
        assert(creds_service_identity(&actor,0,0,gs,1,&target)==CREDS_EPERM);
        assert(!memcmp(&target,&before,sizeof(target)));
    }
    actor.cap_effective=CAP_SETUID|CAP_SETGID;
    assert(!creds_service_identity(&actor,0,0,NULL,0,&target));
    assert(target.cap_effective==actor.cap_effective && !target.ngroups);
    before=target;
    assert(creds_service_identity(&actor,0,0,gs,17,&target)==CREDS_EINVAL);
    assert(!memcmp(&target,&before,sizeof(target)));
    assert(creds_service_identity(&actor,0,0,NULL,1,&target)==CREDS_EINVAL);
    puts("PASS identity resolution, bounds, exact groups, capability authority and failure preservation");
    return 0;
}
