#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "creds.h"

static creds_id_change_t id(uint32_t n) { return (creds_id_change_t){n,false}; }
static const creds_id_change_t keep = {0,true};
static unsigned cases;
static void transitions(bool uid) {
    /* Independent membership oracle: exhaust triples from old IDs, a foreign
     * ID, UINT32_MAX and keep, with and without the relevant capability. */
    const uint32_t values[] = {11,22,33,44,UINT32_MAX};
    for (unsigned privileged=0; privileged<2; ++privileged)
    for (unsigned a=0; a<6; ++a) for (unsigned b=0; b<6; ++b)
    for (unsigned c=0; c<6; ++c) {
        creds_t old = {.uid=11,.euid=22,.suid=33,.gid=11,.egid=22,.sgid=33,
            .umask=0027,.cap_effective=CAP_KILL};
        if (privileged) old.cap_effective |= uid ? CAP_SETUID : CAP_SETGID;
        creds_t out; memset(&out,0xa5,sizeof(out)); creds_t untouched=out;
        creds_id_change_t r=a==5 ? keep:id(values[a]);
        creds_id_change_t e=b==5 ? keep:id(values[b]);
        creds_id_change_t s=c==5 ? keep:id(values[c]);
        bool permitted=privileged || ((a<3 || a==5) && (b<3 || b==5) && (c<3 || c==5));
        int result=uid ? creds_setresuid(&old,r,e,s,&out):creds_setresgid(&old,r,e,s,&out);
        assert(result==(permitted ? CREDS_OK:CREDS_EPERM));
        if (!permitted) assert(!memcmp(&out,&untouched,sizeof(out)));
        else {
            assert(creds_valid(&out));
            assert((uid ? out.uid:out.gid)==(a==5 ? 11:values[a]));
            assert((uid ? out.euid:out.egid)==(b==5 ? 22:values[b]));
            assert((uid ? out.suid:out.sgid)==(c==5 ? 33:values[c]));
            assert(out.cap_effective==(uid ? 0:old.cap_effective));
            assert(out.umask==0027 && out.ngroups==0);
            assert(uid ? out.gid==11 && out.egid==22 && out.sgid==33:
                         out.uid==11 && out.euid==22 && out.suid==33);
        }
        ++cases;
    }
}
int main(void) {
    creds_t root, out;
    creds_init_root(&root); assert(creds_valid(&root));
    assert(root.cap_effective==CAP_ALL && root.umask==0022 && !root.uid && !root.euid && !root.suid);
    transitions(true); transitions(false);
    /* Losing the last zero UID clears caps; retaining any zero preserves them. */
    for (unsigned zeros=0; zeros<8; ++zeros) {
        assert(!creds_setresuid(&root,id(zeros&1 ? 0:1000),
               id(zeros&2 ? 0:1000),id(zeros&4 ? 0:1000),&out));
        assert(out.cap_effective==(zeros ? CAP_ALL:0));
    }
    assert(!creds_capset(&root,0,&out));
    assert(!creds_inherit(&out,&out) && !out.cap_effective && !out.euid);
    assert(creds_setresuid(&out,id(1000),keep,keep,&out)==CREDS_EPERM);
    assert(!creds_capset(&out,CAP_ALL,&out) && !out.cap_effective);
    uint32_t groups[CREDS_MAX_GROUPS];
    for (unsigned i=0;i<CREDS_MAX_GROUPS;++i) groups[i]=UINT32_MAX-i;
    assert(!creds_setgroups(&root,groups,CREDS_MAX_GROUPS,&out));
    assert(out.ngroups==16 && !memcmp(out.groups,groups,sizeof(groups)));
    /* Input may alias output; replacement still uses a complete temporary. */
    assert(!creds_setgroups(&out,out.groups+1,2,&out));
    assert(out.ngroups==2 && out.groups[0]==UINT32_MAX-1 && out.groups[1]==UINT32_MAX-2);
    for (unsigned i=2;i<16;++i) assert(!out.groups[i]);
    assert(!creds_setgroups(&out,NULL,0,&out) && !out.ngroups);
    creds_t before=out;
    assert(creds_setgroups(&out,groups,17,&out)==CREDS_EINVAL);
    assert(!memcmp(&out,&before,sizeof(out)));
    assert(creds_setgroups(&out,NULL,1,&out)==CREDS_EINVAL);
    assert(!memcmp(&out,&before,sizeof(out)));
    assert(!creds_capset(&out,0,&out)); before=out;
    assert(creds_setgroups(&out,NULL,0,&out)==CREDS_EPERM);
    assert(!memcmp(&out,&before,sizeof(out)));
    assert(creds_capset(&out,1ULL<<63,&out)==CREDS_EINVAL);
    assert(!memcmp(&out,&before,sizeof(out)));
    for (uint64_t caps=0;caps<=CAP_ALL;++caps) {
        creds_t limited=root; limited.cap_effective=caps;
        assert(!creds_capset(&limited,CAP_ALL,&out) && out.cap_effective==caps);
        assert(!creds_capset(&limited,CAP_KILL|CAP_SETUID,&out));
        assert(out.cap_effective==(caps & (CAP_KILL|CAP_SETUID)));
        assert(!creds_inherit(&limited,&out) && out.cap_effective==caps);
        ++cases;
    }
    for (uint32_t mask=0;mask<65536;++mask) {
        assert(!creds_umask(&root,mask,&out) && out.umask==(mask&0777));
        assert(out.cap_effective==CAP_ALL); ++cases;
    }
    for (unsigned bad=0;bad<5;++bad) {
        creds_t invalid=root;
        if (bad==0) invalid.ngroups=17;
        if (bad==1) invalid.reserved=1;
        if (bad==2) invalid.umask=01000;
        if (bad==3) invalid.cap_effective=1ULL<<63;
        if (bad==4) invalid.groups[15]=123;
        before=out;
        assert(!creds_valid(&invalid));
        assert(creds_inherit(&invalid,&out)==CREDS_EINVAL);
        assert(creds_setresuid(&invalid,keep,keep,keep,&out)==CREDS_EINVAL);
        assert(creds_setresgid(&invalid,keep,keep,keep,&out)==CREDS_EINVAL);
        assert(creds_setgroups(&invalid,NULL,0,&out)==CREDS_EINVAL);
        assert(creds_capset(&invalid,0,&out)==CREDS_EINVAL);
        assert(creds_umask(&invalid,0,&out)==CREDS_EINVAL);
        assert(!memcmp(&out,&before,sizeof(out)));
    }
    assert(!creds_valid(NULL));
    assert(creds_inherit(NULL,&out)==CREDS_EINVAL);
    assert(creds_inherit(&root,NULL)==CREDS_EINVAL);
    puts("PASS credential values: root/inheritance, ID transition matrix, cap drop,");
    printf("groups/alias/bounds, canonical storage, failure rollback, umask (%u matrix cases)\n",cases);
    return 0;
}
