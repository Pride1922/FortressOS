/* Actual CLI batch control; catalog/network/inventory/syscalls are mocked here.
 * Disk durability and parsers are covered by the separate actual-code suites. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/pkg/repository.h"
#include "user/pkg/system_cli.h"
#include "user/pkg/mutation_cli.h"
#include "user/tools/common.h"
#include "pkg_abi.h"
#include "terminal.h"
static pkg_installed installed[PKG_INSTALLED_MAX];
static unsigned refreshes,downloads,mutations,releases;static bool session,interactive,current,stale;
static int fail_download=-1;static const char *answer="yes\n";static char output[32768];static size_t used;
int pkg_installed_read(pkg_installed out[PKG_INSTALLED_MAX],unsigned *n){memcpy(out,installed,3*sizeof *out);*n=3;return 0;}
int pkg_catalog_refresh(void){assert(!session);refreshes++;return 0;}
int pkg_catalog_target(const char *name,uint64_t *build,uint8_t hash[32]){(void)name;*build=current?1:2;memset(hash,current?1:2,32);return 0;}
int pkg_download_selected(const char *name){assert(!session);downloads++;if(stale)installed[0].identity[0]++;return name[0]==fail_download;}
const char *pkg_selected_directory(void){return "/cache";}
int pkg_previous_source(const char *name,uint64_t *bytes){(void)name;assert(!session);*bytes=128;return 12;}
int pkg_fetch_main(int n,char **v){(void)n;(void)v;assert(0);return 1;}
int pkg_update_main(int n,char **v){(void)n;(void)v;assert(0);return 1;}
int pkg_get_main(int n,char **v){(void)n;(void)v;assert(0);return 1;}
int pkg_mutation_cli(int n,char **v,const fpk_catalog_policy *p,const pkg_mutation_backend *b){(void)n;(void)v;(void)p;(void)b;assert(0);return 1;}
static void result(pkg_result_v1 *r,uint64_t token,uint64_t sequence){*r=(pkg_result_v1){.size=sizeof *r,.version=1,.session=token,.sequence=sequence,.account_sequence=1,.credential_generation=1};}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c){
    if(nr==SYS_WRITE){assert(a==1||a==2);assert(c<sizeof output-used);memcpy(output+used,(void *)b,c);used+=c;output[used]=0;return c;}
    if(nr==SYS_TERMCTL){assert(a==TERM_ISATTY);return interactive;}
    if(nr==SYS_READ){if(!a){assert(interactive&&!session);size_t n=strlen(answer);assert(n<=c);memcpy((void *)b,answer,n);return n;}memset((void *)b,0,c);return c;}
    if(nr==SYS_OPEN){const char *path=(void *)a;return strstr(path,"catalog")?10:11;}
    if(nr==SYS_FSTAT){stat_ext_v1_t *st=(void *)b;*st=(stat_ext_v1_t){.size=sizeof *st,.version=1,.type=VFS_FILE,.file_size=128};return 0;}
    if(nr==SYS_CLOSE)return 0;
    assert(nr==SYS_PKGCTL);const pkg_request_v1 *q=(void *)a;pkg_result_v1 *r=(void *)b;
    if(q->operation==PKG_ACQUIRE){assert(!session&&!q->session);session=true;result(r,42,1);return 0;}
    if(q->operation==PKG_RELEASE){assert(session&&q->session==42);session=false;releases++;result(r,0,1);return 0;}
    assert(session&&q->session==42&&q->version==3&&c==112&&q->operation==PKG_UPGRADE_SOURCE);
    unsigned i=0;while(i<3&&strcmp((char *)q->package,installed[i].name))i++;assert(i<3);
    mutations++;installed[i].build=2;installed[i].sequence++;installed[i].identity[0]++;
    result(r,42,installed[i].sequence);return 0;
}
static void reset(void){
    memset(installed,0,sizeof installed);const char *names[]={"alpha","beta","gamma"};
    for(unsigned i=0;i<3;i++){memcpy(installed[i].name,names[i],strlen(names[i])+1);memcpy(installed[i].command,names[i],strlen(names[i])+1);installed[i].build=installed[i].sequence=1;memset(installed[i].artifact,1,32);installed[i].identity[0]=i;}
    refreshes=downloads=mutations=releases=0;session=interactive=current=stale=false;fail_download=-1;answer="yes\n";used=0;output[0]=0;
}
static int run(bool yes){char *argv[]={"pkg","upgrade","-y"};return pkg_system_main(yes?3:2,argv);}
int main(void){
    reset();assert(run(false)&&refreshes==1&&!downloads&&!mutations&&strstr(output,"noninteractive"));
    reset();interactive=true;answer="n\n";assert(run(false)&&!downloads&&!mutations&&strstr(output,"cancelled"));
    reset();interactive=true;assert(!run(false)&&refreshes==1&&downloads==3&&mutations==3&&releases==3&&!session);
    reset();current=true;assert(!run(false)&&!downloads&&!mutations&&strstr(output,"are current"));
    reset();assert(!run(true)&&refreshes==1&&mutations==3&&releases==3);
    reset();fail_download='b';assert(run(true)&&mutations==1&&downloads==2&&!session);
    assert(strstr(output,"alpha: completed")&&strstr(output,"beta: failed")&&strstr(output,"gamma: unattempted"));
    assert(installed[0].build==2&&installed[1].build==1&&installed[2].build==1);
    reset();stale=true;assert(run(true)&&!mutations&&strstr(output,"record changed")&&!session);
    puts("PASS actual batch CLI: preview/refusal, noninteractive refusal, yes/-y, no upgrades, sequential partial failure, stale snapshot, session-free downloads");return 0;
}
