#include "pkg_retirement_candidate.h"
#include "user/tools/digest.h"
static uint64_t rd(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static int eq(const uint8_t *a,const uint8_t *b,size_t n){for(size_t i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1;}
static int zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return 0;return 1;}
static int hash(const uint8_t *p,size_t n,const uint8_t *expected){uint8_t h[32];digest_t c;digest_init(&c,DIGEST_SHA256);if(!digest_update(&c,p,n))return 0;digest_final(&c,h);return eq(h,expected,32);}
static int record(const uint8_t *p,const char *magic,unsigned version){return eq(p,(const uint8_t *)magic,16)&&rd(p+16,4)==1024&&rd(p+20,2)==version&&hash(p,992,p+992);}
int pr_decode(const uint8_t *p,size_t n,pr_view *out){
 pr_view v={0};
 if(!p||!out||n!=1024||!record(p,"FORT-PKG-RLG-v1\0",1)||rd(p+22,2)||zero(p+40,64)||!zero(p+304,688))return -1;
 v.sequence=rd(p+24,8);v.epoch=rd(p+32,8);v.account_sequence=rd(p+200,8);v.credential_generation=rd(p+208,8);
 if(!v.sequence||!v.epoch||!v.account_sequence||!v.credential_generation)return -1;
 for(unsigned i=40;i<104;i+=16)if(zero(p+i,16))return -1;
 for(unsigned i=104;i<200;i+=32)if(zero(p+i,32))return -1;
 v.role=(uint16_t)rd(p+216,2);v.classification=(uint16_t)rd(p+218,2);v.flags=(uint32_t)rd(p+220,4);
 v.inode=(uint32_t)rd(p+224,4);v.generation=(uint32_t)rd(p+228,4);v.size=rd(p+232,8);
 if(!v.role||v.role>20||!v.inode||!v.generation)return -1;
 for(unsigned i=0;i<2;i++){
  v.total[i]=rd(p+240+8*i,8);v.remaining[i]=rd(p+256+8*i,8);v.charged[i]=rd(p+272+8*i,8);v.owned[i]=rd(p+288+8*i,8);
  if(v.remaining[i]>UINT64_MAX-v.charged[i]||v.total[i]!=v.remaining[i]+v.charged[i]||v.owned[i]>v.charged[i])return -1;
 }
 if(v.classification==1){if((v.flags!=1&&v.flags!=2)||v.owned[1]!=1)return -1;}
 else if(v.classification!=3||v.flags||v.size||v.owned[0]||v.owned[1])return -1;
 *out=v;return 0;
}
int pr_encode(const uint8_t *p,size_t n,uint8_t *out,size_t cap){pr_view v;if(!out||cap!=1024||pr_decode(p,n,&v))return -1;for(size_t i=0;i<n;i++)out[i]=p[i];return 0;}
static int account(const pr_set *s,const pr_binding *b,const pr_view *v){
 const uint8_t *a=s->account,*o=s->origin,*g=s->grant,*l=s->ledger;
 if(!record(a,"FORT-ACC-GEN-v1\0",4)||rd(a+22,2)||rd(a+24,8)!=v->account_sequence||!rd(a+32,8)||
    rd(a+40,8)!=v->epoch||!eq(a+48,b->grant.uuid[4],16)||!eq(a+64,b->grant.uuid[0],16)||!eq(a+80,b->account_profile,32)||
    !hash(o,1024,a+112)||rd(a+144,8)!=v->credential_generation||!zero(a+152,176)||!zero(a+436,4)||
    rd(a+440,8)!=v->sequence||!hash(g,1024,a+448)||!eq(a+480,g+232,32)||!eq(a+512,g+200,32)||!eq(a+544,g+136,32)||
    !eq(a+576,g+40,16)||!hash(l,1024,a+592)||!zero(a+624,368))return -1;
 for(unsigned i=0;i<3;i++)if(zero(a+328+32*i,32)||!rd(a+424+4*i,4)||rd(a+424+4*i,4)>8192)return -1;
 if(!record(o,"FORT-ACC-ORG-v1\0",1)||rd(o+22,2)||rd(o+24,8)!=v->account_sequence||rd(o+32,8)!=v->epoch||
    rd(o+40,8)!=rd(a+32,8)||!eq(o+48,a+48,64)||!zero(o+112,880))return -1;
 return 0;
}
int pr_validate(const pr_set *s,const pr_binding *b,pr_view *out){
 pr_view v;pg_view g;
 if(!s||!b||!out||!s->marker||!s->grant||!s->owners||!s->ledger||!s->account||!s->origin||
    s->account_n!=1024||s->origin_n!=1024||zero(b->capability,32)||zero(b->account_profile,32)||
    pr_decode(s->ledger,s->ledger_n,&v)||pg_validate_set(s->marker,s->marker_n,s->grant,s->grant_n,s->owners,s->owners_n,&b->grant,&g))return -1;
 const uint8_t *l=s->ledger,*p=NULL;
 if(g.phase!=PG_RETIRING||rd(s->grant+348,2)!=1||rd(s->grant+424,8)||v.sequence!=g.sequence||v.epoch!=g.epoch||v.inode!=b->inode||v.generation!=b->generation||v.role!=b->role||
    !eq(l+40,b->transaction,16)||!eq(s->grant+232,b->trust,32)||!eq(s->grant+200,b->state,32)||!eq(s->grant+136,b->intent,32)||
    !eq(l+40,s->grant+40,64)||!eq(l+104,b->capability,32)||!hash(s->grant,1024,l+136)||!hash(s->owners,8192,l+168))return -1;
 for(unsigned i=0;i<2;i++)if(v.total[i]!=g.total[i]||v.remaining[i]!=g.remaining[i]||v.charged[i]!=g.charged[i])return -1;
 for(unsigned i=0;i<rd(s->owners+22,2);i++){const uint8_t *row=s->owners+256+64*i;if(rd(row,2)==v.role)p=row;}
 if(!p||rd(p+2,2)!=v.classification||rd(p+4,4)!=v.inode||rd(p+8,4)!=v.generation||rd(p+12,4)!=v.flags||
    rd(p+32,8)!=v.size||rd(p+40,8)!=v.owned[0]||rd(p+48,8)!=v.owned[1]||account(s,b,&v))return -1;
 *out=v;return 0;
}
/* Equal except the enumerated mutable byte ranges. */
static int stable(const uint8_t *a,const uint8_t *b,size_t n,const uint16_t (*ranges)[2],unsigned count){
 for(size_t i=0;i<n;i++){int mutable=0;for(unsigned j=0;j<count;j++)if(i>=ranges[j][0]&&i<ranges[j][1])mutable=1;if(!mutable&&a[i]!=b[i])return 0;}return 1;
}
int pr_transition(const pr_set *a,const pr_set *b,const pr_binding *binding,const uint64_t f[2],const uint64_t next[2],pr_change *out){
 pr_view x,y;pr_change change={0};
 if(!out||!f||!next||pr_validate(a,binding,&x)||pr_validate(b,binding,&y)||!eq(a->marker,b->marker,512))return -1;
 for(unsigned i=0;i<2;i++)if(f[i]<x.remaining[i]||next[i]<y.remaining[i])return -1;
 if(eq(a->ledger,b->ledger,1024)&&eq(a->grant,b->grant,1024)&&eq(a->owners,b->owners,8192)&&eq(a->account,b->account,1024)&&eq(a->origin,b->origin,1024)){
  if(f[0]!=next[0]||f[1]!=next[1])return -1;
  change.sequence=x.sequence;*out=change;return 0;
 }
 if(x.sequence==UINT64_MAX||x.account_sequence==UINT64_MAX||y.sequence!=x.sequence+1||y.account_sequence!=x.account_sequence+1)return -1;
 static const uint16_t grant_ranges[][2]={{24,32},{264,296},{312,344},{992,1024}};
 static const uint16_t ledger_ranges[][2]={{24,32},{136,208},{218,224},{232,240},{256,304},{992,1024}};
 static const uint16_t account_ranges[][2]={{24,32},{112,144},{440,480},{592,624},{992,1024}};
 static const uint16_t origin_ranges[][2]={{24,32},{992,1024}};
 uint16_t row=0;for(unsigned i=0;i<rd(a->owners+22,2);i++)if(rd(a->owners+256+64*i,2)==x.role)row=(uint16_t)(256+64*i);
 uint16_t table_ranges[][2]={{48,56},{row,(uint16_t)(row+64)},{8160,8192}};
 if(!stable(a->grant,b->grant,1024,grant_ranges,4)||!stable(a->ledger,b->ledger,1024,ledger_ranges,6)||
    !stable(a->account,b->account,1024,account_ranges,5)||!stable(a->origin,b->origin,1024,origin_ranges,2)||
    !stable(a->owners,b->owners,8192,table_ranges,3))return -1;
 if(x.classification==1&&x.flags==1){
  if(y.classification!=1||y.flags!=2||x.size!=y.size||x.owned[0]!=y.owned[0]||x.owned[1]!=y.owned[1])return -1;
  change.operation=PR_UNLINK;
 }else if(x.classification==1&&x.flags==2){
  if(y.classification==1){if(y.flags!=2||y.size!=x.size)return -1;}
  else if(x.owned[0]||y.classification!=3)return -1;
  change.operation=PR_REFUND;
 }else return -1;
 for(unsigned i=0;i<2;i++){
  if(y.owned[i]>x.owned[i]||next[i]<f[i]||next[i]-f[i]!=x.owned[i]-y.owned[i]||
     x.total[i]!=y.total[i]||y.charged[i]>x.charged[i]||x.charged[i]-y.charged[i]!=x.owned[i]-y.owned[i]||
     y.remaining[i]<x.remaining[i]||y.remaining[i]-x.remaining[i]!=x.owned[i]-y.owned[i]||
     f[i]<x.remaining[i]||next[i]<y.remaining[i])return -1;
  change.refunded[i]=x.owned[i]-y.owned[i];
 }
 if(change.operation==PR_REFUND&&!change.refunded[0]&&!change.refunded[1])return -1;
 change.sequence=y.sequence;*out=change;return 0;
}
