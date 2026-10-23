#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkg_retirement_auth.h"
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c){(void)nr;(void)a;(void)b;(void)c;assert(!"unexpected host syscall");return -1;}
static pa_workspace work;
static pr_binding admitted;
static uint64_t rd(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static uint8_t *load(const char *path,size_t *n){
 FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long len=ftell(f);assert(len>0&&len<65536);rewind(f);
 uint8_t *p=malloc((size_t)len);assert(p&&fread(p,1,(size_t)len,f)==(size_t)len&&!fclose(f));*n=(size_t)len;return p;
}
int main(int argc,char **argv){
 assert(argc==2);FILE *index=fopen(argv[1],"r");assert(index);char path[256];unsigned expected,cases=0,accepted=0;
 while(fscanf(index,"%u %255s",&expected,path)==2){
  size_t n;uint8_t *data=load(path,&n);assert(n>=1056+16+512+1024*7+8192);
  pe_policy en={0};fpk_catalog_policy catalog={0};pa_policy policy={0};
  memcpy(en.keys,data,256);memcpy(en.uuid,data+256,80);memcpy(en.descriptor_sha256,data+336,32);
  en.key_count=(unsigned)rd(data+368,4);en.threshold=(unsigned)rd(data+372,4);en.epoch=rd(data+376,8);
  const uint8_t *cp=data+384;memcpy(catalog.repository,cp,16);catalog.root_version=rd(cp+16,8);catalog.revision_floor=rd(cp+24,8);catalog.now=rd(cp+32,8);
  memcpy(catalog.floor_sha256,cp+40,32);catalog.threshold=(unsigned)rd(cp+72,4);catalog.key_count=(unsigned)rd(cp+76,4);memcpy(catalog.keys,cp+80,256);
  policy.enrollment=&en;policy.catalog=&catalog;policy.platform=cp+336;policy.base_version=rd(cp+368,8);
  const uint8_t *rp=data+760;memcpy(policy.keys,rp,256);policy.key_count=(unsigned)rd(rp+256,4);policy.threshold=(unsigned)rd(rp+260,4);memcpy(policy.certificate_sha256,rp+264,32);
  const uint8_t *h=data+1056,*p=h+16;size_t lens[4];for(unsigned i=0;i<4;i++)lens[i]=(size_t)rd(h+4*i,4);
  assert(lens[0]<=PE_MAX&&lens[1]<=PA_MAX+1&&lens[2]<=8192&&lens[3]<=8192);
  assert(n==1056+16+512+1024*7+8192+lens[0]+lens[1]+lens[2]+lens[3]);
  pa_input in={0};pr_set *s=&in.records;s->marker=p;s->marker_n=512;p+=512;
  s->grant=p;s->grant_n=1024;p+=1024;s->owners=p;s->owners_n=8192;p+=8192;
  s->ledger=p;s->ledger_n=1024;p+=1024;s->account=p;s->account_n=1024;p+=1024;s->origin=p;s->origin_n=1024;p+=1024;
  in.state=p;p+=1024;in.trust=p;p+=1024;in.intent=p;p+=1024;
  in.enrollment=p;in.enrollment_n=lens[0];p+=lens[0];in.certificate=p;in.certificate_n=lens[1];p+=lens[1];
  in.artifact=p;in.artifact_n=lens[2];p+=lens[2];in.executable=p;in.executable_n=lens[3];
  pa_result result,before;memset(&result,0xa5,sizeof result);memcpy(&before,&result,sizeof result);
  work.mount.verify.verified=1;
  if(strstr(path,"coherent-forged-catalog")||strstr(path,"authorized-unsupported-manifest")){
   pr_binding candidate=admitted;pr_view v;pe_enrollment enrollment;
   memcpy(candidate.capability,policy.certificate_sha256,32);memcpy(candidate.trust,in.certificate+256,32);
   memcpy(candidate.state,in.certificate+288,32);memcpy(candidate.intent,in.certificate+320,32);
   assert(!pe_verify(in.enrollment,in.enrollment_n,&en,&enrollment));
   assert(!pr_validate(&in.records,&candidate,&v));
  }
  if(strstr(path,"valid-records-")){pr_view v;assert(!pr_validate(&in.records,&admitted,&v));}
  int r=pa_verify(&in,&policy,&work,&result);
  if((r==0)!=expected){fprintf(stderr,"case=%u file=%s got=%d expected=%u verified=%d\n",cases,path,r,expected,work.mount.verify.verified);return 1;}
  if(r){assert(!memcmp(&result,&before,sizeof result));assert(!work.mount.verify.verified);}
  else {assert(work.mount.verify.verified);assert(result.binding.role==1);accepted++;if(!cases)admitted=result.binding;}
  assert(pa_verify(NULL,&policy,&work,&result)<0&&!work.mount.verify.verified);
  free(data);cases++;
 }
 assert(!ferror(index)&&!fclose(index));printf("PASS provenance cases=%u accepted=%u rejected=%u preserved-failure-output verified-cleared\n",cases,accepted,cases-accepted);return 0;
}
