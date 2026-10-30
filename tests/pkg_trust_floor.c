#include "pkg_trust_floor.h"
#include "crypto.h"
#include "string.h"
static uint64_t le(const uint8_t *p,unsigned n){uint64_t x=0;for(unsigned i=0;i<n;i++)x|=(uint64_t)p[i]<<(8*i);return x;}
static bool zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return false;return true;}
static bool range(const void *p,size_t n){return p&&n<=UINTPTR_MAX-(uintptr_t)p;}
static bool apart(const void *p,size_t n,const void *q,size_t m){return range(p,n)&&range(q,m)&&((uintptr_t)p+n<=(uintptr_t)q||(uintptr_t)q+m<=(uintptr_t)p);}
static bool hashed(const void *p,size_t n,const uint8_t *expected){uint8_t h[32];return !crypto_sha256(p,n,h)&&!memcmp(h,expected,32);}
int tf_decode(const uint8_t w[TF_BYTES],const ps_policy *p,tf_floor *out){
    if(!range(w,1024)||!range(p,sizeof *p)||!apart(out,sizeof *out,w,1024)||!apart(out,sizeof *out,p,sizeof *p))return -1;
    unsigned count=le(w+22,2);size_t n=TF_BODY+96*count;
    if(memcmp(w,"FORT-PKG-FLOOR1\0",16)||le(w+16,4)!=n||le(w+20,2)!=1||!count||count>4||
       !p->selector_count||p->selector_count>8||!p->selector_threshold||p->selector_threshold>4||p->selector_threshold>p->selector_count||count<p->selector_threshold||
       !zero(w+n,1024-n)||!hashed(w,608,w+608)||!zero(w+36,4)||!zero(w+400,48)||
       !le(w+24,8)||le(w+24,8)==UINT64_MAX||!le(w+32,4)||!le(w+216,8)||!le(w+224,8)||!le(w+240,8)||!le(w+248,8)||
       memcmp(w+40,p->uuid,80)||memcmp(w+120,p->platform,32)||!hashed(w+480,128,w+448)||
       le(w+384,8)<1767225600ULL||le(w+392,8)>4102444800ULL||le(w+384,8)>=le(w+392,8))return -1;
    for(unsigned i=0;i<5;i++)if(zero(w+40+16*i,16))return -1;
    for(unsigned i=0;i<8;i++){
        if(i<p->selector_count){if(zero(p->selector_keys[i],32))return -1;for(unsigned j=0;j<i;j++)if(!memcmp(p->selector_keys[i],p->selector_keys[j],32))return -1;}
        else if(!zero(p->selector_keys[i],32))return -1;
    }
    if(zero(w+120,32)||zero(w+152,32)||zero(w+184,32)||zero(w+256,32)||zero(w+288,32)||zero(w+352,32))return -1;
    if(le(w+232,8)?zero(w+320,32):!zero(w+320,32))return -1;
    const uint8_t *loc=w+480;
    if(le(loc,2)!=49||le(loc+2,2)!=1||le(loc+4,4)<11||!le(loc+8,4)||le(loc+12,4)!=0600||le(loc+16,8)!=1024||
       le(loc+24,8)!=1||le(loc+32,2)!=1||!zero(loc+34,6)||le(loc+40,4)||le(loc+44,4)!=1||!le(loc+48,8)||!zero(loc+56,72))return -1;
    for(unsigned i=0;i<count;i++){
        const uint8_t *sig=w+TF_BODY+96*i;if(i&&memcmp(sig-96,sig,32)>=0)return -1;
        unsigned key=0;for(;key<p->selector_count;key++)if(hashed(p->selector_keys[key],32,sig))break;
        if(key==p->selector_count||crypto_ed25519_verify(sig+32,p->selector_keys[key],w,TF_BODY))return -1;
    }
    tf_floor f;memset(&f,0,sizeof f);f.epoch=le(w+24,8);f.contract=le(w+32,4);memcpy(f.identity,w+40,80);
    memcpy(f.platform,w+120,32);memcpy(f.kernel,w+152,32);memcpy(f.bootstrap,w+184,32);
    for(unsigned i=0;i<5;i++)f.counter[i]=le(w+216+8*i,8);
    memcpy(f.root,w+256,32);memcpy(f.catalog,w+288,32);memcpy(f.artifact,w+320,32);memcpy(f.selector,w+352,32);
    f.issued=le(w+384,8);f.expires=le(w+392,8);memcpy(f.locator,loc,128);
    if(crypto_sha256(w,1024,f.wire_hash))return -1;
    *out=f;return 0;
}
static bool same_realm(const tf_floor *a,const tf_floor *b){return !memcmp(a->identity,b->identity,80)&&!memcmp(a->platform,b->platform,32)&&!memcmp(a->locator,b->locator,128);}
int tf_transition(const tf_floor *a,const tf_floor *b,uint64_t now){
    if(!a||!b||!same_realm(a,b)||now<1767225600ULL||now>4102444800ULL||now<b->issued||now>=b->expires)return -1;
    if(a->epoch==b->epoch)return memcmp(a->wire_hash,b->wire_hash,32)?-1:0;
    if(a->epoch==UINT64_MAX||b->epoch!=a->epoch+1||b->contract<a->contract)return -1;
    for(unsigned i=0;i<5;i++)if(b->counter[i]<a->counter[i])return -1;
    if((a->counter[0]==b->counter[0]&&memcmp(a->root,b->root,32))||
       (a->counter[1]==b->counter[1]&&memcmp(a->catalog,b->catalog,32))||
       (a->counter[2]==b->counter[2]&&memcmp(a->artifact,b->artifact,32)))return -1;
    return 0;
}
int tf_admit(const tf_floor *f,const tf_observed *o){
    if(!f||!o)return -1;
    for(unsigned i=0;i<5;i++)if(o->counter[i]<f->counter[i])return -1;
    if((o->counter[0]==f->counter[0]&&memcmp(o->root,f->root,32))||
       (o->counter[1]==f->counter[1]&&memcmp(o->catalog,f->catalog,32))||
       (o->counter[2]==f->counter[2]&&memcmp(o->artifact,f->artifact,32)))return -1;
    return 0;
}
int tf_boot(const tf_floor *cur,const tf_floor *min,const tf_floor *pre,const tf_floor *end,const ps_policy *p){
    if(!cur||!min||!pre||!end||!p||!same_realm(cur,min)||!same_realm(min,pre)||!same_realm(pre,end)||
       cur->epoch<min->epoch||cur->epoch>end->epoch||tf_transition(pre,end,p->now)||
       end->contract>TF_CONTRACT||pre->contract>TF_CONTRACT||cur->contract>TF_CONTRACT||min->contract>TF_CONTRACT||
       memcmp(end->kernel,p->kernel,32)||memcmp(end->bootstrap,p->bootstrap,32)||memcmp(end->selector,p->selector_floor_hash,32)||
       memcmp(pre->kernel,p->kernel,32)||memcmp(pre->bootstrap,p->bootstrap,32)||memcmp(pre->selector,p->selector_floor_hash,32))return -1;
    const tf_floor *known=cur->epoch==min->epoch?min:cur->epoch==pre->epoch?pre:cur->epoch==end->epoch?end:NULL;
    if(!known||memcmp(cur->wire_hash,known->wire_hash,32))return -1;
    if(min->epoch<=pre->epoch){if(tf_transition(min,pre,p->now))return -1;}
    else if(min->epoch!=end->epoch||memcmp(min->wire_hash,end->wire_hash,32))return -1;
    for(unsigned i=0;i<5;i++)if(cur->counter[i]<min->counter[i])return -1;
    return 0;
}
