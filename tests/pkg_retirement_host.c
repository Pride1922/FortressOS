#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkg_retirement_candidate.h"
#include "user/tools/digest.h"
typedef struct {uint8_t bytes[13164];pr_binding binding;pr_set set;} fixture;
static uint8_t previous_a[13164],previous_b[13164];
static unsigned validations,transitions,mixed,codec,replays,stale,byte_bit_probes;
static uint64_t rd(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static fixture *load(const char *path){
 fixture *v=calloc(1,sizeof *v);assert(v);FILE *f=fopen(path,"rb");assert(f&&fread(v->bytes,1,sizeof v->bytes,f)==sizeof v->bytes&&fgetc(f)==EOF&&!fclose(f));
 const uint8_t *p=v->bytes;memcpy(v->binding.grant.uuid,p,80);memcpy(v->binding.grant.profile,p+80,32);memcpy(v->binding.grant.marker_sha256,p+112,32);v->binding.grant.epoch=rd(p+144,8);
 for(unsigned i=0;i<3;i++){v->binding.grant.inode[i]=(uint32_t)rd(p+152+8*i,4);v->binding.grant.generation[i]=(uint32_t)rd(p+156+8*i,4);}
 memcpy(v->binding.capability,p+176,32);memcpy(v->binding.account_profile,p+208,32);v->binding.role=(uint16_t)rd(p+240,2);v->binding.inode=(uint32_t)rd(p+244,4);v->binding.generation=(uint32_t)rd(p+248,4);
 memcpy(v->binding.transaction,p+252,16);memcpy(v->binding.trust,p+268,32);memcpy(v->binding.state,p+300,32);memcpy(v->binding.intent,p+332,32);
 p+=364;v->set=(pr_set){p,p+512,p+1536,p+9728,p+10752,p+11776,512,1024,8192,1024,1024,1024};return v;
}
static void seal(uint8_t *p){digest_t c;digest_init(&c,DIGEST_SHA256);assert(digest_update(&c,p,992));digest_final(&c,p+992);}
static void decode_reject(const uint8_t *p,size_t n){pr_view out,saved;memset(&saved,0xa5,sizeof saved);out=saved;assert(pr_decode(p,n,&out)&&!memcmp(&saved,&out,sizeof out));codec++;}
static void codec_checks(fixture *f){
 pr_view v;assert(!pr_decode(f->set.ledger,1024,&v));uint8_t b[1025],encoded[1025];memcpy(b,f->set.ledger,1024);b[1024]=0;
 for(unsigned n=0;n<1024;n++){decode_reject(b,n);}decode_reject(b,1025);
 for(unsigned i=0;i<1024;i++){b[i]^=1;decode_reject(b,1024);b[i]^=1;}
 for(unsigned i=304;i<992;i++){b[i]=1;seal(b);decode_reject(b,1024);b[i]=0;}
 memcpy(b,f->set.ledger,1024);memset(encoded,0x5a,sizeof encoded);assert(!pr_encode(b,1024,encoded,1024)&&!memcmp(b,encoded,1024)&&encoded[1024]==0x5a);
 for(unsigned n=0;n<1024;n++){memset(encoded,0x5a,sizeof encoded);assert(pr_encode(b,1024,encoded,n));for(unsigned j=0;j<sizeof encoded;j++)assert(encoded[j]==0x5a);codec++;}
 assert(pr_encode(b,1024,encoded,1025));decode_reject(NULL,1024);
 pr_set short_set=f->set;size_t *lengths[]={&short_set.marker_n,&short_set.grant_n,&short_set.owners_n,&short_set.ledger_n,&short_set.account_n,&short_set.origin_n};
 for(unsigned i=0;i<6;i++){size_t actual=*lengths[i];for(unsigned mode=0;mode<3;mode++){*lengths[i]=mode==0?0:mode==1?actual-1:actual+1;pr_view out,saved;memset(&saved,0xa5,sizeof saved);out=saved;assert(pr_validate(&short_set,&f->binding,&out)&&!memcmp(&saved,&out,sizeof out));codec++;}*lengths[i]=actual;}
 const uint8_t *parts[]={f->set.grant,f->set.owners,f->set.ledger,f->set.account,f->set.origin};size_t sizes[]={1024,8192,1024,1024,1024};
 for(unsigned part=0;part<5;part++)for(size_t i=0;i<sizes[part];i++){
  uint8_t *p=f->bytes+(parts[part]-f->bytes)+i;*p^=1;pr_view out,saved;memset(&saved,0xa5,sizeof saved);memcpy(&out,&saved,sizeof out);
  assert(pr_validate(&f->set,&f->binding,&out)&&!memcmp(&saved,&out,sizeof out));*p^=1;byte_bit_probes++;
 }
}
static void valid_case(fixture *f,int expected){pr_view out,saved;memcpy(previous_a,f->bytes,sizeof previous_a);memset(&saved,0xa5,sizeof saved);out=saved;int r=pr_validate(&f->set,&f->binding,&out);assert((r==0)==expected);if(r)assert(!memcmp(&out,&saved,sizeof out));assert(!memcmp(previous_a,f->bytes,sizeof previous_a));validations++;}
static void transition_case(fixture *a,fixture *b,int expected,unsigned operation,uint64_t f[2],uint64_t next[2]){
 pr_view av,bv;assert(!pr_validate(&a->set,&a->binding,&av)&&!pr_validate(&b->set,&a->binding,&bv));
 memcpy(previous_a,a->bytes,sizeof previous_a);memcpy(previous_b,b->bytes,sizeof previous_b);
 pr_change out,saved;memset(&saved,0xa5,sizeof saved);memcpy(&out,&saved,sizeof out);int r=pr_transition(&a->set,&b->set,&a->binding,f,next,&out);
 if((r==0)!=expected)fprintf(stderr,"unexpected transition status=%d expected=%d\n",r,expected);
 assert((r==0)==expected);if(r)assert(!memcmp(&out,&saved,sizeof out));else{
  assert(out.operation==operation);for(unsigned i=0;i<2;i++){assert(out.refunded[i]==next[i]-f[i]);assert(next[i]-bv.remaining[i]==f[i]-av.remaining[i]);}
 }assert(!memcmp(previous_a,a->bytes,sizeof previous_a)&&!memcmp(previous_b,b->bytes,sizeof previous_b));transitions++;
}
static int publish(pr_set *current,const pr_set *expected,const pr_set *next,const pr_binding *binding,uint64_t f[2],const uint64_t planned[2]){
 if(memcmp(current->grant,expected->grant,1024)||memcmp(current->owners,expected->owners,8192)||memcmp(current->ledger,expected->ledger,1024)||memcmp(current->account,expected->account,1024)||memcmp(current->origin,expected->origin,1024))return -1;
 pr_change change;if(pr_transition(expected,next,binding,f,planned,&change))return -1;
 *current=*next;memcpy(f,planned,2*sizeof *f);return 0;
}
static void mixes(fixture *a,fixture *b){
 for(unsigned mask=0;mask<32;mask++){
  pr_set s=a->set;if(mask&1)s.grant=b->set.grant;if(mask&2)s.owners=b->set.owners;if(mask&4)s.ledger=b->set.ledger;if(mask&8)s.account=b->set.account;if(mask&16)s.origin=b->set.origin;
  pr_view out,saved;memset(&saved,0xa5,sizeof saved);out=saved;int r=pr_validate(&s,&a->binding,&out);assert((r==0)==(mask==0||mask==31));if(r)assert(!memcmp(&out,&saved,sizeof out));mixed++;
 }
 /* Mock expected-generation exclusion: publish the complete set once. No
  * actual journal, filesystem write or power-loss behavior is modeled. */
 pr_view old,newer;assert(!pr_validate(&a->set,&a->binding,&old)&&!pr_validate(&b->set,&a->binding,&newer));
 uint64_t current_free[2]={100,100},planned[2]={100+old.owned[0]-newer.owned[0],100+old.owned[1]-newer.owned[1]};pr_set current=a->set;
 assert(!publish(&current,&a->set,&b->set,&a->binding,current_free,planned));
 assert(publish(&current,&a->set,&b->set,&a->binding,current_free,planned)&&current.ledger==b->set.ledger&&current_free[0]==planned[0]&&current_free[1]==planned[1]);stale++;
 uint64_t f[2]={100,100};pr_change replay;assert(!pr_transition(&b->set,&b->set,&a->binding,f,f,&replay)&&replay.operation==PR_REPLAY&&!replay.refunded[0]&&!replay.refunded[1]);replays++;
}
int main(int argc,char **argv){
 assert(argc==2);FILE *index=fopen(argv[1],"r");assert(index);char kind,label[128],ap[256],bp[256];unsigned expected,operation;unsigned long long f0,f1,n0,n1;
 while(fscanf(index," %c",&kind)==1){
  if(kind=='V'){assert(fscanf(index,"%u %127s %255s",&expected,label,ap)==3);fixture *a=load(ap);valid_case(a,(int)expected);if(!strcmp(label,"initial"))codec_checks(a);free(a);}
  else {assert(fscanf(index,"%u %u %llu %llu %llu %llu %127s %255s %255s",&expected,&operation,&f0,&f1,&n0,&n1,label,ap,bp)==9);fixture *a=load(ap),*b=load(bp);uint64_t f[2]={f0,f1},next[2]={n0,n1};transition_case(a,b,(int)expected,operation,f,next);if(kind=='M')mixes(a,b);free(a);free(b);}
 }assert(!ferror(index)&&!fclose(index));printf("PASS validate=%u transitions=%u mixed=%u codec=%u byte_bit_probes=%u replay=%u stale=%u\n",validations,transitions,mixed,codec,byte_bit_probes,replays,stale);return 0;
}
