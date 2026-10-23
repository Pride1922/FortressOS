#include "pkg_mount_candidate.h"
#include "user/tools/digest.h"
static uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static int same(const uint8_t *a,const uint8_t *b,size_t n){for(size_t i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1;}
static int zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return 0;return 1;}
static void hash(const void *p,size_t n,uint8_t h[32]){digest_t d;digest_init(&d,DIGEST_SHA256);(void)digest_update(&d,p,n);digest_final(&d,h);}
static int matches(const void *p,size_t n,const uint8_t *h){uint8_t b[32];hash(p,n,b);return same(b,h,32);}
static int record(const uint8_t *p,const char *magic){return p&&same(p,(const uint8_t *)magic,16)&&get(p+16,4)==1024&&get(p+20,2)==1&&matches(p,992,p+992);}
static int state(const uint8_t *s,const pm_input *in){
 if(!record(s,"FORT-PKG-ST-v1\0\0")||get(s+22,2)||!get(s+24,8)||!same(s+32,in->policy->repository,16)||!matches(in->trust,1024,s+48)||!zero(s+226,766))return 0;
 uint64_t gen=get(s+24,8),build=get(s+112,8);
 if(gen==1?!zero(s+80,32):zero(s+80,32))return 0;
 if(!build)return gen==1&&zero(s+120,106);
 unsigned n=s[192];return gen==2&&n==4&&same(s+193,(const uint8_t *)"demo",4)&&fpk_identity_valid(s+193,n)&&zero(s+193+n,33-n)&&get(s+184,8)&&!zero(s+120,32)&&!zero(s+152,32);
}
/* This install-only profile has one explicit canonical empty generation.
 * Validate consumed-source lineage without inventing a missing old inventory. */
static void initial_state(const pm_input *in,uint8_t s[1024]){
 static const uint8_t domain[16]="FORT-PKG-ST-v1";
 for(unsigned i=0;i<1024;i++)s[i]=0;
 for(unsigned i=0;i<16;i++){s[i]=domain[i];s[32+i]=in->policy->repository[i];}
 s[17]=4;s[20]=1;s[24]=1;hash(in->trust,1024,s+48);hash(s,992,s+992);
}
typedef struct {const uint8_t *p;size_t n,at;} stream;
static long read_bytes(void *v,void *b,size_t n){stream *s=v;if(n>s->n-s->at)n=s->n-s->at;for(size_t i=0;i<n;i++)((uint8_t *)b)[i]=s->p[s->at+i];s->at+=n;return (long)n;}
int pm_validate(const pm_input *in,pm_workspace *workspace,pm_view *out){
 fpk_verify_context *ctx=workspace?&workspace->verify:NULL;if(ctx)ctx->verified=0;
 if(!in||!ctx||!out||!in->enrollment||!in->enrollment->wire||in->enrollment->length<PE_BODY||in->enrollment->length>PE_MAX||!in->policy||!in->platform||!in->base_version||zero(in->platform,32)||(in->artifact_size&&!in->artifact)||in->artifact_size>8192||!in->account||!in->marker||!in->ledger||!in->owners||!in->state||!in->trust)return -1;
 const uint8_t *en=in->enrollment->wire,*l=in->ledger,*t=in->trust,*a=in->account;
 pg_binding b={0};for(unsigned i=0;i<5;i++)for(unsigned j=0;j<16;j++)b.uuid[i][j]=en[40+16*i+j];
 for(unsigned j=0;j<32;j++){b.profile[j]=en[152+j];b.marker_sha256[j]=en[120+j];}
 b.epoch=get(en+24,8);for(unsigned i=0;i<3;i++){b.inode[i]=(uint32_t)get(en+416+128*(i+1)+4,4);b.generation[i]=(uint32_t)get(en+416+128*(i+1)+8,4);}
 uint8_t base[40];for(unsigned i=0;i<32;i++)base[i]=in->platform[i];for(unsigned i=0;i<8;i++)base[32+i]=(uint8_t)(in->base_version>>(8*i));if(!matches(base,40,en+280))return -1;
 pg_view v;if(pg_validate_set(in->marker,512,l,1024,in->owners,8192,&b,&v)||v.sequence==UINT64_MAX||get(l+424,8)||(v.phase!=PG_IDLE&&get(l+348,2)!=1)||!same(in->policy->repository,en+88,16))return -1;
 if(!record(t,"FORT-PKG-TR-v1\0\0")||get(t+22,2)||get(t+24,8)!=in->policy->root_version||get(t+32,8)!=in->policy->revision_floor||!same(t+48,b.uuid[3],16)||!zero(t+100,156))return -1;
 size_t cn=(size_t)get(t+96,4);if(cn<72||cn>736||!zero(t+256+cn,736-cn)||!matches(t+256,cn,t+64)||!same(t+64,in->policy->floor_sha256,32)||!matches(t,1024,en+248))return -1;
 fpk_catalog_policy *policy=&workspace->policy;*policy=*in->policy;uint64_t accepted=get(t+40,8);if(!accepted||accepted>policy->now)return -1;
 policy->now=accepted;fpk_catalog catalog;if(fpk_catalog_verify(t+256,cn,policy,&catalog))return -1;
 initial_state(in,workspace->initial_state);
 if(!state(in->state,in)||!matches(t,1024,l+232)||get(l+376,8)!=policy->root_version||get(l+384,8)!=catalog.revision)return -1;
 if(get(a+20,2)!=3||get(a+440,8)!=v.sequence||!matches(l,1024,a+448)||!matches(t,1024,a+480)||!matches(in->state,1024,a+512)||!same(a+576,l+40,16)||!zero(a+436,4)||!zero(a+592,400))return -1;
 if(in->pending_size){if(in->pending_size!=1024||!in->pending||!matches(in->pending,1024,a+544))return -1;}else if(!zero(a+544,32))return -1;
 uint64_t sg=get(in->state+24,8);unsigned installed=get(in->state+112,8)!=0;
 if(installed&&!matches(workspace->initial_state,1024,in->state+80))return -1;
 const uint8_t *verified_state=installed?in->state:NULL;
 if(v.phase==PG_IDLE){if(in->pending_size||in->next_size||(!installed&&in->artifact_size)||!matches(in->state,1024,l+168)||sg!=get(l+360,8))return -1;}
 else{
  const uint8_t *next=in->next,*p=in->pending;verified_state=next;
  if(get(l+360,8)!=1||!matches(workspace->initial_state,1024,l+168))return -1;
  if(in->next_size!=1024||!next||!state(next,in)||!get(next+112,8)||get(l+360,8)==UINT64_MAX||get(next+24,8)!=get(l+360,8)+1)return -1;
  if(!same(next+80,l+168,32)||!matches(in->artifact,in->artifact_size,next+120)||!same(next+193,(const uint8_t *)"demo",4)||next[192]!=4)return -1;
  uint8_t plan[96],ph[32];hash(next,1024,plan);hash(in->artifact,in->artifact_size,plan+32);for(unsigned i=0;i<32;i++)plan[64+i]=b.profile[i];hash(plan,96,ph);if(!same(ph,l+392,32))return -1;
  if(v.phase==PG_RESERVED){if(in->pending_size||sg!=get(l+360,8)||!matches(in->state,1024,l+168))return -1;policy->now=in->policy->now;}
  else{
   if(!matches(next,1024,l+200)||get(l+368,8)!=get(next+24,8))return -1;
   {
    if(in->pending_size!=1024||!record(p,"FORT-PKG-PN-v1\0\0")||get(p+22,2)!=1||!same(p+24,l+40,16)||!same(p+40,b.uuid[4],16)||!same(p+56,b.uuid[3],16)||!same(p+72,l+168,32)||!same(p+104,l+200,32)||!same(p+136,l+232,32)||!same(p+168,next+120,32)||!same(p+200,ph,32)||get(p+232,8)!=get(l+360,8)||get(p+240,8)!=get(l+368,8)||get(p+248,8)!=policy->root_version||get(p+256,8)!=catalog.revision||!get(p+264,8)||get(p+264,8)<accepted||get(p+264,8)>in->policy->now||get(p+272,8)!=in->artifact_size||get(p+280,8)!=get(next+112,8)||p[288]!=4||!same(p+289,(const uint8_t *)"demo",4)||!zero(p+293,35)||!same(p+328,next+152,32)||get(p+360,8)!=get(next+184,8)||!zero(p+368,624)||!matches(p,1024,l+136))return -1;
    policy->now=get(p+264,8);
   }
   if(matches(in->state,1024,l+168)){if(sg!=get(l+360,8)||v.phase==PG_RETIRING)return -1;}
   else if(!matches(in->state,1024,l+200)||sg!=get(l+368,8))return -1;
  }
 }
 if(verified_state){const uint8_t *next=verified_state;
  stream stream={in->artifact,in->artifact_size,0};
  if(!matches(in->artifact,in->artifact_size,next+120)||fpk_verify_authorized(ctx,t+256,cn,policy,(const uint8_t *)"demo",4,read_bytes,&stream)||ctx->manifest.file_count!=1||ctx->manifest.command.length!=4||!same(ctx->manifest.command.bytes,(const uint8_t *)"demo",4)||ctx->manifest.build!=get(next+112,8)||ctx->manifest.base_min>in->base_version||ctx->manifest.base_max<in->base_version||!same(ctx->manifest.platform_id,in->platform,32)||ctx->manifest.files[0].size!=get(next+184,8)||!same(ctx->manifest.files[0].sha256,next+152,32)){ctx->verified=0;return -1;}
 }
 pm_view result={v,sg,installed};*out=result;return 0;
}
