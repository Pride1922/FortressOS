#include "repository.h"
#include "pkg_abi.h"
#include "../tools/common.h"
#include "string.h"
#include "crypto.h"
static pkg_repository repository;
static pkg_repository_trust trust;
static fpk_verify_context verifier;
static uint8_t trust_wire[1024],catalog_wire[FPK_CATALOG_MAX],config_wire[512];
static char request_url[PKG_REPO_URL_MAX],catalog_path[192],artifact_path[192];
static int artifact_fd;
static size_t selected_catalog_n;
static char selected_cache[128],snapshot_directory[160];
static int opened(const char *path,uint64_t maximum,bool configuration,uint64_t *length){
    long fd=tool_syscall(SYS_OPEN,(uintptr_t)path,VFS_O_RDONLY|VFS_O_NOFOLLOW,0);if(fd<0)return -1;
    stat_ext_v1_t st={0};
    if(tool_syscall(SYS_FSTAT,fd,(uintptr_t)&st,0)||st.size!=sizeof st||st.version!=1||st.type!=VFS_FILE||!st.file_size||st.file_size>maximum||
       (configuration&&(st.uid||st.gid||(st.mode&0022)))){
        (void)tool_syscall(SYS_CLOSE,fd,0,0);return -1;
    }
    *length=st.file_size;return (int)fd;
}
static int read_file(const char *path,uint8_t *out,size_t maximum,bool configuration,size_t *length){
    uint64_t bytes=0;int fd=opened(path,maximum,configuration,&bytes);if(fd<0)return -1;
    size_t at=0;int result=-1;
    while(at<bytes){size_t n=(size_t)bytes-at;if(n>4096)n=4096;long got=tool_syscall(SYS_READ,fd,(uintptr_t)(out+at),n);if(got<=0||(size_t)got>n)goto done;at+=(size_t)got;}
    uint8_t extra;if(tool_syscall(SYS_READ,fd,(uintptr_t)&extra,1))goto done;
    *length=at;result=0;
done:
    if(tool_syscall(SYS_CLOSE,fd,0,0))result=-1;
    return result;
}
static int download(const char *url,const char *path){
    const char *args[]={"wget","-q","-O",path,url,NULL};
    long child=tool_syscall(SYS_SPAWN,(uintptr_t)"/bin/wget",(uintptr_t)args,0);if(child<0)return -1;
    int64_t status=0;long waited;
    do{waited=tool_syscall(SYS_WAIT,child,(uintptr_t)&status,0);}while(waited==SYSCALL_EINTR);
    return waited||status?-1:0;
}
static long read_artifact(void *unused,void *out,size_t n){(void)unused;return tool_syscall(SYS_READ,artifact_fd,(uintptr_t)out,n);}
static bool staging(const char *directory){
    size_t n=0;while(n<160&&directory[n])n++;
    if(!n||n==160||directory[0]!='/'||directory[n-1]=='/')return false;
    for(size_t at=1;at<n;){
        size_t start=at;while(at<n&&directory[at]!='/'){
            char c=directory[at++];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'))return false;
        }
        size_t len=at-start;if(!len||(len==1&&directory[start]=='.')||(len==2&&directory[start]=='.'&&directory[start+1]=='.'))return false;
        if(at<n)at++;
    }
    memcpy(catalog_path,directory,n);memcpy(catalog_path+n,"/catalog.v1",12);
    memcpy(artifact_path,directory,n);memcpy(artifact_path+n,"/artifact.fpk",14);return true;
}
static const char *pkg_cache_dir(char out[128]){
    (void)tool_syscall(SYS_MKDIR,(uintptr_t)"/var",0755,0);
    (void)tool_syscall(SYS_MKDIR,(uintptr_t)"/var/cache",0755,0);
    (void)tool_syscall(SYS_MKDIR,(uintptr_t)"/var/cache/pkg",0755,0);
    long fd=tool_syscall(SYS_OPEN,(uintptr_t)"/var/cache/pkg/.probe",VFS_O_WRONLY|VFS_O_CREAT|VFS_O_TRUNC,0644);
    if(fd>=0){
        tool_syscall(SYS_CLOSE,fd,0,0);
        tool_syscall(SYS_UNLINK,(uintptr_t)"/var/cache/pkg/.probe",0,0);
        memcpy(out,"/var/cache/pkg",15);return out;
    }
    (void)tool_syscall(SYS_MKDIR,(uintptr_t)"/tmp",0777,0);
    (void)tool_syscall(SYS_MKDIR,(uintptr_t)"/tmp/pkg-cache",0700,0);
    memcpy(out,"/tmp/pkg-cache",15);return out;
}
static int load_trust_and_repo(timespec_t *now){
    size_t n=0;
    if(read_file("/etc/pkg/repository.conf",config_wire,sizeof config_wire,true,&n)||pkg_repository_decode(config_wire,n,&repository))
        return tool_error("pkg","invalid or missing repository configuration",NULL);
    if(tool_syscall(SYS_CLOCK_GETTIME,CLOCK_REALTIME,(uintptr_t)now,0)||now->tv_sec<0||now->tv_nsec<0||now->tv_nsec>=1000000000LL)
        return tool_error("pkg","valid UTC clock required",NULL);
    if(read_file("/etc/pkg/trust.conf",trust_wire,sizeof trust_wire,true,&n)||pkg_repository_trust_decode(trust_wire,n,(uint64_t)now->tv_sec,&trust))
        return tool_error("pkg","invalid or missing repository public-key configuration",NULL);
    return 0;
}
int pkg_fetch_main(int argc,char **argv){
    tool_output_failed=false;
    size_t name_length=0;if(argc==4&&argv&&argv[2])while(name_length<33&&argv[2][name_length])name_length++;
    if(argc!=4||!argv||!argv[2]||!argv[3]||!fpk_identity_valid((const uint8_t *)argv[2],name_length)||!staging(argv[3]))
        return tool_error("pkg","usage: pkg fetch PACKAGE NEW-ABSOLUTE-DIRECTORY",NULL);
    size_t n=0;timespec_t now={0};
    if(read_file("/etc/pkg/repository.conf",config_wire,sizeof config_wire,true,&n)||pkg_repository_decode(config_wire,n,&repository))
        return tool_error("pkg","invalid or missing repository configuration",NULL);
    if(tool_syscall(SYS_CLOCK_GETTIME,CLOCK_REALTIME,(uintptr_t)&now,0)||now.tv_sec<0||now.tv_nsec<0||now.tv_nsec>=1000000000LL)
        return tool_error("pkg","valid UTC clock required",NULL);
    if(read_file("/etc/pkg/trust.conf",trust_wire,sizeof trust_wire,true,&n)||pkg_repository_trust_decode(trust_wire,n,(uint64_t)now.tv_sec,&trust))
        return tool_error("pkg","invalid or missing repository public-key configuration",NULL);
    if(tool_syscall(SYS_MKDIR,(uintptr_t)argv[3],0700,0))return tool_error("pkg","staging directory must be new and writable",NULL);
    if(pkg_repository_url(&repository,NULL,request_url)||download(request_url,catalog_path)||
       read_file(catalog_path,catalog_wire,sizeof catalog_wire,false,&n))return tool_error("pkg","catalog download failed; staging retained",NULL);
    fpk_catalog catalog;fpk_catalog_target target;
    if(fpk_catalog_verify(catalog_wire,n,&trust.catalog,&catalog)||fpk_catalog_find(&catalog,(const uint8_t *)argv[2],tool_length(argv[2]),&target))
        return tool_error("pkg","catalog signature or package rejected; staging retained",NULL);
    if(pkg_repository_url(&repository,target.sha256,request_url)||download(request_url,artifact_path))return tool_error("pkg","artifact download failed; staging retained",NULL);
    uint64_t size;artifact_fd=opened(artifact_path,FPK_HEADER_SIZE+FPK_MANIFEST_MAX+FPK_COMPRESSED_MAX,false,&size);if(artifact_fd<0)return tool_error("pkg","invalid downloaded artifact",NULL);
    int r=size!=target.length?-1:fpk_verify_authorized(&verifier,catalog_wire,n,&trust.catalog,(const uint8_t *)argv[2],tool_length(argv[2]),read_artifact,NULL);
    if(tool_syscall(SYS_CLOSE,artifact_fd,0,0))r=-1;
    artifact_fd=-1;
    if(r||memcmp(verifier.manifest.platform_id,trust.platform,32)||verifier.manifest.base_min>trust.base_version||verifier.manifest.base_max<trust.base_version)
        return tool_error("pkg","artifact integrity/platform/base rejected; staging retained",NULL);
    static const char done[]="download verified; catalog and package retained in staging\n";
    return tool_write("pkg",done,sizeof done-1);
}

int pkg_update_main(int argc,char **argv){
    (void)argc;(void)argv;
    tool_output_failed=false;
    timespec_t now={0};
    int r=load_trust_and_repo(&now);if(r)return r;
    char cdir[128];pkg_cache_dir(cdir);
    char cat_tmp[192],cat_dst[192];
    size_t clen=strlen(cdir);
    memcpy(cat_tmp,cdir,clen);memcpy(cat_tmp+clen,"/catalog.v1.tmp",16);
    memcpy(cat_dst,cdir,clen);memcpy(cat_dst+clen,"/catalog.v1",12);

    if(pkg_repository_url(&repository,NULL,request_url)||download(request_url,cat_tmp))
        return tool_error("pkg","catalog download failed",NULL);

    size_t n=0;
    if(read_file(cat_tmp,catalog_wire,sizeof catalog_wire,false,&n))
        return tool_error("pkg","cannot read downloaded catalog",NULL);

    fpk_catalog catalog;
    if(fpk_catalog_verify(catalog_wire,n,&trust.catalog,&catalog))
        return tool_error("pkg","catalog signature rejected",NULL);

    long renamed=tool_syscall(SYS_RENAME_REPLACE,(uintptr_t)cat_tmp,(uintptr_t)cat_dst,0);
    if(renamed<0){
        if(renamed!=SYSCALL_ENOENT)return tool_error("pkg","catalog replacement failed; no retry",NULL);
        if(tool_syscall(SYS_RENAME,(uintptr_t)cat_tmp,(uintptr_t)cat_dst,0)<0)
            return tool_error("pkg","cannot update catalog in cache",NULL);
    }
    static const char msg[]="catalog updated\n";
    return tool_write("pkg",msg,sizeof msg-1);
}

int pkg_get_main(int argc,char **argv){
    tool_output_failed=false;
    if(argc<3||!argv[2])return tool_error("pkg","usage: pkg get PACKAGE",NULL);
    size_t name_len=strlen(argv[2]);
    if(!fpk_identity_valid((const uint8_t *)argv[2],name_len))
        return tool_error("pkg","invalid package name",NULL);

    timespec_t now={0};
    int r=load_trust_and_repo(&now);if(r)return r;

    char cdir[128];pkg_cache_dir(cdir);
    char cat_path[192];size_t clen=strlen(cdir);
    memcpy(cat_path,cdir,clen);memcpy(cat_path+clen,"/catalog.v1",12);

    size_t n=0;
    if(read_file(cat_path,catalog_wire,sizeof catalog_wire,false,&n)){
        if(pkg_update_main(1,argv))return tool_error("pkg","cannot find or update catalog",NULL);
        if(read_file(cat_path,catalog_wire,sizeof catalog_wire,false,&n))
            return tool_error("pkg","cannot read catalog from cache",NULL);
    }

    fpk_catalog catalog;fpk_catalog_target target;
    if(fpk_catalog_verify(catalog_wire,n,&trust.catalog,&catalog)||
       fpk_catalog_find(&catalog,(const uint8_t *)argv[2],name_len,&target))
        return tool_error("pkg","package not found in catalog or catalog signature invalid",argv[2]);

    char art_tmp[192],art_dst[192];
    memcpy(art_tmp,cdir,clen);art_tmp[clen]='/';
    memcpy(art_tmp+clen+1,argv[2],name_len);
    memcpy(art_tmp+clen+1+name_len,".fpk.tmp",9);

    memcpy(art_dst,cdir,clen);art_dst[clen]='/';
    memcpy(art_dst+clen+1,argv[2],name_len);
    memcpy(art_dst+clen+1+name_len,".fpk",5);

    if(pkg_repository_url(&repository,target.sha256,request_url)||download(request_url,art_tmp))
        return tool_error("pkg","artifact download failed",NULL);

    uint64_t size=0;
    artifact_fd=opened(art_tmp,FPK_HEADER_SIZE+FPK_MANIFEST_MAX+FPK_COMPRESSED_MAX,false,&size);
    if(artifact_fd<0)return tool_error("pkg","invalid downloaded artifact",NULL);
    r=(size!=target.length)?-1:
        fpk_verify_authorized(&verifier,catalog_wire,n,&trust.catalog,
                              (const uint8_t *)argv[2],name_len,read_artifact,NULL);
    (void)tool_syscall(SYS_CLOSE,artifact_fd,0,0);
    artifact_fd=-1;

    if(r||memcmp(verifier.manifest.platform_id,trust.platform,32)||
       verifier.manifest.base_min>trust.base_version||verifier.manifest.base_max<trust.base_version){
        (void)tool_syscall(SYS_UNLINK,(uintptr_t)art_tmp,0,0);
        return tool_error("pkg","artifact integrity/platform/base rejected",NULL);
    }

    long renamed=tool_syscall(SYS_RENAME_REPLACE,(uintptr_t)art_tmp,(uintptr_t)art_dst,0);
    if(renamed<0){
        if(renamed!=SYSCALL_ENOENT)return tool_error("pkg","artifact replacement failed; no retry",NULL);
        if(tool_syscall(SYS_RENAME,(uintptr_t)art_tmp,(uintptr_t)art_dst,0)<0)
            return tool_error("pkg","cannot store artifact in cache",NULL);
    }

    static const char msg[]="download verified\n";
    return tool_write("pkg",msg,sizeof msg-1);
}

static uint64_t installed_u64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
int pkg_installed_read(pkg_installed out[PKG_INSTALLED_MAX],unsigned *count) {
    static uint8_t raw[8192];*count=0;
    for(unsigned slot=0;slot<PKG_INSTALLED_MAX;slot++) {
        char path[64]="/var/lib/pkg/install.v2";
        if(slot){memcpy(path,"/var/lib/pkg/install-00.v2",26);path[21]='0'+slot/10;path[22]='0'+slot%10;}
        long fd=tool_syscall(SYS_OPEN,(uintptr_t)path,VFS_O_RDONLY|VFS_O_NOFOLLOW,0);
        if(fd==SYSCALL_ENOENT)continue;
        if(fd<0)return -1;
        if(tool_syscall(SYS_CLOSE,fd,0,0))return -1;
        size_t n=0;uint8_t digest[32];
        if(read_file(path,raw,sizeof raw,true,&n)||n!=sizeof raw||crypto_sha256(raw,8160,digest)||
           memcmp(digest,raw+8160,32)||memcmp(raw,"FORT-PKG-EXEC1",14)||raw[64]||raw[65]||raw[66]||raw[67]||raw[68]>1)return -1;
        if(!raw[68])continue;
        pkg_installed *v=out+*count;memset(v,0,sizeof *v);v->slot=slot;
        memcpy(v->name,raw+320,32);memcpy(v->command,raw+352,32);
        if(!fpk_identity_valid((uint8_t *)v->name,strlen(v->name))||!fpk_identity_valid((uint8_t *)v->command,strlen(v->command)))return -1;
        for(unsigned i=0;i<*count;i++)if(!strcmp(v->name,out[i].name)||!strcmp(v->command,out[i].command))return -1;
        v->sequence=installed_u64(raw+24);v->build=installed_u64(raw+40);memcpy(v->artifact,raw+112,32);
        if(!v->build||crypto_sha256(raw,sizeof raw,v->identity))return -1;
        (*count)++;
    }
    /* Lexical package order is independent of installation history. */
    for(unsigned i=1;i<*count;i++)for(unsigned j=i;j&&strcmp(out[j-1].name,out[j].name)>0;j--){pkg_installed tmp=out[j];out[j]=out[j-1];out[j-1]=tmp;}
    return 0;
}
int pkg_catalog_refresh(void) {
    snapshot_directory[0]=0;
    int r=pkg_update_main(0,NULL);if(r)return r;
    pkg_cache_dir(selected_cache);char path[192];size_t n=strlen(selected_cache);
    memcpy(path,selected_cache,n);memcpy(path+n,"/catalog.v1",12);
    if(read_file(path,catalog_wire,sizeof catalog_wire,false,&selected_catalog_n))return -1;
    fpk_catalog catalog;
    return fpk_catalog_verify(catalog_wire,selected_catalog_n,&trust.catalog,&catalog);
}
int pkg_catalog_target(const char *name,uint64_t *build,uint8_t hash[32]) {
    fpk_catalog catalog;fpk_catalog_target target;
    if(!selected_catalog_n||fpk_catalog_verify(catalog_wire,selected_catalog_n,&trust.catalog,&catalog)||
       fpk_catalog_find(&catalog,(const uint8_t *)name,strlen(name),&target))return -1;
    *build=target.build;memcpy(hash,target.sha256,32);return 0;
}
const char *pkg_selected_directory(void){return snapshot_directory[0]?snapshot_directory:NULL;}
int pkg_download_selected(const char *name) {
    fpk_catalog catalog;fpk_catalog_target target;
    if(!selected_catalog_n||fpk_catalog_verify(catalog_wire,selected_catalog_n,&trust.catalog,&catalog)||
       fpk_catalog_find(&catalog,(const uint8_t *)name,strlen(name),&target))return tool_error("pkg","package unavailable",name);
    if(!snapshot_directory[0]) {
        size_t n=strlen(selected_cache);memcpy(snapshot_directory,selected_cache,n);memcpy(snapshot_directory+n,"/run-",5);
        uint64_t nonce=0;if(tool_syscall(SYS_GETRANDOM,(uintptr_t)&nonce,sizeof nonce,0)!=(long)sizeof nonce)return 1;
        n+=5;n+=tool_format_u64(snapshot_directory+n,nonce);snapshot_directory[n]=0;
        if(tool_syscall(SYS_MKDIR,(uintptr_t)snapshot_directory,0700,0)){snapshot_directory[0]=0;return tool_error("pkg","cannot create private download directory",NULL);}
        char path[192];memcpy(path,snapshot_directory,n);memcpy(path+n,"/catalog.v1",12);
        long fd=tool_syscall(SYS_OPEN,(uintptr_t)path,VFS_O_WRONLY|VFS_O_CREAT|VFS_O_EXCL,0600);
        if(fd<0)return 1;
        size_t at=0;int r=0;
        while(at<selected_catalog_n){size_t take=selected_catalog_n-at;if(take>4096)take=4096;long got=tool_syscall(SYS_WRITE,fd,(uintptr_t)(catalog_wire+at),take);if(got<=0||(size_t)got>take){r=1;break;}at+=(size_t)got;}
        if(tool_syscall(SYS_CLOSE,fd,0,0))r=1;
        if(r)return tool_error("pkg","cannot retain catalog snapshot",NULL);
    }
    char path[192];size_t n=strlen(snapshot_directory),nn=strlen(name);
    memcpy(path,snapshot_directory,n);path[n++]='/';memcpy(path+n,name,nn);memcpy(path+n+nn,".fpk",5);
    if(pkg_repository_url(&repository,target.sha256,request_url)||download(request_url,path))return tool_error("pkg","artifact download failed",name);
    uint64_t bytes=0;artifact_fd=opened(path,PKG_SOURCE_ARTIFACT_MAX,false,&bytes);
    if(artifact_fd<0)return tool_error("pkg","cannot open downloaded artifact",name);
    int r=bytes!=target.length?-1:fpk_verify_authorized(&verifier,catalog_wire,selected_catalog_n,&trust.catalog,
        (const uint8_t *)name,nn,read_artifact,NULL);
    if(tool_syscall(SYS_CLOSE,artifact_fd,0,0))r=-1;
    artifact_fd=-1;
    if(r||memcmp(verifier.manifest.platform_id,trust.platform,32)||verifier.manifest.base_min>trust.base_version||verifier.manifest.base_max<trust.base_version)
        return tool_error("pkg","artifact integrity/platform/base rejected",name);
    return 0;
}

/* The installed record supplies only a download hint. SYS_PKGCTL independently
 * binds this source's hash to the retained current inventory. */
int pkg_previous_source(const char *name,uint64_t *bytes) {
    static pkg_installed records[PKG_INSTALLED_MAX];unsigned count=0;
    if(pkg_installed_read(records,&count))return -1;
    const pkg_installed *installed=NULL;
    for(unsigned i=0;i<count;i++)if(!strcmp(name,records[i].name))installed=records+i;
    if(!installed)return -1;
    timespec_t now={0};if(load_trust_and_repo(&now))return -1;
    char cdir[160];
    if(snapshot_directory[0])memcpy(cdir,snapshot_directory,strlen(snapshot_directory)+1);else pkg_cache_dir(cdir);
    size_t cn=strlen(cdir);
    char path[192],temporary[192];memcpy(path,cdir,cn);memcpy(path+cn,"/previous.fpk",14);
    memcpy(temporary,cdir,cn);memcpy(temporary+cn,"/previous.fpk.tmp",18);
    if(pkg_repository_url(&repository,installed->artifact,request_url)||download(request_url,temporary))return -1;
    long moved=tool_syscall(SYS_RENAME_REPLACE,(uintptr_t)temporary,(uintptr_t)path,0);
    if(moved<0) {
        /* Only a missing destination permits the no-replace first publication.
         * An uncertain replacement I/O result is never unlinked/retried. */
        if(moved!=SYSCALL_ENOENT||tool_syscall(SYS_RENAME,(uintptr_t)temporary,(uintptr_t)path,0)<0)return -1;
    }
    return opened(path,PKG_SOURCE_ARTIFACT_MAX,false,bytes);
}
