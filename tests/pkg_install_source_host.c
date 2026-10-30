/* Actual normal source-install frontend; syscall model is not disk evidence. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/pkg/system_cli.h"
#include "user/pkg/mutation_cli.h"
#include "user/tools/common.h"
#include "pkg_abi.h"
bool tool_output_failed;
static unsigned acquired,released,installed,opened,closed;
static bool denied,oversize,bad_close,bad_result,fail_install,bad_release;
static const uint8_t catalog[73]={1,2,3},artifact[129]={4,5,6};
size_t tool_length(const char *s){return strlen(s);}
bool tool_equal(const char *a,const char *b){return !strcmp(a,b);}
int tool_error(const char *a,const char *b,const char *c){(void)a;(void)b;(void)c;return 1;}
int tool_write(const char *t,const void *p,size_t n){(void)t;(void)p;(void)n;return 0;}
int pkg_fetch_main(int n,char **v){(void)n;(void)v;assert(0);return 1;}
int pkg_mutation_cli(int n,char **v,const fpk_catalog_policy *p,const pkg_mutation_backend *b){
    (void)n;(void)v;(void)p;(void)b;return 2;
}
static void result(pkg_result_v1 *r,uint64_t session){
    *r=(pkg_result_v1){.size=80,.version=1,.session=session,.sequence=8,
        .account_sequence=2,.credential_generation=3};
}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c){
    if(nr==SYS_PKGCTL){
        const pkg_request_v1 *q=(const void *)a;pkg_result_v1 *r=(void *)b;
        if(q->operation==PKG_ACQUIRE){
            assert(c==64&&q->version==1&&!q->session);acquired++;
            if(denied)return SYSCALL_EOPNOTSUPP;
            result(r,42);return 0;
        }
        if(q->operation==PKG_RELEASE){
            assert(c==64&&q->version==1&&q->session==42);released++;
            result(r,0);return bad_release?SYSCALL_EIO:0;
        }
        const pkg_request_v2 *s=(const void *)a;
        assert(c==96&&q->size==96&&q->version==2&&q->operation==PKG_INSTALL_SOURCE&&q->session==42);
        assert(!memcmp(q->package,"fortress-desktop",16)&&!q->reserved&&!q->reserved64);
        assert(s->catalog_bytes==sizeof catalog&&s->artifact_bytes==sizeof artifact);
        assert(s->catalog_fd==10&&s->artifact_fd==11);
        installed++;result(r,42);if(bad_result)r->reserved=1;
        return fail_install?SYSCALL_EIO:0;
    }
    if(nr==SYS_OPEN){
        assert(b==(VFS_O_RDONLY|VFS_O_NOFOLLOW));const char *p=(const void *)a;
        unsigned i=!strcmp(p,"/staging/catalog.v1")?0:1;
        assert(!i||!strcmp(p,"/staging/artifact.fpk"));opened++;return 10+i;
    }
    assert(a==10||a==11);unsigned i=a-10;size_t size=i?sizeof artifact:sizeof catalog;
    if(nr==SYS_FSTAT){
        stat_ext_v1_t *st=(void *)b;
        *st=(stat_ext_v1_t){.size=sizeof *st,.version=1,.type=VFS_FILE,
            .file_size=oversize?PKG_SOURCE_ARTIFACT_MAX+1:size};return 0;
    }
    assert(nr==SYS_CLOSE);closed++;return bad_close?SYSCALL_EIO:0;
}
static int run(const char *name,const char *path){
    char *argv[]={"pkg","install",(char *)name,(char *)path};return pkg_system_main(4,argv);
}
int main(void){
    assert(run("../bad","/staging")&&!acquired&&!installed);
    denied=true;assert(run("fortress-desktop","/staging")&&acquired==1&&!opened&&!released);denied=false;
    assert(!run("fortress-desktop","/staging")&&installed==1&&released==1&&opened==closed);
    oversize=true;assert(run("fortress-desktop","/staging")&&installed==1&&opened==closed);oversize=false;
    bad_close=true;assert(run("fortress-desktop","/staging")&&installed==2&&opened==closed);bad_close=false;
    fail_install=true;assert(run("fortress-desktop","/staging")&&installed==3);fail_install=false;
    bad_result=true;assert(run("fortress-desktop","/staging")&&installed==4);bad_result=false;
    bad_release=true;assert(run("fortress-desktop","/staging")&&installed==5);bad_release=false;
    assert(run("fortress-desktop","relative")&&installed==5);
    assert(opened==closed&&acquired==released+1);
    puts("PASS source install frontend: bounded opened sources, session/descriptor cleanup, one ingestion call, no mutation retry");
}
