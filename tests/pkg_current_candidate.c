#include "pkg_current_candidate.h"
#include "crypto.h"
#include "string.h"
static uint64_t pc_le(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static bool pc_range(const void *p,size_t n){return p && n<=UINTPTR_MAX-(uintptr_t)p;}
static bool pc_apart(const void *a,size_t an,const void *b,size_t bn){return pc_range(a,an)&&pc_range(b,bn)&&((uintptr_t)a+an<=(uintptr_t)b || (uintptr_t)b+bn<=(uintptr_t)a);}
static bool pc_zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return false;return true;}
static int pc_unpack(ps_blob b,ps_blob parts[8]){
    static const unsigned sizes[]={2656,512,1024,8192,1024,1024,1024,1024};
    if(b.n<96 || b.n>PS_BUNDLE_MAX || memcmp(b.p,"FORT-PKG-BND-v1\0",16) ||
        pc_le(b.p+16,4)!=b.n || pc_le(b.p+20,2)!=1 || pc_le(b.p+22,2)!=8 || !pc_zero(b.p+24,8))return -1;
    size_t at=96;
    for(unsigned i=0;i<8;i++){
        const uint8_t *r=b.p+32+8*i;size_t n=(size_t)pc_le(r,4);
        if(n!=sizes[i] || n>b.n-at || pc_le(r+4,2)!=i+1 || pc_le(r+6,2))return -1;
        parts[i]=(ps_blob){b.p+at,n};at+=n;
    }
    return at==b.n?0:-1;
}
static bool pc_same_except(const uint8_t *a,const uint8_t *b,size_t n,unsigned kind){
    for(size_t i=0;i<n;i++){
        bool allowed=kind==0 ? (i>=24&&i<32)||(i>=168&&i<232)||(i>=264&&i<296)||(i>=360&&i<376)||i>=992 :
            kind==1 ? (i>=48&&i<56)||i>=8160 : (i>=440&&i<544)||i>=992;
        if(!allowed && a[i]!=b[i])return false;
    }
    return true;
}
int pc_validate(const pc_input *in,pc_workspace *w,pc_result *out){
    if(!pc_range(in,sizeof(*in)) || !pc_range(w,sizeof(*w)) || !pc_range(out,sizeof(*out)) ||
        !pc_apart(w,sizeof(*w),out,sizeof(*out)) || !pc_apart(w,sizeof(*w),in,sizeof(*in)) ||
        !pc_apart(out,sizeof(*out),in,sizeof(*in)) || !pc_range(in->policy,sizeof(*in->policy)) ||
        !pc_apart(w,sizeof(*w),in->policy,sizeof(*in->policy)) || !pc_apart(out,sizeof(*out),in->policy,sizeof(*in->policy)))return -1;
    ps_blob blobs[]={in->selector,in->baseline,in->current,in->artifact,in->executable};
    for(unsigned i=0;i<5;i++){
        if(!blobs[i].n && i>=3)continue;
        if(!pc_range(blobs[i].p,blobs[i].n) || !pc_apart(w,sizeof(*w),blobs[i].p,blobs[i].n) ||
            !pc_apart(out,sizeof(*out),blobs[i].p,blobs[i].n))return -1;
    }
    w->current.verify.verified=0;pc_result value;memset(&value,0,sizeof value);
    if(!in->grant_floor || in->grant_floor>2 || in->grant_floor!=in->state_floor ||
        in->artifact.n>8192 || in->executable.n>8192 ||
        ps_validate(in->selector.p,in->selector.n,in->baseline.p,in->baseline.n,in->policy,&w->baseline,&value.enrollment) ||
        pc_unpack(in->current,w->parts))return -1;
    ps_blob *b=w->baseline.parts,*c=w->parts;
    for(unsigned i=0;i<8;i++)if((i==0||i==1||i==5||i==7) && (b[i].n!=c[i].n || memcmp(b[i].p,c[i].p,b[i].n)))return -1;
    uint64_t sequence=pc_le(c[2].p+24,8),generation=pc_le(c[6].p+24,8);
    if(sequence<in->grant_floor || generation<in->state_floor)return -1;
    if(sequence==1 && generation==1){
        if(in->artifact.n || in->executable.n || in->current.n!=in->baseline.n || memcmp(in->current.p,in->baseline.p,in->baseline.n))return -1;
        value.grant_sequence=value.state_generation=value.credential_generation=1;*out=value;return 0;
    }
    if(sequence!=2 || generation!=2 || !in->artifact.n || !in->executable.n ||
        !pc_same_except(c[2].p,b[2].p,1024,0) || !pc_same_except(c[3].p,b[3].p,8192,1) ||
        !pc_same_except(c[4].p,b[4].p,1024,2))return -1;
    /* Account operations are deliberately excluded: unchanged closed lineage
     * and credential witnesses, with only package bindings advanced. */
    uint8_t hash[32];
    if(crypto_sha256(c[4].p,992,hash) || memcmp(hash,c[4].p+992,32))return -1;
    pm_input current={.marker=c[1].p,.ledger=c[2].p,.owners=c[3].p,.state=c[6].p,.trust=c[7].p,.account=c[4].p,
        .artifact=in->artifact.p,.artifact_size=in->artifact.n,.enrollment=&w->baseline.enrollment,
        .policy=&w->baseline.catalog_policy,.platform=in->policy->platform,.base_version=in->policy->base_version};
    if(pm_validate(&current,&w->current,&w->view))return -1;
    if(w->view.grant.phase!=PG_IDLE || w->view.grant.sequence!=2 || w->view.state_generation!=2 || !w->view.installed ||
        pc_le(c[6].p+184,8)!=in->executable.n || crypto_sha256(in->executable.p,in->executable.n,hash) || memcmp(hash,c[6].p+152,32)){
        w->current.verify.verified=0;return -1;
    }
    value.grant_sequence=sequence;value.state_generation=generation;value.credential_generation=1;value.installed=1;
    *out=value;return 0;
}
