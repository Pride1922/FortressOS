/* Normal Ring 3 entry: one request per callback, no mutation retries. */
#include "system_cli.h"
#include "pkg_abi.h"
#include "mutation_cli.h"
#include "../tools/common.h"
#include "string.h"
#ifdef FORTRESS_PKG_FETCH
#include "repository.h"
#endif
typedef struct {uint64_t session;} cli_owner;
static int control(unsigned operation,uint64_t session,pkg_result_v1 *out) {
    pkg_request_v1 q={.size=sizeof q,.version=1,.operation=operation,.session=session};
    return (int)tool_syscall(SYS_PKGCTL,(uintptr_t)&q,(uintptr_t)out,sizeof q);
}
static int snapshot(const pkg_result_v1 *r,uint64_t session,pkg_transaction_status *out) {
    if(r->size!=sizeof *r||r->version!=1||r->reserved||r->session!=session||
       r->phase>PKG_TX_RETIRING||!r->sequence||!r->account_sequence||!r->credential_generation)return -1;
    *out=(pkg_transaction_status){.phase=r->phase,.sequence=r->sequence,
        .account_sequence=r->account_sequence,.credential_generation=r->credential_generation};
    for(unsigned i=0;i<2;i++) {
        if(r->remaining[i]>UINT64_MAX-r->charged[i]||
           (r->phase==PKG_TX_IDLE&&(r->remaining[i]||r->charged[i])))return -1;
        out->remaining[i]=r->remaining[i];out->charged[i]=r->charged[i];
    }
    return 0;
}
static int acquire(cli_owner *owner) {
    if(owner->session)return 0;
    pkg_result_v1 r={0};pkg_transaction_status status;
    if(control(PKG_ACQUIRE,0,&r))return -1;
    /* Retain a returned handle for cleanup even if its snapshot is rejected. */
    owner->session=r.session;
    return !r.session?-1:snapshot(&r,r.session,&status);
}
static int status(void *context,pkg_transaction_status *out) {
    (void)context;pkg_result_v1 r={0};
    return control(PKG_STATUS,0,&r)?-1:snapshot(&r,0,out);
}
static int recover(void *context,pkg_transaction_status *out) {
    cli_owner *owner=context;pkg_result_v1 r={0};
    if(acquire(owner)||control(PKG_RECOVER,owner->session,&r))return -1;
    return snapshot(&r,owner->session,out);
}
static int mutate(void *context,bool remove,const char *name,size_t n) {
    cli_owner *owner=context;
    if(!n||n>32||acquire(owner))return -1;
    pkg_request_v1 q={.size=sizeof q,.version=1,.operation=remove?PKG_REMOVE:PKG_INSTALL,.session=owner->session};
    for(size_t i=0;i<n;i++)q.package[i]=name[i];
    pkg_result_v1 r={0};pkg_transaction_status result;
    if(tool_syscall(SYS_PKGCTL,(uintptr_t)&q,(uintptr_t)&r,sizeof q))return -1;
    return snapshot(&r,owner->session,&result)||result.phase!=PKG_TX_IDLE?-1:0;
}
#ifdef FORTRESS_PKG_FETCH
/* Opened sources have no authority; the kernel retains and copies their bytes. */
static int source_open(const char *directory,const char *leaf,size_t maximum,uint64_t *length) {
    char path[192];size_t n=0;
    if(!directory||directory[0]!='/')return -1;
    while(n<160&&directory[n])n++;
    if(!n||n==160||directory[n-1]=='/')return -1;
    for(size_t i=0;i<n;i++) {
        path[i]=directory[i];
    }
    path[n++]='/';
    size_t k=0;while(leaf[k]){path[n+k]=leaf[k];k++;}path[n+k]=0;
    long fd=tool_syscall(SYS_OPEN,(uintptr_t)path,VFS_O_RDONLY|VFS_O_NOFOLLOW,0);
    if(fd<0)return -1;
    stat_ext_v1_t st={0};
    if(tool_syscall(SYS_FSTAT,fd,(uintptr_t)&st,0)||st.size!=sizeof st||st.version!=1||
       st.type!=VFS_FILE||!st.file_size||st.file_size>maximum){
        (void)tool_syscall(SYS_CLOSE,fd,0,0);return -1;
    }
    *length=st.file_size;return (int)fd;
}
static void cli_msg(const char *s) {
    size_t l=0;while(s[l])l++;
    (void)tool_syscall(SYS_WRITE,1,(uintptr_t)s,l);
}
static int install_source(cli_owner *owner,int argc,char **argv) {
    size_t n=0;
    if(argc!=4||!argv[2]||!argv[3])return tool_error("pkg","usage: pkg install PACKAGE ABSOLUTE-STAGING-DIRECTORY",NULL);
    while(n<33&&argv[2][n])n++;
    if(!fpk_identity_valid((const uint8_t *)argv[2],n))return tool_error("pkg","invalid package name",NULL);
    if(acquire(owner))return tool_error("pkg","installation backend unavailable or session denied",NULL);
    uint64_t cn=0,an=0;
    int catalog=source_open(argv[3],"catalog.v1",PKG_SOURCE_CATALOG_MAX,&cn);
    int artifact=catalog<0?-1:source_open(argv[3],"artifact.fpk",PKG_SOURCE_ARTIFACT_MAX,&an);
    if(catalog<0||artifact<0){
        if(catalog>=0)(void)tool_syscall(SYS_CLOSE,catalog,0,0);
        return tool_error("pkg","cannot open bounded catalog/artifact from staging",NULL);
    }
    pkg_request_v2 q={.request={.size=sizeof q,.version=2,.operation=PKG_INSTALL_SOURCE,.session=owner->session},
        .catalog_fd=(uint64_t)catalog,.catalog_bytes=cn,
        .artifact_fd=(uint64_t)artifact,.artifact_bytes=an};
    for(size_t i=0;i<n;i++)q.request.package[i]=argv[2][i];
    pkg_result_v1 r={0};pkg_transaction_status state;
    long error=tool_syscall(SYS_PKGCTL,(uintptr_t)&q,(uintptr_t)&r,sizeof q);
    long close_catalog=tool_syscall(SYS_CLOSE,catalog,0,0),close_artifact=tool_syscall(SYS_CLOSE,artifact,0,0);
    if(error==SYSCALL_EOPNOTSUPP)return tool_error("pkg","source installation is unavailable in this kernel",NULL);
    if(error){
        char errbuf[64]="kernel error code: ";
        size_t el=strlen(errbuf);
        el+=tool_format_u64(errbuf+el,(uint64_t)(error<0?-error:error));
        errbuf[el++]='\n';errbuf[el]=0;
        cli_msg(errbuf);
        return tool_error("pkg","installation request rejected by kernel",NULL);
    }
    if(close_catalog||close_artifact)
        return tool_error("pkg","failed to close staging descriptors",NULL);
    if(snapshot(&r,owner->session,&state)||state.phase!=PKG_TX_IDLE)
        return tool_error("pkg","installation not confirmed; inspect status before retrying",NULL);
    static const char done[]="installation complete\n";
    return tool_write("pkg",done,sizeof done-1);
}
static int install_cached(cli_owner *owner,const char *name) {
    size_t n=0;while(n<33&&name[n])n++;
    if(!fpk_identity_valid((const uint8_t *)name,n))return tool_error("pkg","invalid package name",NULL);
    if(acquire(owner))return tool_error("pkg","installation backend unavailable or session denied",NULL);

    static const char *candidates[]={"/var/cache/pkg","/tmp/pkg-cache"};
    int catalog=-1,artifact=-1;
    uint64_t cn=0,an=0;

    for(unsigned c=0;c<2;c++){
        char art_leaf[64];
        if(n+5>=sizeof art_leaf)return tool_error("pkg","package name too long",NULL);
        memcpy(art_leaf,name,n);memcpy(art_leaf+n,".fpk",5);
        catalog=source_open(candidates[c],"catalog.v1",PKG_SOURCE_CATALOG_MAX,&cn);
        if(catalog>=0){
            artifact=source_open(candidates[c],art_leaf,PKG_SOURCE_ARTIFACT_MAX,&an);
            if(artifact>=0)break;
            (void)tool_syscall(SYS_CLOSE,catalog,0,0);
            catalog=-1;
        }
    }
    if(catalog<0||artifact<0){
        if(catalog>=0)(void)tool_syscall(SYS_CLOSE,catalog,0,0);
        return tool_error("pkg","package not found in cache; run 'pkg get' first",name);
    }

    pkg_request_v2 q={.request={.size=sizeof q,.version=2,.operation=PKG_INSTALL_SOURCE,.session=owner->session},
        .catalog_fd=(uint64_t)catalog,.catalog_bytes=cn,
        .artifact_fd=(uint64_t)artifact,.artifact_bytes=an};
    for(size_t i=0;i<n;i++)q.request.package[i]=name[i];
    pkg_result_v1 r={0};pkg_transaction_status state;
    long error=tool_syscall(SYS_PKGCTL,(uintptr_t)&q,(uintptr_t)&r,sizeof q);
    long close_catalog=tool_syscall(SYS_CLOSE,catalog,0,0),close_artifact=tool_syscall(SYS_CLOSE,artifact,0,0);
    if(error==SYSCALL_EOPNOTSUPP)return tool_error("pkg","source installation is unavailable in this kernel",NULL);
    if(error){
        return tool_error("pkg","installation request rejected by kernel",NULL);
    }
    if(close_catalog||close_artifact)
        return tool_error("pkg","failed to close staging descriptors",NULL);
    if(snapshot(&r,owner->session,&state)||state.phase!=PKG_TX_IDLE)
        return tool_error("pkg","installation not confirmed; inspect status before retrying",NULL);
    static const char done[]="installation complete\n";
    return tool_write("pkg",done,sizeof done-1);
}
#endif
int pkg_system_main(int argc,char **argv) {
#ifdef FORTRESS_PKG_FETCH
    if(argc>1&&argv&&argv[1]){
        if(tool_equal(argv[1],"fetch"))return pkg_fetch_main(argc,argv);
        if(tool_equal(argv[1],"update"))return pkg_update_main(argc,argv);
        if(tool_equal(argv[1],"get"))return pkg_get_main(argc,argv);
    }
#endif
    cli_owner owner={0};
    pkg_mutation_backend backend={.transactions={&owner,status,recover},.mutate=mutate};
    int result;
#ifdef FORTRESS_PKG_FETCH
    if(argc==4&&argv&&argv[1]&&tool_equal(argv[1],"install"))result=install_source(&owner,argc,argv);
    else if(argc==3&&argv&&argv[1]&&tool_equal(argv[1],"install"))result=install_cached(&owner,argv[2]);
    else
#endif
    result=pkg_mutation_cli(argc,argv,NULL,&backend);
    if(owner.session) {
        pkg_result_v1 released={0};
        if(control(PKG_RELEASE,owner.session,&released)||released.size!=sizeof released||
           released.version!=1||released.reserved||released.session)
            return tool_error("pkg","session release not confirmed; inspect status before retrying",NULL);
    }
    return result;
}
