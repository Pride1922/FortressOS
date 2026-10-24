#include "pkg_pending_candidate.h"
#include "crypto.h"
#include "string.h"
static uint64_t get(const uint8_t *p,unsigned n){uint64_t x=0;for(unsigned i=0;i<n;i++)x|=(uint64_t)p[i]<<(8*i);return x;}
static void put(uint8_t *p,unsigned n,uint64_t x){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(x>>(8*i));}
static void hash(const void *p,size_t n,uint8_t out[32]){(void)crypto_sha256(p,n,out);}
static void seal(uint8_t *p,size_t n){hash(p,n-32,p+n-32);}
static bool zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return false;return true;}
static bool range(const void *p,size_t n){return p && n<=UINTPTR_MAX-(uintptr_t)p;}
static bool apart(const void *a,size_t an,const void *b,size_t bn){return range(a,an)&&range(b,bn)&&((uintptr_t)a+an<=(uintptr_t)b || (uintptr_t)b+bn<=(uintptr_t)a);}
typedef struct {ps_blob b;size_t at;} stream;
static long read_bytes(void *ctx,void *out,size_t n){stream *s=ctx;if(n>s->b.n-s->at)n=s->b.n-s->at;memcpy(out,s->b.p+s->at,n);s->at+=n;return (long)n;}
static bool object(const pp_object *o,const uint8_t *en,const uint8_t expected[32],uint64_t size){
    if(!o || !o->inode || !o->generation || o->uid || o->gid || o->links!=1 || o->size!=size ||
        !size || size>8192 || o->mode!=(o->role==17?0755:0600) || !o->count || o->count>4 || memcmp(o->hash,expected,32))return false;
    uint64_t logical=0,capacity=get(en+344,8),bs=get(en+36,4);
    for(unsigned j=0;j<16;j++)if(o->inode==get(en+420+128*j,4))return false;
    for(unsigned i=0;i<o->count;i++){
        uint64_t p=o->extent[i].physical,n=o->extent[i].length;
        if(o->extent[i].logical!=logical || !n || !p || p>=capacity || n>capacity-p)return false;
        for(unsigned a=0;a<16;a++)for(unsigned b=0;b<get(en+448+128*a,2);b++){
            const uint8_t *x=en+456+128*a+16*b;uint64_t q=get(x+8,8),qn=get(x+4,4);
            if(p<q+qn && q<p+n)return false;
        }
        for(unsigned j=0;j<i;j++)if(p<o->extent[j].physical+o->extent[j].length && o->extent[j].physical<p+n)return false;
        logical+=n;
    }
    return logical==o->blocks && logical==(size+bs-1)/bs && zero((const uint8_t *)(o->extent+o->count),(4-o->count)*sizeof(o->extent[0]));
}
static int validate(const pp_input *in,pp_workspace *w,pp_view *out){
    if(!range(in,sizeof(*in)) || !apart(w,sizeof(*w),out,sizeof(*out)) || !apart(w,sizeof(*w),in,sizeof(*in)) ||
        !apart(out,sizeof(*out),in,sizeof(*in)) || !range(in->authority.policy,sizeof(ps_policy)))return -1;
    const pc_input *a=&in->authority;ps_blob blobs[]={a->selector,a->baseline,a->current,a->artifact,in->intent};
    for(unsigned i=0;i<5;i++)if((blobs[i].n || i<4) && (!apart(w,sizeof(*w),blobs[i].p,blobs[i].n)||!apart(out,sizeof(*out),blobs[i].p,blobs[i].n)))return -1;
    if(!apart(w,sizeof(*w),a->policy,sizeof(*a->policy)) || !apart(out,sizeof(*out),a->policy,sizeof(*a->policy)) ||
        in->object_count>4 || (in->object_count && (!apart(w,sizeof(*w),in->objects,in->object_count*sizeof(*in->objects)) ||
        !apart(out,sizeof(*out),in->objects,in->object_count*sizeof(*in->objects)))) ||
        (in->executable && (!apart(w,sizeof(*w),in->executable,sizeof(*in->executable)) || !apart(out,sizeof(*out),in->executable,sizeof(*in->executable)))))return -1;
    w->authority.verified=w->mount.verify.verified=0;
    if(!a->grant_floor || a->grant_floor>64 || !a->state_floor || a->state_floor>2 || a->artifact.n>8192 || !a->artifact.n ||
        ps_validate(a->selector.p,a->selector.n,a->baseline.p,a->baseline.n,a->policy,&w->baseline,&w->baseline_result) || pc_bundle_open(a->current,w->parts))return -1;
    ps_blob *base=w->baseline.parts,*c=w->parts;const uint8_t *en=base[0].p,*g=c[2].p,*t=c[3].p,*account=c[4].p;
    for(unsigned i=0;i<8;i++)if((i==0||i==1||i==5||i==7) && (c[i].n!=base[i].n || memcmp(c[i].p,base[i].p,base[i].n)))return -1;
    for(unsigned i=0;i<1024;i++)if(!((i>=440&&i<592)||i>=992) && account[i]!=base[4].p[i])return -1;
    uint8_t digest[32];hash(account,992,digest);if(memcmp(digest,account+992,32))return -1;
    const uint8_t *trust=base[7].p;size_t catalog_n=(size_t)get(trust+96,4);stream s={a->artifact,0};
    if(fpk_verify_authorized(&w->authority,trust+256,catalog_n,&w->baseline.catalog_policy,(const uint8_t *)"demo",4,read_bytes,&s) ||
        w->authority.manifest.file_count!=1 || w->authority.manifest.build!=1 || w->authority.manifest.command.length!=4 ||
        memcmp(w->authority.manifest.command.bytes,"demo",4) || w->authority.manifest.base_min>a->policy->base_version ||
        w->authority.manifest.base_max<a->policy->base_version || memcmp(w->authority.manifest.platform_id,a->policy->platform,32) ||
        !w->authority.manifest.files[0].size || w->authority.manifest.files[0].size>8192)return -1;
    memcpy(w->target,base[6].p,1024);put(w->target+24,8,2);hash(base[6].p,1024,w->target+80);put(w->target+112,8,1);
    hash(a->artifact.p,a->artifact.n,w->target+120);memcpy(w->target+152,w->authority.manifest.files[0].sha256,32);
    put(w->target+184,8,w->authority.manifest.files[0].size);w->target[192]=4;memcpy(w->target+193,"demo",4);seal(w->target,1024);
    pp_view v;memset(&v,0,sizeof v);v.sequence=get(g+24,8);v.phase=(uint32_t)get(g+22,2);v.state_generation=get(c[6].p+24,8);v.installed=v.state_generation==2;
    if(!v.sequence || v.sequence<a->grant_floor || v.sequence>64 || v.state_generation<a->state_floor || v.phase>PG_RETIRING ||
        (v.installed?memcmp(c[6].p,w->target,1024):memcmp(c[6].p,base[6].p,1024)))return -1;
    pm_input mi={.marker=c[1].p,.ledger=g,.owners=t,.state=c[6].p,.trust=c[7].p,.account=account,
        .enrollment=&w->baseline.enrollment,.policy=&w->baseline.catalog_policy,.platform=a->policy->platform,.base_version=a->policy->base_version};
    if(v.phase || v.installed){mi.artifact=a->artifact.p;mi.artifact_size=a->artifact.n;}
    if(v.phase){mi.next=w->target;mi.next_size=1024;mi.pending=in->intent.p;mi.pending_size=in->intent.n;}
    else if(in->intent.n)return -1;
    if(pm_validate(&mi,&w->mount,&w->mounted) || (v.phase==PG_RESERVED && in->executable) || (v.installed && !in->executable))return -1;
    if(in->executable && (in->executable->role!=17 || !object(in->executable,en,w->target+152,get(w->target+184,8))))return -1;
    v.publication=in->executable?1:0;
    if(get(t+22,2)!=in->object_count)return -1;
    unsigned roles=0;
    for(unsigned i=0;i<in->object_count;i++){
        const pp_object *o=in->objects+i;const uint8_t *row=t+256+64*i;
        const uint8_t *expected=NULL;uint64_t size=0;
        if(o->role==1){expected=w->target+120;size=a->artifact.n;}
        else if(o->role==17){expected=w->target+152;size=get(w->target+184,8);}
        else if(o->role==18){hash(w->target,1024,digest);expected=digest;size=1024;}
        else if(o->role==19 && v.phase>=PG_PENDING){hash(in->intent.p,in->intent.n,digest);expected=digest;size=1024;}
        else return -1;
        if(!object(o,en,expected,size) || get(row,2)!=o->role || get(row+2,2)!=1 || get(row+4,4)!=o->inode ||
            get(row+8,4)!=o->generation || get(row+12,4)!=1 || get(row+32,8)!=size || get(row+40,8)!=o->blocks || get(row+48,8)!=1)return -1;
        for(unsigned j=0;j<i;j++){
            const pp_object *q=in->objects+j;if(q->inode==o->inode)return -1;
            for(unsigned x=0;x<q->count;x++)for(unsigned y=0;y<o->count;y++)
                if(q->extent[x].physical<o->extent[y].physical+o->extent[y].length && o->extent[y].physical<q->extent[x].physical+q->extent[x].length)return -1;
        }
        if(o->role==17 && in->executable && memcmp(o,in->executable,sizeof *o))return -1;
        roles|=1u<<(o->role==1?0:o->role-16);
    }
    if(v.phase>=PG_PENDING && (!(roles&2) || !(roles&8)))return -1;
    if(v.phase==PG_IDLE && in->object_count)return -1;
    uint64_t bs=get(en+36,4),limit[2]={8+(a->artifact.n+bs-1)/bs+(get(w->target+184,8)+bs-1)/bs+2,8};
    for(unsigned i=0;i<2;i++){
        v.budget.free[i]=in->free[i];v.budget.capacity[i]=in->capacity[i];v.budget.reserve[i]=i?16:(256*1024+bs-1)/bs;
        v.budget.total[i]=get(g+296+8*i,8);v.budget.remaining[i]=get(g+312+8*i,8);v.budget.charged[i]=get(g+328+8*i,8);
        if((v.phase && v.budget.total[i]!=limit[i]) || in->capacity[i]!=(i?get(en+352,4):get(en+344,8)) ||
            v.budget.free[i]>v.budget.capacity[i] || v.budget.charged[i]>v.budget.capacity[i]-v.budget.free[i])return -1;
        v.budget.baseline[i]=v.budget.capacity[i]-v.budget.free[i]-v.budget.charged[i];
    }
    v.budget.sequence=v.sequence;if(!pg_budget_valid(&v.budget))return -1;
    if(!v.phase)v.action=PP_DONE;
    else if(v.phase==PG_RESERVED){
        v.action=PP_FINISH;
        if(in->object_count){v.action=PP_DELETE;v.role=in->objects[0].role;}
    }else if(!v.installed)v.action=v.publication?PP_PUBLISH:PP_ACTIVATE;
    else if(v.phase==PG_PENDING)v.action=PP_RETIRE;
    else{
        v.action=PP_FINISH;v.role=19;
        for(unsigned i=0;i<in->object_count;i++)if(in->objects[i].role!=17 && in->objects[i].role!=19){v.action=PP_DELETE;v.role=in->objects[i].role;break;}
    }
    *out=v;return 0;
}
int pp_next(const pp_input *in,const pp_workspace *w,const pp_view *v,pp_change *out){
    if(!in || !w || !v || !out || v->action==PP_DONE || v->sequence>=64)return -1;
    memcpy(out->grant,w->parts[2].p,1024);memcpy(out->owners,w->parts[3].p,8192);memcpy(out->account,w->parts[4].p,1024);memcpy(out->state,w->parts[6].p,1024);
    out->action=v->action;out->role=v->role;pg_budget b=v->budget;const pp_object *removed=NULL;
    for(unsigned i=0;i<in->object_count;i++)if(in->objects[i].role==v->role)removed=in->objects+i;
    if(v->action==PP_DELETE || (v->action==PP_FINISH && v->role)){
        if(!removed)return -1;
        uint64_t f[2]={b.free[0]+removed->blocks,b.free[1]+1},q[2]={b.charged[0]-removed->blocks,b.charged[1]-1};pg_budget n;
        if(pg_reclaim(&b,f,q,&n))return -1;
        b=n;b.sequence=v->sequence;
        unsigned count=(unsigned)get(out->owners+22,2),at=0;
        while(at<count && get(out->owners+256+64*at,2)!=v->role)at++;
        if(at==count)return -1;
        memmove(out->owners+256+64*at,out->owners+320+64*at,64*(count-at-1));memset(out->owners+256+64*(count-1),0,64);put(out->owners+22,2,count-1);
    }
    if(v->action==PP_ACTIVATE || v->action==PP_PUBLISH)memcpy(out->state,w->target,1024);
    if(v->action==PP_RETIRE)put(out->grant+22,2,PG_RETIRING);
    if(v->action==PP_FINISH){
        if(v->installed){uint64_t cost[2]={0,0};for(unsigned i=0;i<in->object_count;i++)if(in->objects[i].role==17){cost[0]=in->objects[i].blocks;cost[1]=1;}
            pg_budget n;if(!cost[1] || pg_plan(&b,PG_TRANSFER,cost,&n))return -1;
            b=n;b.sequence=v->sequence;}
        pg_budget n;uint64_t left[2]={b.remaining[0],b.remaining[1]};if(pg_plan(&b,PG_RELEASE,left,&n))return -1;
        b=n;
        if(b.charged[0] || b.charged[1])return -1;
        put(out->grant+22,2,0);memset(out->grant+40,0,16);memset(out->grant+136,0,32);hash(out->state,1024,out->grant+168);memcpy(out->grant+200,out->grant+168,32);
        put(out->grant+348,2,0);uint64_t generation=get(out->state+24,8);put(out->grant+360,8,generation);put(out->grant+368,8,generation);memset(out->grant+392,0,40);
        memset(out->owners+32,0,16);put(out->owners+22,2,0);memset(out->owners+256,0,4096);
    }
    uint64_t seq=v->sequence+1;put(out->grant+24,8,seq);put(out->owners+48,8,seq);
    for(unsigned i=0;i<2;i++){put(out->grant+296+8*i,8,b.total[i]);put(out->grant+312+8*i,8,b.remaining[i]);put(out->grant+328+8*i,8,b.charged[i]);out->free_after[i]=b.free[i];}
    seal(out->owners,8192);hash(out->owners,8192,out->grant+264);seal(out->grant,1024);
    put(out->account+440,8,seq);hash(out->grant,1024,out->account+448);hash(out->state,1024,out->account+512);
    if(v->action==PP_FINISH){memset(out->account+544,0,48);}seal(out->account,1024);
    return 0;
}
int pp_validate(const pp_input *in,pp_workspace *w,pp_view *out){
    int r=validate(in,w,out);
    /* Only clear accessible disjoint scratch; invalid aliases remain untouched. */
    if(r && range(in,sizeof(*in)) && apart(w,sizeof(*w),out,sizeof(*out)) &&
       apart(w,sizeof(*w),in,sizeof(*in)) && range(in->authority.policy,sizeof(ps_policy)) &&
       apart(w,sizeof(*w),in->authority.policy,sizeof(ps_policy))){
        ps_blob b[]={in->authority.selector,in->authority.baseline,in->authority.current,in->authority.artifact,in->intent};
        bool safe=true;
        for(unsigned i=0;i<5;i++)if(b[i].n && !apart(w,sizeof(*w),b[i].p,b[i].n))safe=false;
        if(in->object_count && (in->object_count>4 || !apart(w,sizeof(*w),in->objects,in->object_count*sizeof(*in->objects))))safe=false;
        if(in->executable && !apart(w,sizeof(*w),in->executable,sizeof(*in->executable)))safe=false;
        if(safe)w->authority.verified=w->mount.verify.verified=0;
    }
    return r;
}
