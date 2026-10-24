#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkg_pending_candidate.h"
static pp_workspace workspace;
static pp_change change;
static ps_policy policy;
static uint8_t selector[PS_MAX+1],baseline[PS_BUNDLE_MAX+1],current[PS_BUNDLE_MAX+1],artifact[8193],intent[1025];
static pp_object objects[4],executable;
long tool_syscall(long n,uintptr_t a,uintptr_t b,uintptr_t c){(void)n;(void)a;(void)b;(void)c;assert(!"unexpected verifier I/O");return -1;}
static size_t read_file(const char *path,void *out,size_t max){FILE *f=fopen(path,"rb");assert(f);size_t n=fread(out,1,max,f);assert(!ferror(f)&&fgetc(f)==EOF);assert(!fclose(f));return n;}
int main(int argc,char **argv){
    assert(argc==11 && sizeof(pp_object)==144);assert(read_file(argv[1],&policy,sizeof policy)==sizeof policy);
    pp_input in={.authority={.policy=&policy,.grant_floor=1,.state_floor=1},.objects=objects};
    in.authority.selector=(ps_blob){selector,read_file(argv[2],selector,sizeof selector)};
    in.authority.baseline=(ps_blob){baseline,read_file(argv[3],baseline,sizeof baseline)};
    in.authority.current=(ps_blob){current,read_file(argv[4],current,sizeof current)};
    in.authority.artifact=(ps_blob){artifact,read_file(argv[5],artifact,sizeof artifact)};
    in.intent=(ps_blob){intent,read_file(argv[6],intent,sizeof intent)};
    size_t n=read_file(argv[7],objects,sizeof objects);assert(!(n%sizeof(*objects)));in.object_count=n/sizeof(*objects);
    if(atoi(argv[8])){for(unsigned i=0;i<in.object_count;i++)if(objects[i].role==17)executable=objects[i];in.executable=&executable;}
    /* Independent native free/capacity observation is supplied by the harness. */
    FILE *f=fopen(argv[9],"rb");assert(f&&fread(in.free,8,2,f)==2&&fread(in.capacity,8,2,f)==2&&!fclose(f));
    pp_view view,old;memset(&view,0xa5,sizeof view);old=view;int r=pp_validate(&in,&workspace,&view);bool expected=atoi(argv[10]);
    if(expected!=(!r)){fprintf(stderr,"pending validation r=%d expected=%u\n",r,expected);return 1;}
    if(r){assert(!memcmp(&old,&view,sizeof view));assert(!workspace.authority.verified&&!workspace.mount.verify.verified);puts("PASS pending refusal preserved output");return 0;}
    unsigned steps=0;
    while(view.action!=PP_DONE){
        assert(++steps<=8);pg_budget before=view.budget;assert(!pp_next(&in,&workspace,&view,&change));
        if(view.action==PP_DELETE || (view.action==PP_FINISH&&view.role)){
            unsigned i=0;while(i<in.object_count&&objects[i].role!=view.role)i++;assert(i<in.object_count);
            in.free[0]+=objects[i].blocks;in.free[1]++;memmove(objects+i,objects+i+1,(in.object_count-i-1)*sizeof *objects);in.object_count--;
        }
        assert(change.free_after[0]==in.free[0]&&change.free_after[1]==in.free[1]);
        if(view.action==PP_ACTIVATE){for(unsigned i=0;i<in.object_count;i++)if(objects[i].role==17)executable=objects[i];in.executable=&executable;}
        size_t at=96;for(unsigned i=0;i<8;i++){
            const uint8_t *p=i==2?change.grant:i==3?change.owners:i==4?change.account:i==6?change.state:workspace.parts[i].p;
            memmove(current+at,p,workspace.parts[i].n);at+=workspace.parts[i].n;
        }
        if(view.action==PP_FINISH){in.intent.n=0;in.object_count=0;}
        assert(!pp_validate(&in,&workspace,&view));assert(view.sequence==before.sequence+1);
        if(change.action==PP_FINISH&&view.installed){assert(view.budget.free[0]==before.free[0]+1);assert(view.budget.baseline[0]==before.baseline[0]+executable.blocks);}
        if(view.action==PP_DONE)assert(pp_next(&in,&workspace,&view,&change));
    }
    assert(!view.budget.total[0]&&!view.budget.total[1]);
    assert(pp_validate(NULL,&workspace,&view));assert(pp_validate(&in,&workspace,(void *)&workspace));assert(pp_validate(&in,(void *)&policy,&view));
    printf("PASS pending recovery model steps=%u sequence=%llu installed=%u\n",steps,(unsigned long long)view.sequence,view.installed);return 0;
}
