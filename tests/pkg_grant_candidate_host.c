#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkg_grant_candidate.h"

static unsigned long plans, rejected, cuts, vectors;
static void accounting(void) {
    for (unsigned cap=1; cap<=10; ++cap)
    for (unsigned f=0; f<=cap; ++f)
    for (unsigned q=0; q<=cap-f; ++q)
    for (unsigned g=0; g<=f; ++g)
    for (unsigned floor_case=0;floor_case<2;++floor_case) {
        pg_budget b = {.sequence=1};
        for (unsigned i=0;i<2;++i) {
            b.free[i]=f; b.charged[i]=q; b.remaining[i]=g;
            b.total[i]=q+g; b.baseline[i]=cap-f-q;
            b.capacity[i]=cap; b.reserve[i]=floor_case ? f-g : 0;
        }
        assert(pg_budget_valid(&b));
        for (unsigned op=0;op<=PG_ORDINARY;++op)
        for (unsigned db=0;db<=cap+1;++db)
        for (unsigned di=0;di<=cap+1;++di) {
            uint64_t d[2]={db,di};
            pg_budget out, sentinel;
            memset(&sentinel,0xa5,sizeof sentinel); out=sentinel;
            int ok = db || di;
            for (unsigned i=0;i<2;++i) {
                uint64_t bound = op==PG_CHARGE || op==PG_RELEASE ? g :
                                 op==PG_REFUND || op==PG_TRANSFER ? q :
                                 op==PG_BASE_FREE ? cap-f-q : f-g-b.reserve[i];
                if (d[i]>bound) ok=0;
            }
            int rc=pg_plan(&b,op,d,&out);
            assert((rc==0)==ok);
            if (rc) { assert(!memcmp(&out,&sentinel,sizeof out)); ++rejected; }
            else {
                assert(pg_budget_valid(&out)); assert(out.sequence==2);
                /* Mock journal: before/after snapshots only, not real JBD2. */
                for (unsigned cut=0;cut<4;++cut) {
                    pg_budget disk=cut<2 ? b : out;
                    pg_budget reboot=disk;
                    assert(pg_budget_valid(&reboot));
                    assert(!memcmp(&reboot,&disk,sizeof disk));
                }
                ++plans;
            }
        }
    }
    pg_budget b={.sequence=UINT64_MAX,.capacity={UINT64_MAX,10},
        .free={UINT64_MAX,10}};
    uint64_t d[2]={1,1}; pg_budget out=b;
    assert(pg_budget_valid(&b)); assert(pg_plan(&b,PG_ORDINARY,d,&out));
    assert(!memcmp(&b,&out,sizeof b));
    b.sequence=1; b.remaining[0]=UINT64_MAX; b.charged[0]=1;
    assert(!pg_budget_valid(&b));
    /* Inode-only failure must not publish already-computed block changes. */
    b=(pg_budget){.sequence=1,.capacity={20,2},.free={20,2},
        .remaining={10,0},.total={10,0}}; out=b;
    assert(pg_plan(&b,PG_CHARGE,d,&out)); assert(!memcmp(&b,&out,sizeof b));
}
static size_t load(const char *name,uint8_t *p) {
    FILE *f=fopen(name,"rb"); assert(f);
    size_t n=fread(p,1,1025,f); assert(!ferror(f)); assert(!fclose(f)); return n;
}
static uint64_t readle(const uint8_t *p,unsigned n) {
    uint64_t v=0;
    for(unsigned i=0;i<n;++i) v|=(uint64_t)p[i]<<(8*i);
    return v;
}
static int sets(int argc,char **argv) {
    static uint8_t bytes[12000];
    unsigned passed=0,refused=0;
    for(int a=2;a<argc;++a) {
        FILE *f=fopen(argv[a],"rb"); assert(f);
        size_t n=fread(bytes,1,sizeof bytes,f); assert(!ferror(f));
        assert(feof(f)); assert(!fclose(f)); assert(n>=188);
        size_t mn=readle(bytes,4), ln=readle(bytes+4,4), tn=readle(bytes+8,4);
        assert(mn+ln+tn+188==n);
        pg_binding e={0};
        memcpy(e.uuid,bytes+12,80); memcpy(e.profile,bytes+92,32);
        memcpy(e.marker_sha256,bytes+124,32); e.epoch=readle(bytes+156,8);
        for(unsigned i=0;i<3;++i) {
            e.inode[i]=(uint32_t)readle(bytes+164+8*i,4);
            e.generation[i]=(uint32_t)readle(bytes+168+8*i,4);
        }
        pg_view v,sentinel;
        memset(&sentinel,0xa5,sizeof sentinel); v=sentinel;
        int rc=pg_validate_set(bytes+188,mn,bytes+188+mn,ln,
                               bytes+188+mn+ln,tn,&e,&v);
        assert(pg_validate_set(NULL,mn,bytes+188+mn,ln,bytes+188+mn+ln,tn,&e,&v));
        assert(pg_validate_set(bytes+188,mn,NULL,ln,bytes+188+mn+ln,tn,&e,&v));
        assert(pg_validate_set(bytes+188,mn,bytes+188+mn,ln,NULL,tn,&e,&v));
        assert(pg_validate_set(bytes+188,mn,bytes+188+mn,ln,bytes+188+mn+ln,tn,NULL,&v));
        assert(pg_validate_set(bytes+188,mn,bytes+188+mn,ln,bytes+188+mn+ln,tn,&e,NULL));
        const char *base=argv[a];
        for(const char *p=base;*p;++p) if(*p=='/') base=p+1;
        int expected=!strncmp(base,"ok-",3);
        if ((rc==0)!=expected) fprintf(stderr,"Unexpected set result: %s rc=%d\n",base,rc);
        assert((rc==0)==expected);
        if(rc) { assert(!memcmp(&v,&sentinel,sizeof v)); ++refused; }
        else ++passed;
    }
    printf("PASS record-sets=%u accepted=%u rejected=%u\n",passed+refused,passed,refused);
    return 0;
}
int main(int argc,char **argv) {
    uint8_t wire[1025], before[1025], after[1025], mixed[1024], encoded[1024];
    pg_view v, sentinel;
    assert(argc>=3);
    if(!strcmp(argv[1],"--sets")) return sets(argc,argv);
    for (int a=1;a<argc;++a) {
        size_t n=load(argv[a],wire);
        const char *base=argv[a];
        for (const char *p=argv[a];*p;++p) if (*p=='/') base=p+1;
        int expected=!strncmp(base,"ok-",3);
        memset(&sentinel,0xa5,sizeof sentinel); v=sentinel;
        int rc=pg_decode(wire,n,&v); assert((rc==0)==expected);
        memset(encoded,0xa5,sizeof encoded);
        assert((pg_encode(wire,n,encoded,sizeof encoded)==0)==expected);
        if (!expected) {
            assert(!memcmp(&sentinel,&v,sizeof v));
            for (size_t i=0;i<sizeof encoded;++i) assert(encoded[i]==0xa5);
        } else assert(!memcmp(encoded,wire,1024));
        ++vectors;
    }
    assert(load(argv[1],before)==1024); assert(load(argv[2],after)==1024);
    for (size_t cut=0;cut<=1024;++cut) {
        memcpy(mixed,after,cut); memcpy(mixed+cut,before+cut,1024-cut);
        int whole=!memcmp(mixed,before,1024)||!memcmp(mixed,after,1024);
        assert((pg_decode(mixed,1024,&v)==0)==whole);
        assert((pg_decode(mixed,1024,&v)==0)==whole); ++cuts;
    }
    assert(pg_decode(NULL,1024,&v)); assert(pg_decode(wire,1024,NULL));
    accounting();
    printf("PASS vectors=%lu byte-cuts=%lu accepted-plans=%lu rejected-plans=%lu mock-restart-selections=%lu\n",
           vectors,cuts,plans,rejected,plans*4);
    return 0;
}
