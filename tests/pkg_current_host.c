#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkg_current_candidate.h"
static pc_workspace workspace;
static ps_policy policy;
static uint8_t selector[PS_MAX+1],baseline[PS_BUNDLE_MAX+1],current[PS_BUNDLE_MAX+1],artifact[8193],executable[8193];
long tool_syscall(long n,uintptr_t a,uintptr_t b,uintptr_t c){(void)n;(void)a;(void)b;(void)c;assert(!"unexpected verifier I/O");return -1;}
static size_t read_file(const char *path,void *out,size_t max){FILE *f=fopen(path,"rb");assert(f);size_t n=fread(out,1,max,f);assert(!ferror(f)&&fgetc(f)==EOF);assert(!fclose(f));return n;}
int main(int argc,char **argv){
    assert(argc==10);assert(read_file(argv[1],&policy,sizeof policy)==sizeof policy);
    pc_input in={.policy=&policy,.grant_floor=strtoull(argv[7],NULL,10),.state_floor=strtoull(argv[8],NULL,10)};
    in.selector=(ps_blob){selector,read_file(argv[2],selector,sizeof selector)};
    in.baseline=(ps_blob){baseline,read_file(argv[3],baseline,sizeof baseline)};
    in.current=(ps_blob){current,read_file(argv[4],current,sizeof current)};
    in.artifact=(ps_blob){artifact,read_file(argv[5],artifact,sizeof artifact)};
    in.executable=(ps_blob){executable,read_file(argv[6],executable,sizeof executable)};
    pc_result result,old;memset(&result,0xa5,sizeof result);old=result;
    int r=pc_validate(&in,&workspace,&result);bool expected=argv[9][0]=='1';
    if(expected!=(!r)){fprintf(stderr,"current r=%d expected=%u\n",r,expected);return 1;}
    if(r){assert(!memcmp(&result,&old,sizeof result));assert(!workspace.current.verify.verified);}
    else {assert(result.grant_sequence>=in.grant_floor && result.state_generation>=in.state_floor);assert(result.installed==(result.state_generation==2));}
    assert(pc_validate(NULL,&workspace,&result));
    assert(pc_validate(&in,&workspace,(void *)&workspace));
    assert(pc_validate(&in,(void *)&policy,&result));
    puts(r?"PASS current refusal output-preserved":"PASS baseline and authenticated current endpoint");return 0;
}
