/* Actual normal frontend/fetch/verifiers; mocked filesystem, spawn and network
 * delivery only. The mock never implements an admission or install backend. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "user/pkg/system_cli.h"
#include "user/pkg/repository.h"
#include "user/tools/common.h"
#include "crypto.h"
static uint8_t policy[1025],catalog[4096],artifact[4096];
static uint8_t configuration[]="version=1\nurl=https://repository.invalid\n";
static const char *paths[]={"/etc/pkg/repository.conf","/etc/pkg/trust.conf","/staged/catalog.v1","/staged/artifact.fpk"};
static const uint8_t *bytes[4];static size_t sizes[4],offsets[4];static bool opened[4],stage;
static unsigned downloads,open_count;static bool bad_owner,bad_mode,bad_clock,bad_read,bad_close,bad_stat,bad_output,failed_download;
static char output[1024],object_url[256];static size_t written;
long tool_syscall(long call,uintptr_t a,uintptr_t b,uintptr_t c){
    if(call==SYS_OPEN){
        assert(b==(VFS_O_RDONLY|VFS_O_NOFOLLOW)&&!c);
        unsigned i=0;while(i<4&&strcmp((void *)a,paths[i]))i++;assert(i<4&&!opened[i]);
        if(i>=2&&downloads<i-1)return SYSCALL_ENOENT;
        opened[i]=true;offsets[i]=0;open_count++;return i+3;
    }
    if(call==SYS_CLOCK_GETTIME){assert(a==CLOCK_REALTIME);*(timespec_t *)b=(timespec_t){bad_clock?0:1793188800,0};return 0;}
    if(call==SYS_MKDIR){assert(!strcmp((void *)a,"/staged")&&b==0700&&!c);if(stage)return SYSCALL_EEXIST;stage=true;return 0;}
    if(call==SYS_SPAWN){
        assert(stage&&!strcmp((void *)a,"/bin/wget")&&!c);const char *const *args=(void *)b;
        assert(!strcmp(args[0],"wget")&&!strcmp(args[1],"-q")&&!strcmp(args[2],"-O")&&!args[5]&&downloads<2);
        assert(!strcmp(args[3],paths[2+downloads]));assert(!strcmp(args[4],downloads?object_url:"https://repository.invalid/catalog.v1"));
        downloads++;return 90;
    }
    if(call==SYS_WAIT){assert(a==90&&!c);*(int64_t *)b=failed_download?1:0;return 0;}
    if(call==SYS_WRITE){assert(a==1||a==2);if(bad_output)return SYSCALL_EIO;size_t n=c>11?11:c;assert(written+n<sizeof output);memcpy(output+written,(void *)b,n);written+=n;output[written]=0;return n;}
    /* Any PKGCTL or filesystem mutation from this frontend is a test failure. */
    assert(a>=3&&a<7&&opened[a-3]);unsigned i=a-3;
    if(call==SYS_CLOSE){opened[i]=false;open_count--;return bad_close?SYSCALL_EIO:0;}
    if(call==SYS_FSTAT){
        stat_ext_v1_t *st=(void *)b;*st=(stat_ext_v1_t){.size=sizeof *st,.version=1,.file_size=sizes[i],.type=bad_stat?VFS_STREAM:VFS_FILE,
            .mode=bad_mode&&i==0?0666:0644,.uid=bad_owner&&i==0?1000:0};return 0;
    }
    assert(call==SYS_READ);if(bad_read)return SYSCALL_EIO;
    size_t n=c;if(n>sizes[i]-offsets[i])n=sizes[i]-offsets[i];if(n>37)n=37;memcpy((void *)b,bytes[i]+offsets[i],n);offsets[i]+=n;return n;
}
static int invoke(void){
    char *args[]={"pkg","fetch","demo","/staged",NULL};downloads=0;written=0;output[0]=0;stage=false;
    int result=pkg_system_main(4,args);assert(!open_count);return result;
}
static size_t load(const char *path,void *out,size_t cap){FILE *f=fopen(path,"rb");assert(f);size_t n=fread(out,1,cap,f);assert(n<cap&&!ferror(f)&&!fclose(f));return n;}
static void parser_cases(void){
    pkg_repository r,before;memset(&r,0xa5,sizeof r);before=r;
    const char *bad[]={"version=2\nurl=https://repository.invalid\n","version=1\nurl=https://user@host\n","version=1\nurl=https://host/x/../y\n",
        "version=1\nurl=https://host?x\n","version=1\nurl=https://host/%2e\n","version=1\nurl=https://-host\n","version=1\nurl=https://host:0\n",
        "version=1\nurl=https://host:65536\n","version=1\nurl=https://host:080\n","version=1\nurl=https://host\nurl=https://evil\n"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++)assert(pkg_repository_decode((void *)bad[i],strlen(bad[i]),&r)<0&&!memcmp(&r,&before,sizeof r));
    assert(!pkg_repository_decode(configuration,sizeof configuration-1,&r));
    char url[PKG_REPO_URL_MAX];assert(!pkg_repository_url(&r,NULL,url)&&!strcmp(url,"https://repository.invalid/catalog.v1"));
}
int main(int argc,char **argv){
    assert(argc==4||argc==5);bytes[0]=configuration;bytes[1]=policy;bytes[2]=catalog;bytes[3]=artifact;
    sizes[0]=sizeof configuration-1;sizes[1]=load(argv[1],policy,sizeof policy);sizes[2]=load(argv[2],catalog,sizeof catalog);sizes[3]=load(argv[3],artifact,sizeof artifact);
    uint8_t hash[32];assert(!crypto_sha256(artifact,sizes[3],hash));char hex[65];for(unsigned i=0;i<32;i++)sprintf(hex+2*i,"%02x",hash[i]);
    snprintf(object_url,sizeof object_url,"https://repository.invalid/objects/sha256/%s.fpk",hex);
    if(argc==5){assert(invoke()&&downloads==(unsigned)atoi(argv[4]));puts("PASS fetch rejects repository key/platform mismatch");return 0;}
    parser_cases();assert(!invoke()&&downloads==2&&strstr(output,"download verified"));
    catalog[sizes[2]-1]^=1;assert(invoke()&&downloads==1);catalog[sizes[2]-1]^=1;
    artifact[sizes[3]-1]^=1;assert(invoke()&&downloads==2);artifact[sizes[3]-1]^=1;
    policy[0]^=1;assert(invoke()&&!downloads);policy[0]^=1;
    pkg_repository_trust trust,before;memset(&trust,0xa5,sizeof trust);before=trust;
    assert(pkg_repository_trust_decode(policy,sizes[1]-1,1793188800,&trust)&&!memcmp(&trust,&before,sizeof trust));
    assert(pkg_repository_trust_decode(policy,sizes[1],0,&trust)&&!memcmp(&trust,&before,sizeof trust));
    assert(pkg_repository_trust_decode(policy,sizes[1],1793188800,(pkg_repository_trust *)policy));
    bad_owner=true;assert(invoke()&&!downloads);bad_owner=false;
    bad_mode=true;assert(invoke()&&!downloads);bad_mode=false;
    bad_clock=true;assert(invoke()&&!downloads);bad_clock=false;
    bad_read=true;assert(invoke()&&!downloads);bad_read=false;
    bad_close=true;assert(invoke()&&!downloads);bad_close=false;
    bad_stat=true;assert(invoke()&&!downloads);bad_stat=false;
    failed_download=true;assert(invoke()&&downloads==1);failed_download=false;
    bad_output=true;assert(invoke()&&downloads==2);bad_output=false;
    puts("PASS actual fetch: signature-before-artifact, content-addressed URL, integrity, config ownership, clock and fd/output failures; mocked delivery, zero installation calls");
}
