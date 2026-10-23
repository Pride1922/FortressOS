#include "pkg_retirement_auth.h"
#include "crypto.h"
static uint64_t rd(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static void wr(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static void copy(uint8_t *p,const uint8_t *q,size_t n){for(size_t i=0;i<n;i++)p[i]=q[i];}
static void clear(uint8_t *p,size_t n){for(size_t i=0;i<n;i++)p[i]=0;}
static int eq(const uint8_t *p,const uint8_t *q,size_t n){for(size_t i=0;i<n;i++)if(p[i]!=q[i])return 0;return 1;}
static int zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return 0;return 1;}
static int match(const uint8_t *p,size_t n,const uint8_t *expected){uint8_t h[32];return !crypto_sha256(p,n,h)&&eq(h,expected,32);}
static int compare(const uint8_t *p,const uint8_t *q){for(unsigned i=0;i<32;i++)if(p[i]!=q[i])return p[i]<q[i]?-1:1;return 0;}
static int signature(const pa_input *in,const pa_policy *p){
    const uint8_t *c=in->certificate;size_t n=in->certificate_n;
    if(!c||n<PA_BODY||n>PA_MAX||!eq(c,(const uint8_t *)"FORT-PKG-RET-v1\0",16)||
       rd(c+16,4)!=n||rd(c+20,2)!=1||!rd(c+22,2)||rd(c+22,2)>8||n!=PA_BODY+96*rd(c+22,2)||
       !zero(c+712,312)||!p->key_count||p->key_count>8||!p->threshold||p->threshold>p->key_count||
       !match(c,n,p->certificate_sha256))return -1;
    for(unsigned i=0;i<p->key_count;i++){
        if(zero(p->keys[i],32))return -1;
        for(unsigned j=0;j<i;j++)if(eq(p->keys[i],p->keys[j],32))return -1;
    }
    uint8_t id[32];unsigned count=(unsigned)rd(c+22,2);
    for(unsigned i=0;i<count;i++){
        const uint8_t *s=c+PA_BODY+96*i;
        if(i&&compare(s-96,s)>=0)return -1;
        unsigned k=0;
        for(;k<p->key_count;k++){if(crypto_sha256(p->keys[k],32,id))return -1;if(eq(id,s,32))break;}
        if(k==p->key_count||crypto_ed25519_verify(s+32,p->keys[k],c,PA_BODY))return -1;
    }
    return count>=p->threshold?0:-1;
}
static int anchors(const pa_input *in,const uint8_t *c,pa_workspace *w){
    copy(w->anchor,in->records.account,1024);
    clear(w->anchor+24,8);clear(w->anchor+112,32);clear(w->anchor+440,40);
    clear(w->anchor+592,32);clear(w->anchor+992,32);
    if(!match(w->anchor,1024,c+648))return -1;
    copy(w->anchor,in->records.origin,1024);clear(w->anchor+24,8);clear(w->anchor+992,32);
    return match(w->anchor,1024,c+680)?0:-1;
}
/* Restore the signed baseline from current ownership plus exact selected frees.
 * Other ownership rows and every other grant byte must reproduce that baseline. */
static int baseline(const pa_input *in,const uint8_t *c,pa_workspace *w){
    const pr_view *v=&w->view;uint64_t gs=rd(c+496,8),as=rd(c+504,8);
    const uint8_t *row=c+520;
    if(!gs||!as||gs==UINT64_MAX||as==UINT64_MAX||v->sequence<gs||v->account_sequence<as||
       v->sequence-gs!=v->account_sequence-as||rd(row,2)!=1||rd(row+2,2)!=1||rd(row+12,4)!=1||
       !eq(row+16,c+240,16)||rd(row+32,8)!=in->artifact_n||rd(row+48,8)!=1||!zero(row+56,8))return -1;
    uint64_t original=rd(row+40,8),freed,delta=v->sequence-gs;
    if(original!=(in->artifact_n+rd(in->enrollment+36,4)-1)/rd(in->enrollment+36,4)||
       v->owned[0]>original||v->owned[1]>1||
       (v->classification==1&&v->size!=rd(row+32,8)))return -1;
    freed=original-v->owned[0];
    if(v->flags==1){if(delta||freed||v->owned[1]!=1)return -1;}
    else if(v->classification==1){if(delta<1+(freed?1u:0u)||delta>1+freed)return -1;}
    else if(delta<(original?3u:2u)||delta>original+2)return -1;
    copy(w->initial_owners,in->records.owners,8192);copy(w->initial_grant,in->records.grant,1024);
    unsigned at=0;
    for(unsigned i=0;i<rd(w->initial_owners+22,2);i++)if(rd(w->initial_owners+256+64*i,2)==1)at=256+64*i;
    if(!at)return -1;
    copy(w->initial_owners+at,row,64);wr(w->initial_owners+48,gs,8);
    if(crypto_sha256(w->initial_owners,8160,w->initial_owners+8160)||!match(w->initial_owners,8192,c+616))return -1;
    wr(w->initial_grant+24,gs,8);
    for(unsigned i=0;i<2;i++){
        uint64_t released=i?1-v->owned[1]:freed;
        if(v->remaining[i]<released||v->charged[i]>UINT64_MAX-released)return -1;
        wr(w->initial_grant+312+8*i,v->remaining[i]-released,8);
        wr(w->initial_grant+328+8*i,v->charged[i]+released,8);
    }
    if(crypto_sha256(w->initial_owners,8192,w->initial_grant+264)||
       crypto_sha256(w->initial_grant,992,w->initial_grant+992)||!match(w->initial_grant,1024,c+584))return -1;
    return anchors(in,c,w);
}
static int verify(const pa_input *in,const pa_policy *p,pa_workspace *w){
    if(!in||!p||!w||!p->enrollment||!p->catalog||!p->platform||!p->base_version||
       !in->state||!in->trust||!in->intent||!in->artifact||!in->executable||
       !in->artifact_n||p->catalog->now<1767225600ULL||p->catalog->now>4102444800ULL||in->artifact_n>8192||!in->executable_n||in->executable_n>8192||
       signature(in,p)||pe_verify(in->enrollment,in->enrollment_n,p->enrollment,&w->enrollment))return -1;
    const uint8_t *c=in->certificate,*en=w->enrollment.wire;
    if(rd(c+24,8)!=rd(en+24,8)||!eq(c+32,en+40,80)||!match(en,w->enrollment.length,c+112)||
       !eq(c+144,en+120,32)||!eq(c+176,en+152,32)||!eq(c+208,en+312,32)||
       rd(c+456,8)!=p->base_version||!eq(c+464,p->platform,32)||
       !rd(c+432,8)||rd(c+432,8)>rd(c+448,8)||rd(c+448,8)>=rd(c+440,8)||
       rd(c+448,8)>p->catalog->now||rd(c+448,8)!=rd(in->intent+264,8)||
       rd(c+416,8)!=in->artifact_n||rd(c+424,8)!=in->executable_n||
       !match(in->trust,1024,c+256)||!match(in->state,1024,c+288)||
       !match(in->intent,1024,c+320)||!match(in->artifact,in->artifact_n,c+352)||
       !match(in->executable,in->executable_n,c+384))return -1;
    pr_binding *b=&w->binding;clear((uint8_t *)b,sizeof(*b));copy((uint8_t *)b->grant.uuid,en+40,80);
    copy(b->grant.profile,en+152,32);copy(b->grant.marker_sha256,en+120,32);b->grant.epoch=rd(en+24,8);
    for(unsigned i=0;i<3;i++){b->grant.inode[i]=(uint32_t)rd(en+416+128*(i+1)+4,4);b->grant.generation[i]=(uint32_t)rd(en+416+128*(i+1)+8,4);}
    copy(b->capability,p->certificate_sha256,32);copy(b->account_profile,c+208,32);
    copy(b->transaction,c+240,16);copy(b->trust,c+256,32);copy(b->state,c+288,32);copy(b->intent,c+320,32);
    b->role=1;b->inode=(uint32_t)rd(c+524,4);b->generation=(uint32_t)rd(c+528,4);
    if(pr_validate(&in->records,b,&w->view)||w->view.credential_generation!=rd(c+512,8)||baseline(in,c,w))return -1;
    /* Explicit checked v4 -> existing v3 validator projection. The actual v4
     * record was fully checked above; this scratch record is never published. */
    copy(w->account_v3,in->records.account,1024);wr(w->account_v3+20,3,2);clear(w->account_v3+592,400);
    if(crypto_sha256(w->account_v3,992,w->account_v3+992))return -1;
    pm_input pm={.marker=in->records.marker,.ledger=in->records.grant,.owners=in->records.owners,
        .state=in->state,.trust=in->trust,.pending=in->intent,.pending_size=1024,
        .next=in->state,.next_size=1024,.artifact=in->artifact,.artifact_size=in->artifact_n,
        .account=w->account_v3,.enrollment=&w->enrollment,.policy=p->catalog,
        .base_version=p->base_version,.platform=p->platform};
    pm_view v;
    if(pm_validate(&pm,&w->mount,&v)||v.grant.phase!=PG_RETIRING||!v.installed||
       !eq(in->state+152,c+384,32)||rd(in->state+184,8)!=in->executable_n)return -1;
    return 0;
}
int pa_verify(const pa_input *in,const pa_policy *p,pa_workspace *w,pa_result *out){
    if(w)w->mount.verify.verified=0;
    if(!out||!w)return -1;
    if(verify(in,p,w)){w->mount.verify.verified=0;return -1;}
    out->binding=w->binding;out->view=w->view;return 0;
}
