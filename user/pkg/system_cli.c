/* Normal Ring 3 entry: one request per callback, no mutation retries. */
#include "system_cli.h"
#include "pkg_abi.h"
#include "mutation_cli.h"
#include "../tools/common.h"
#include "string.h"
#include "terminal.h"
#ifdef FORTRESS_PKG_FETCH
#include "repository.h"
#endif
typedef struct {uint64_t session,sequence;} cli_owner;
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
    int error=control(PKG_ACQUIRE,0,&r);
    if(error)return error;
    /* Retain a returned handle for cleanup even if its snapshot is rejected. */
    owner->session=r.session;
    if(!r.session||snapshot(&r,r.session,&status))return SYSCALL_EIO;
    owner->sequence=status.sequence;return 0;
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
static int acquire_error(int error) {
    switch(error) {
    case SYSCALL_EAGAIN:cli_msg("pkg: package backend is busy; no package operation started\n");break;
    case SYSCALL_EACCES:case SYSCALL_EPERM:cli_msg("pkg: package session denied\n");break;
    case SYSCALL_EOPNOTSUPP:cli_msg("pkg: installation backend unavailable in this kernel\n");break;
    case SYSCALL_EIO:cli_msg("pkg: could not read a valid package state\n");break;
    case SYSCALL_ENOMEM:cli_msg("pkg: insufficient memory reading package state\n");break;
    default:cli_msg("pkg: cannot acquire package session\n");break;
    }
    char message[96]="pkg: session acquisition failed (kernel error ";
    size_t n=strlen(message);n+=tool_format_i64(message+n,error);
    message[n++]=')';message[n++]='\n';message[n]=0;cli_msg(message);
    return 1;
}
/* Display metadata only. Kernel verification and the returned sequence decide
 * success/no-op. Reading the caller fd does not change kernel snapshot offsets. */
static int display_read(int fd,uint8_t *out,size_t bytes) {
    size_t at=0;
    while(at<bytes) {
        long n=tool_syscall(SYS_READ,fd,(uintptr_t)(out+at),bytes-at);
        if(n<=0||(uint64_t)n>bytes-at)return -1;
        at+=(size_t)n;
    }
    return 0;
}
static void display_version(int fd,const char *name,char version[33]) {
    static uint8_t header[FPK_HEADER_SIZE],wire[FPK_MANIFEST_MAX];
    static fpk_manifest manifest;
    fpk_envelope envelope;
    version[0]=0;
    if(display_read(fd,header,sizeof header)||fpk_parse_envelope(header,sizeof header,&envelope)||
       display_read(fd,wire,envelope.manifest_bytes)||
       fpk_parse_manifest(wire,envelope.manifest_bytes,&manifest)||
       manifest.package.length!=strlen(name)||memcmp(manifest.package.bytes,name,manifest.package.length))return;
    memcpy(version,manifest.version.bytes,manifest.version.length);
    version[manifest.version.length]=0;
}
static int install_done(cli_owner *owner,const pkg_transaction_status *state,const char *name,const char *version,bool upgrading) {
    if(state->sequence==owner->sequence) {
        char message[128];const char *prefix=upgrading?"package is up to date ":"package is already installed ";
        size_t n=strlen(prefix),names=strlen(name),versions=strlen(version);
        memcpy(message,prefix,n);memcpy(message+n,name,names);n+=names;
        if(versions){memcpy(message+n," v ",3);n+=3;memcpy(message+n,version,versions);n+=versions;}
        message[n++]='\n';return tool_write("pkg",message,n);
    }
    const char *done=upgrading?"upgrade complete\n":"installation complete\n";
    int written=tool_write("pkg",done,strlen(done));
    if(!written&&upgrading&&!strcmp(name,"fortress-desktop")) {
        static const char restart[]="Save and close your apps, then restart the desktop to apply this update. No laptop reboot is needed.\n";
        return tool_write("pkg",restart,sizeof restart-1);
    }
    return written;
}

static int install_error(long error) {
    switch(error) {
    case SYSCALL_EIO: cli_msg("pkg: storage I/O failure; installation is not confirmed\n");break;
    case SYSCALL_ENOSPC: cli_msg("pkg: insufficient disk space\n");break;
    case SYSCALL_EACCES: cli_msg("pkg: package verification or authorization rejected\n");break;
    case SYSCALL_ENOMEM: cli_msg("pkg: insufficient memory\n");break;
    case SYSCALL_EAGAIN: cli_msg("pkg: package backend is busy; installation is not confirmed\n");break;
    default:break;
    }
    char message[80]="pkg: installation failed (kernel error ";
    size_t n=strlen(message);
    n+=tool_format_u64(message+n,(uint64_t)(error<0?-error:error));
    message[n++]=')';message[n++]='\n';message[n]=0;
    cli_msg(message);
    cli_msg("pkg: run 'sudo pkg status' before retrying\n");
    return 1;
}
static int install_source(cli_owner *owner,int argc,char **argv) {
    size_t n=0;
    if(argc!=4||!argv[2]||!argv[3])return tool_error("pkg","usage: pkg install PACKAGE ABSOLUTE-STAGING-DIRECTORY",NULL);
    while(n<33&&argv[2][n])n++;
    if(!fpk_identity_valid((const uint8_t *)argv[2],n))return tool_error("pkg","invalid package name",NULL);
    cli_msg("pkg: checking installation backend...\n");
    int acquisition=acquire(owner);if(acquisition)return acquire_error(acquisition);
    uint64_t cn=0,an=0;
    int catalog=source_open(argv[3],"catalog.v1",PKG_SOURCE_CATALOG_MAX,&cn);
    int artifact=catalog<0?-1:source_open(argv[3],"artifact.fpk",PKG_SOURCE_ARTIFACT_MAX,&an);
    if(catalog<0||artifact<0){
        if(catalog>=0)(void)tool_syscall(SYS_CLOSE,catalog,0,0);
        return tool_error("pkg","cannot open bounded catalog/artifact from staging",NULL);
    }
    char version[33];display_version(artifact,argv[2],version);
    pkg_request_v2 q={.request={.size=sizeof q,.version=2,.operation=PKG_INSTALL_SOURCE,.session=owner->session},
        .catalog_fd=(uint64_t)catalog,.catalog_bytes=cn,
        .artifact_fd=(uint64_t)artifact,.artifact_bytes=an};
    for(size_t i=0;i<n;i++)q.request.package[i]=argv[2][i];
    pkg_result_v1 r={0};pkg_transaction_status state;
    cli_msg("pkg: verifying and installing package...\n");
    long error=tool_syscall(SYS_PKGCTL,(uintptr_t)&q,(uintptr_t)&r,sizeof q);
    long close_catalog=tool_syscall(SYS_CLOSE,catalog,0,0),close_artifact=tool_syscall(SYS_CLOSE,artifact,0,0);
    if(error==SYSCALL_EOPNOTSUPP)return tool_error("pkg","source installation is unavailable in this kernel",NULL);
    if(error)return install_error(error);
    if(close_catalog||close_artifact)
        return tool_error("pkg","failed to close staging descriptors",NULL);
    if(snapshot(&r,owner->session,&state)||state.phase!=PKG_TX_IDLE)
        return tool_error("pkg","installation not confirmed; inspect status before retrying",NULL);
    return install_done(owner,&state,argv[2],version,false);
}
static const pkg_installed *confirmed_record;
static int install_cached(cli_owner *owner,const char *name,bool upgrading) {
    size_t n=0;while(n<33&&name[n])n++;
    if(!fpk_identity_valid((const uint8_t *)name,n))return tool_error("pkg","invalid package name",NULL);
    const char *candidates[]={pkg_selected_directory(),"/var/cache/pkg","/tmp/pkg-cache"};
    int catalog=-1,artifact=-1;
    uint64_t cn=0,an=0;

    for(unsigned c=0;c<3;c++){
        if(!candidates[c])continue;
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
        return tool_error("pkg","cannot open fetched package",name);
    }

    char version[33];display_version(artifact,name,version);
    pkg_request_v2 q={.request={.size=sizeof q,.version=2,.operation=PKG_INSTALL_SOURCE,.session=owner->session},
        .catalog_fd=(uint64_t)catalog,.catalog_bytes=cn,
        .artifact_fd=(uint64_t)artifact,.artifact_bytes=an};
    for(size_t i=0;i<n;i++)q.request.package[i]=name[i];
    uint64_t previous_bytes=0;int previous=-1;
    if(upgrading) {
        cli_msg("pkg: retrieving previous package for ownership verification...\n");
        previous=pkg_previous_source(name,&previous_bytes);
        if(previous<0) {
            (void)tool_syscall(SYS_CLOSE,catalog,0,0);(void)tool_syscall(SYS_CLOSE,artifact,0,0);
            return tool_error("pkg","cannot retrieve the installed package; current installation preserved",NULL);
        }
    }
    int acquisition=acquire(owner);
    if(acquisition){
        if(previous>=0)(void)tool_syscall(SYS_CLOSE,previous,0,0);
        (void)tool_syscall(SYS_CLOSE,catalog,0,0);(void)tool_syscall(SYS_CLOSE,artifact,0,0);
        return acquire_error(acquisition);
    }
    if(confirmed_record) {
        static pkg_installed current[PKG_INSTALLED_MAX];unsigned count=0;bool same=false;
        if(!pkg_installed_read(current,&count))for(unsigned i=0;i<count;i++)
            if(!strcmp(current[i].name,name)&&!memcmp(current[i].identity,confirmed_record->identity,32))same=true;
        if(!same){
            if(previous>=0)(void)tool_syscall(SYS_CLOSE,previous,0,0);
            (void)tool_syscall(SYS_CLOSE,catalog,0,0);(void)tool_syscall(SYS_CLOSE,artifact,0,0);
            return tool_error("pkg","confirmed installed record changed; retry",name);
        }
    }
    static pkg_installed installed_now[PKG_INSTALLED_MAX];unsigned installed_count=0;
    owner->sequence=0;
    if(!pkg_installed_read(installed_now,&installed_count))for(unsigned i=0;i<installed_count;i++)
        if(!strcmp(name,installed_now[i].name))owner->sequence=installed_now[i].sequence;
    q.request.session=owner->session;
    pkg_request_v3 upgrade={.source=q,.previous_fd=(uint64_t)previous,.previous_bytes=previous_bytes};
    if(upgrading){upgrade.source.request.size=sizeof upgrade;upgrade.source.request.version=3;upgrade.source.request.operation=PKG_UPGRADE_SOURCE;}
    pkg_result_v1 r={0};pkg_transaction_status state;
    cli_msg(upgrading?"pkg: verifying and upgrading package...\n":"pkg: verifying and installing package...\n");
    long error=tool_syscall(SYS_PKGCTL,upgrading?(uintptr_t)&upgrade:(uintptr_t)&q,(uintptr_t)&r,upgrading?sizeof upgrade:sizeof q);
    long close_previous=previous>=0?tool_syscall(SYS_CLOSE,previous,0,0):0;
    long close_catalog=tool_syscall(SYS_CLOSE,catalog,0,0),close_artifact=tool_syscall(SYS_CLOSE,artifact,0,0);
    if(error==SYSCALL_EOPNOTSUPP)return tool_error("pkg","source installation is unavailable in this kernel",NULL);
    if(error)return install_error(error);
    if(close_catalog||close_artifact||close_previous)
        return tool_error("pkg","failed to close staging descriptors",NULL);
    if(snapshot(&r,owner->session,&state)||state.phase!=PKG_TX_IDLE)
        return tool_error("pkg","installation not confirmed; inspect status before retrying",NULL);
    return install_done(owner,&state,name,version,upgrading);
}
static int release_owner(cli_owner *owner) {
    if(!owner->session)return 0;
    pkg_result_v1 r={0};
    if(control(PKG_RELEASE,owner->session,&r)||r.size!=sizeof r||r.version!=1||r.reserved||r.session)
        return tool_error("pkg","session release not confirmed",NULL);
    owner->session=0;return 0;
}
static int base_query(unsigned op,uint64_t session,pkg_base_result_v1 *out){
    pkg_request_v1 q={.size=sizeof q,.version=1,.operation=op,.session=session};
    int r=(int)tool_syscall(SYS_PKGCTL,(uintptr_t)&q,(uintptr_t)out,sizeof q);
    if(r)return r;
    return out->size!=sizeof *out||out->version!=1||out->reserved||out->reserved64||
        (out->flags&~31u)||!out->running_build||!out->abi?SYSCALL_EIO:0;
}
static int base_notice(const pkg_base_result_v1 *state){
    if(state->flags&PKG_BASE_FENCED)return tool_error("pkg","base update fenced after an uncertain result; reboot into a complete boot set",NULL);
    if(state->flags&PKG_BASE_REBOOT)cli_msg("fortress-base: update prepared; reboot required. Previous base remains in the boot menu.\n");
    else if(state->flags&PKG_BASE_UNCONFIRMED)cli_msg("fortress-base: running new base; boot confirmation required (sudo pkg confirm-base).\n");
    else cli_msg("fortress-base: current base confirmed.\n");
    if(state->flags&PKG_BASE_RECOVERY)cli_msg("fortress-base: interrupted preparation/selection; inspect base-status and use recover-base before retrying.\n");
    if(state->flags&PKG_BASE_RETRY)cli_msg("fortress-base: interrupted preparation; retry the same verified release or a newer build.\n");
    return 0;
}
static int base_action(cli_owner *owner,unsigned op){
    pkg_base_result_v1 state={0};int r=base_query(PKG_BASE_STATUS,0,&state);
    if(r==SYSCALL_EOPNOTSUPP&&op==PKG_BASE_CONFIRM)return 0;
    if(r)return tool_error("pkg","complete base-update layout unavailable",NULL);
    if(op==PKG_BASE_STATUS)return base_notice(&state);
    if(op==PKG_BASE_CONFIRM&&!state.flags)return 0;
    if(op==PKG_BASE_CONFIRM&&(state.flags&(PKG_BASE_REBOOT|PKG_BASE_RECOVERY))){base_notice(&state);return 1;}
    r=acquire(owner);if(r)return acquire_error(r);
    if(base_query(op,owner->session,&state))return tool_error("pkg","base confirmation/recovery failed; inspect base-status; no automatic retry",NULL);
    return base_notice(&state);
}
static int base_cached(cli_owner *owner){
    const char *directory=pkg_selected_directory();if(!directory)return 1;
    uint64_t cn=0,an=0;int c=source_open(directory,"catalog.v1",PKG_SOURCE_CATALOG_MAX,&cn);
    int a=c<0?-1:source_open(directory,"fortress-base.fpk",PKG_SOURCE_ARTIFACT_MAX,&an);
    if(c<0||a<0){if(c>=0)(void)tool_syscall(SYS_CLOSE,c,0,0);return tool_error("pkg","cannot open fetched base release",NULL);}
    int r=acquire(owner);
    if(!r){
        pkg_request_v2 q={.request={.size=sizeof q,.version=2,.operation=PKG_BASE_STAGE,.session=owner->session},
            .catalog_fd=(uint64_t)c,.catalog_bytes=cn,.artifact_fd=(uint64_t)a,.artifact_bytes=an};
        pkg_base_result_v1 state={0};
        r=(int)tool_syscall(SYS_PKGCTL,(uintptr_t)&q,(uintptr_t)&state,sizeof q);
        if(!r)r=base_notice(&state);
        else {cli_msg("pkg: base preparation rejected or failed; inspect pkg base-status. No automatic retry.\n");}
    }
    long cc=tool_syscall(SYS_CLOSE,c,0,0),ca=tool_syscall(SYS_CLOSE,a,0,0);
    return r||cc||ca?1:0;
}
static bool base_known_hash(const uint8_t hash[32]){for(unsigned i=0;i<32;i++)if(hash[i])return true;return false;}
static int base_one(cli_owner *owner,bool upgrade){
    if(!upgrade)return tool_error("pkg","fortress-base is installed by the baseline image; use pkg upgrade fortress-base",NULL);
    pkg_base_result_v1 state={0};
    if(base_query(PKG_BASE_STATUS,0,&state))return tool_error("pkg","complete base-update layout unavailable",NULL);
    if(state.flags&(PKG_BASE_FENCED|PKG_BASE_REBOOT|PKG_BASE_UNCONFIRMED|PKG_BASE_RECOVERY)){base_notice(&state);return 1;}
    if(pkg_catalog_refresh())return 1;
    uint64_t build;uint8_t hash[32];
    if(pkg_catalog_target("fortress-base",&build,hash))return tool_error("pkg","base release unavailable in repository",NULL);
    if(build<state.floor_build||(build==state.floor_build&&memcmp(hash,state.artifact,32)&&base_known_hash(state.artifact)))return tool_error("pkg","base rollback/substitution rejected",NULL);
    if(build==state.running_build){base_notice(&state);return 0;}
    if(pkg_download_selected("fortress-base"))return 1;
    return base_cached(owner);
}
static int automatic_one(cli_owner *owner,const char *name,bool upgrade) {
    if(!strcmp(name,"fortress-base"))return base_one(owner,upgrade);
    size_t nn=strlen(name);if(!fpk_identity_valid((const uint8_t *)name,nn))return tool_error("pkg","invalid package name",NULL);
    if(pkg_catalog_refresh())return 1;
    static pkg_installed records[PKG_INSTALLED_MAX];unsigned count=0;
    if(!pkg_installed_read(records,&count)) {
        const pkg_installed *installed=NULL;
        for(unsigned i=0;i<count;i++)if(!strcmp(name,records[i].name))installed=records+i;
        if(upgrade&&!installed)return tool_error("pkg","package is not installed",name);
        uint64_t build;uint8_t hash[32];
        if(pkg_catalog_target(name,&build,hash))return tool_error("pkg","package unavailable",name);
        if(!upgrade&&installed&&(build!=installed->build||memcmp(hash,installed->artifact,32)))
            return tool_error("pkg","package already installed; use pkg upgrade",name);
    }
    if(pkg_download_selected(name))return 1;
    return install_cached(owner,name,upgrade);
}
static void build_message(const char *name,uint64_t old,uint64_t next) {
    char number[24];cli_msg(name);cli_msg(": build ");size_t n=tool_format_u64(number,old);number[n]=0;cli_msg(number);
    cli_msg(" -> ");n=tool_format_u64(number,next);number[n]=0;cli_msg(number);cli_msg("\n");
}
static int installed_list(const char *name) {
    static pkg_installed all[PKG_INSTALLED_MAX];unsigned count=0;
    if(pkg_installed_read(all,&count))return tool_error("pkg","installed inventory invalid or inaccessible",NULL);
    bool found=false;
    for(unsigned i=0;i<count;i++)if(!name||!strcmp(name,all[i].name)) {
        build_message(all[i].name,all[i].build,all[i].build);found=true;
    }
    if(name&&!found)return tool_error("pkg","package is not installed",name);
    return 0;
}
/* Console/PTY reads may return a character before the line is complete. */
static bool upgrade_confirm(void) {
    char answer[16];size_t used=0;bool overflow=false;
    for(;;) {
        unsigned char ch;
        if(tool_syscall(SYS_READ,0,(uintptr_t)&ch,1)!=1)return false;
        if(ch=='\n'||ch=='\r')break;
        if(ch==8||ch==127){if(used)used--;continue;}
        if(used==sizeof answer){overflow=true;continue;}
        answer[used++]=(ch>='A'&&ch<='Z')?(char)(ch-'A'+'a'):(char)ch;
    }
    return !overflow&&((used==1&&answer[0]=='y')||(used==3&&!memcmp(answer,"yes",3)));
}
static int upgrade_all(cli_owner *owner,bool yes) {
    static pkg_installed all[PKG_INSTALLED_MAX],current[PKG_INSTALLED_MAX];
    static bool eligible[PKG_INSTALLED_MAX];unsigned count=0,total=0;
    if(pkg_installed_read(all,&count))return tool_error("pkg","installed inventory invalid or inaccessible",NULL);
    if(pkg_catalog_refresh())return 1;
    pkg_base_result_v1 base={0};bool base_eligible=false;
    int base_error=base_query(PKG_BASE_STATUS,0,&base);
    if(base_error && base_error!=SYSCALL_EOPNOTSUPP)return tool_error("pkg","cannot inspect base update state",NULL);
    if(!base_error){
        uint64_t build;uint8_t hash[32];
        if(base.flags&(PKG_BASE_FENCED|PKG_BASE_RECOVERY)){base_notice(&base);return 1;}
        if(!pkg_catalog_target("fortress-base",&build,hash)){
            if(build<base.floor_build || (build==base.floor_build && base_known_hash(base.artifact) && memcmp(hash,base.artifact,32)))return tool_error("pkg","base rollback/substitution rejected",NULL);
            if(build>base.running_build && !(base.flags&(PKG_BASE_REBOOT|PKG_BASE_UNCONFIRMED))){base_eligible=true;total++;build_message("fortress-base",base.running_build,build);}
        }
        if(base.flags&(PKG_BASE_REBOOT|PKG_BASE_UNCONFIRMED))base_notice(&base);
    }
    memset(eligible,0,sizeof eligible);
    for(unsigned i=0;i<count;i++) {
        uint64_t build;uint8_t hash[32];
        if(pkg_catalog_target(all[i].name,&build,hash)){cli_msg(all[i].name);cli_msg(": unavailable; left installed\n");continue;}
        if(build<all[i].build||(build==all[i].build&&memcmp(hash,all[i].artifact,32)))
            return tool_error("pkg","catalog rollback or equal-build substitution",all[i].name);
        if(build>all[i].build){eligible[i]=true;total++;build_message(all[i].name,all[i].build,build);}
    }
    if(!total){cli_msg("Installed packages are current.\n");return 0;}
    char number[24];size_t n=tool_format_u64(number,total);number[n]=0;cli_msg(number);cli_msg(" package(s) to upgrade.\n");
    if(!yes) {
        if(tool_syscall(SYS_TERMCTL,TERM_ISATTY,0,0)!=1)return tool_error("pkg","noninteractive upgrade requires -y",NULL);
        cli_msg("Proceed? [y/N] ");
        if(!upgrade_confirm()){cli_msg("Upgrade cancelled.\n");return 1;}
    }
    for(unsigned i=0;i<count;i++)if(eligible[i]) {
        int r=pkg_download_selected(all[i].name);
        unsigned now=0;bool unchanged=false;
        if(!r&&!pkg_installed_read(current,&now))for(unsigned j=0;j<now;j++)
            if(!strcmp(current[j].name,all[i].name)&&!memcmp(current[j].identity,all[i].identity,32))unchanged=true;
        if(!r&&!unchanged)r=tool_error("pkg","installed record changed; retry",all[i].name);
        confirmed_record=all+i;
        if(!r)r=install_cached(owner,all[i].name,true);
        confirmed_record=NULL;
        int released=release_owner(owner);if(released)r=released;
        cli_msg(all[i].name);cli_msg(r?": failed\n":": completed\n");
        if(r){for(unsigned j=i+1;j<count;j++)if(eligible[j]){cli_msg(all[j].name);cli_msg(": unattempted\n");}if(base_eligible)cli_msg("fortress-base: unattempted\n");return 1;}
    }
    if(base_eligible){
        int r=pkg_download_selected("fortress-base");
        if(!r)r=base_cached(owner);
        int released=release_owner(owner);if(released)r=released;
        if(r)return 1;
    }
    return 0;
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
    if(argc==2&&argv&&tool_equal(argv[1],"base-status"))result=base_action(&owner,PKG_BASE_STATUS);
    else if(argc==2&&argv&&tool_equal(argv[1],"confirm-base"))result=base_action(&owner,PKG_BASE_CONFIRM);
    else if(argc==2&&argv&&tool_equal(argv[1],"recover-base"))result=base_action(&owner,PKG_BASE_RECOVER);
    else if(argc==4&&argv&&argv[1]&&tool_equal(argv[1],"install"))result=install_source(&owner,argc,argv);
    else if(argc==3&&argv&&argv[1]&&tool_equal(argv[1],"install"))result=automatic_one(&owner,argv[2],false);
    else if(argc==3&&argv&&argv[1]&&tool_equal(argv[1],"upgrade"))result=tool_equal(argv[2],"-y")?upgrade_all(&owner,true):automatic_one(&owner,argv[2],true);
    else if(argc==2&&argv&&tool_equal(argv[1],"upgrade"))result=upgrade_all(&owner,false);
    else if(argc==2&&argv&&tool_equal(argv[1],"installed"))result=installed_list(NULL);
    else if(argc==3&&argv&&tool_equal(argv[1],"info"))result=installed_list(argv[2]);
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
