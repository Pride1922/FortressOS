#include "cli.h"
#include "../tools/common.h"

static fpk_verify_context verifier;
static fpk_catalog_policy current_policy;
static uint8_t catalog_bytes[FPK_CATALOG_MAX], expected_hash[32];
static int artifact_fd;
static int decimal(const char *s,uint64_t *out) {
    uint64_t v=0; if(!*s) return -1;
    for(;*s;s++) {
        if(*s<'0' || *s>'9' || v>(UINT64_MAX-(unsigned)(*s-'0'))/10) return -1;
        v=v*10+(unsigned)(*s-'0');
    }
    *out=v; return 0;
}
static int hex_hash(const char *s,uint8_t out[32]) {
    for(unsigned i=0;i<64;i++) {
        unsigned v;
        if(s[i]>='0' && s[i]<='9') v=(unsigned)(s[i]-'0');
        else if(s[i]>='a' && s[i]<='f') v=(unsigned)(s[i]-'a'+10);
        else return -1;
        if(i%2) out[i/2]|=(uint8_t)v;
        else out[i/2]=(uint8_t)(v<<4);
    }
    return s[64]?-1:0;
}
static int open_regular(const char *path,uint64_t cap,uint64_t *size) {
    size_t len=0; while(len<256 && path[len]) len++;
    if(!len || len==256) return -1;
    long fd=tool_syscall(SYS_OPEN,(uintptr_t)path,VFS_O_RDONLY|VFS_O_NOFOLLOW|VFS_O_NONBLOCK,0);
    if(fd<0) return -1;
    stat_ext_v1_t st={0};
    if(tool_syscall(SYS_FSTAT,(uintptr_t)fd,(uintptr_t)&st,0)<0 ||
       st.size!=sizeof(st) || st.version!=1 || st.type!=VFS_FILE || !st.file_size || st.file_size>cap) {
        (void)tool_syscall(SYS_CLOSE,(uintptr_t)fd,0,0); return -1;
    }
    *size=st.file_size; return (int)fd;
}
static long read_artifact(void *unused,void *p,size_t n) {
    (void)unused;
    return tool_syscall(SYS_READ,(uintptr_t)artifact_fd,(uintptr_t)p,n);
}
static int read_catalog(const char *path,size_t *length) {
    uint64_t size; int fd=open_regular(path,FPK_CATALOG_MAX,&size);
    if(fd<0) return -1;
    size_t at=0; int result=-1;
    while(at<size) {
        size_t n=(size_t)size-at; if(n>4096) n=4096;
        long r=tool_syscall(SYS_READ,(uintptr_t)fd,(uintptr_t)(catalog_bytes+at),n);
        if(r<=0 || (size_t)r>n) goto done;
        at+=(size_t)r;
    }
    if(tool_syscall(SYS_READ,(uintptr_t)fd,(uintptr_t)verifier.io,1)!=0) goto done;
    *length=at; result=0;
done:
    if(tool_syscall(SYS_CLOSE,(uintptr_t)fd,0,0)<0) result=-1;
    return result;
}
static int summary(const char *message) {
    char number[32];
    if(tool_write("pkg",message,tool_length(message)) ||
       tool_write("pkg","package ",8) ||
       tool_write("pkg",verifier.manifest.package.bytes,verifier.manifest.package.length) ||
       tool_write("pkg"," build ",7)) return 1;
    size_t n=tool_format_u64(number,verifier.manifest.build);
    return tool_write("pkg",number,n) || tool_write("pkg","\n",1);
}
int pkg_cli(int argc,char **argv,const fpk_catalog_policy *policy) {
    tool_output_failed=false; verifier.verified=0;
    if(argc==2 && tool_equal(argv[1],"--help")) {
        static const char help[]="pkg inspect ARTIFACT BYTES SHA256\npkg verify CATALOG PACKAGE ARTIFACT\nRead-only verification. Installation is unavailable.\n";
        return tool_write("pkg",help,sizeof(help)-1);
    }
    if(argc!=5) return tool_error("pkg","usage: pkg --help",NULL);
    int inspect=tool_equal(argv[1],"inspect"), verify=tool_equal(argv[1],"verify");
    if(!inspect && !verify) return tool_error("pkg","unsupported command",NULL);
    uint64_t expected=0,size=0; size_t catalog_length=0;
    if(inspect && (decimal(argv[3],&expected) || expected<128 ||
       expected>128+FPK_MANIFEST_MAX+FPK_COMPRESSED_MAX || hex_hash(argv[4],expected_hash)))
        return tool_error("pkg","invalid expected length or lowercase SHA-256",NULL);
    if(verify) {
        if(!policy) return tool_error("pkg","no provisioned catalog trust policy",NULL);
        if(!fpk_identity_valid((const uint8_t *)argv[3],tool_length(argv[3])))
            return tool_error("pkg","invalid package name",NULL);
        current_policy=*policy;
        timespec_t now={0};
        if(tool_syscall(SYS_CLOCK_GETTIME,CLOCK_REALTIME,(uintptr_t)&now,0)<0 || now.tv_sec<1767225600LL ||
           now.tv_sec>4102444800LL || now.tv_nsec<0 || now.tv_nsec>=1000000000LL)
            return tool_error("pkg","valid UTC clock required",NULL);
        current_policy.now=(uint64_t)now.tv_sec;
        if(read_catalog(argv[2],&catalog_length)) return tool_error("pkg","cannot read catalog",NULL);
        /* Authenticate before opening artifact, including missing-target denial. */
        fpk_catalog cat; fpk_catalog_target target;
        if(fpk_catalog_verify(catalog_bytes,catalog_length,&current_policy,&cat) ||
           fpk_catalog_find(&cat,(const uint8_t *)argv[3],tool_length(argv[3]),&target))
            return tool_error("pkg","catalog rejected or package absent",NULL);
        expected=target.length;
    }
    artifact_fd=open_regular(inspect?argv[2]:argv[4],128+FPK_MANIFEST_MAX+FPK_COMPRESSED_MAX,&size);
    if(artifact_fd<0) return tool_error("pkg","cannot open regular artifact",NULL);
    int result=-1;
    if(size==expected) result=inspect ? fpk_verify(&verifier,read_artifact,NULL,expected,expected_hash) :
        fpk_verify_authorized(&verifier,catalog_bytes,catalog_length,&current_policy,
                             (const uint8_t *)argv[3],tool_length(argv[3]),read_artifact,NULL);
    if(tool_syscall(SYS_CLOSE,(uintptr_t)artifact_fd,0,0)<0) result=-1;
    artifact_fd=-1;
    if(result) { verifier.verified=0; return tool_error("pkg","verification failed",NULL); }
    return summary(inspect?"integrity OK; signatures not checked\n":
                   "catalog policy verified; platform/install admission not checked\n");
}
int pkg_main(int argc,char **argv) { return pkg_cli(argc,argv,NULL); }
