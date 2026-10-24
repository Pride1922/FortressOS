#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkg_selector_candidate.h"
_Static_assert(sizeof(ps_policy)==1136,"test policy native x86-64 harness size");
static ps_policy policy;
static ps_workspace workspace;
static unsigned char selector[PS_MAX+1],bundle[PS_BUNDLE_MAX+1];
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c){(void)nr;(void)a;(void)b;(void)c;assert(!"unexpected verifier I/O");return -1;}
static size_t load(const char *path,void *out,size_t cap){FILE *f=fopen(path,"rb");assert(f);size_t n=fread(out,1,cap,f);assert(!ferror(f)&&fgetc(f)==EOF);assert(!fclose(f));return n;}
int main(int argc,char **argv){
 assert(argc==5);size_t pn=load(argv[1],&policy,sizeof policy+0),sn=load(argv[2],selector,sizeof selector),bn=load(argv[3],bundle,sizeof bundle);
 assert(pn==sizeof policy);ps_result out,old;memset(&out,0xa5,sizeof out);old=out;
 int r=ps_validate(selector,sn,bundle,bn,&policy,&workspace,&out);
 bool expected=argv[4][0]=='1';if(expected!=(!r)){fprintf(stderr,"selector result=%d expected=%d\n",r,expected);return 1;}
 if(r)assert(!memcmp(&out,&old,sizeof out));
 else assert(out.epoch&&out.epoch!=UINT64_MAX);
 for(unsigned required=0;required<2;required++)for(unsigned state=0;state<5;state++)for(unsigned valid=0;valid<2;valid++){
  unsigned want=state==PS_ABSENT?(required?PS_REFUSE:PS_LEGACY):state==PS_PRESENT&&required&&valid?PS_PROTECTED:PS_REFUSE;
  assert(ps_admission(required,state,valid)==want);
 }
 /* Pointer/overlap rejection before writes, independent of envelope validity. */
 assert(ps_validate(NULL,sn,bundle,bn,&policy,&workspace,&out));
 assert(ps_validate(selector,sn,bundle,bn,&policy,&workspace,(void *)&workspace));
 assert(ps_validate(selector,sn,bundle,bn,&policy,(void *)&policy,&out));
 puts(r?"PASS selector refusal output-preserved":"PASS selector authenticated initial-IDLE");return 0;
}
