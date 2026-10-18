/* Fixed host identity command; intentionally no arbitrary output paths. */
#include "common.h"
#include "crypto.h"
#include "ssh_hostkey.h"
#include "hostkey_abi.h"
#include "creds.h"
#include "../ssh_live_key.h"
static ssh_hostkey_workspace workspace;
static char container[HOSTKEY_FILE_MAX],verify[HOSTKEY_FILE_MAX],public_text[128],fingerprint[51];
static uint8_t secret[64],expected[32];
static void cleanup(void) {
    crypto_wipe(&workspace,sizeof(workspace));crypto_wipe(container,sizeof(container));
    crypto_wipe(verify,sizeof(verify));crypto_wipe(secret,sizeof(secret));crypto_wipe(expected,sizeof(expected));
}
static int live_identity(bool provision) {
    uint32_t real,effective,saved;
    long caps=tool_syscall(SYS_CAPGET,0,0,0);
    if (tool_syscall(SYS_GETRESUID,(uintptr_t)&real,(uintptr_t)&effective,(uintptr_t)&saved) ||
        effective || caps<0 || !((uint64_t)caps & CAP_SYS_ADMIN))
        return tool_error("ssh-keygen","live identity requires root and CAP_SYS_ADMIN",NULL);
    /* An installed-root error is never permission to substitute a live key. */
    if (tool_syscall(SYS_HOSTKEY,HOSTKEY_READ,(uintptr_t)verify,sizeof(verify))!=SYSCALL_EOPNOTSUPP)
        return tool_error("ssh-keygen","--live is only available on a live boot",NULL);
    long n=ssh_live_read(tool_syscall,SSH_LIVE_KEY,container);
    if (n>0) return ssh_hostkey_decode(container,(size_t)n,secret,&workspace) ?
        tool_error("ssh-keygen","invalid live identity; existing key preserved",NULL) : 0;
    if (!provision || n!=SYSCALL_ENOENT)
        return tool_error("ssh-keygen","live identity missing, unsafe or incomplete; reboot to recover",NULL);
    size_t length=0;
    if (ssh_hostkey_generate(container,sizeof(container),&length,&workspace) ||
        ssh_hostkey_decode(container,length,secret,&workspace))
        return tool_error("ssh-keygen","strong random/key generation failed; no live key published",NULL);
    for (unsigned i=0;i<32;i++) expected[i]=secret[i+32];
    long mask=tool_syscall(SYS_UMASK,077,0,0);
    if (mask<0) return tool_error("ssh-keygen","cannot secure live identity permissions",NULL);
    int result=1;
    long fd=-1;
    n=tool_syscall(SYS_MKDIR,(uintptr_t)SSH_LIVE_DIR,0700,0);
    if (n==SYSCALL_EEXIST) {
        /* A simultaneous provisioner owns the directory. Reuse only a fully
         * published valid key; never write into another transaction. */
        n=ssh_live_read(tool_syscall,SSH_LIVE_KEY,verify);
        if (n>0 && !ssh_hostkey_decode(verify,(size_t)n,secret,&workspace)) result=0;
        goto done;
    }
    if (n) goto done;
    fd=tool_syscall(SYS_OPEN,(uintptr_t)SSH_LIVE_STAGE,VFS_O_WRONLY|VFS_O_CREAT|VFS_O_CLOEXEC,0);
    if (fd<0) goto done;
    stat_ext_v1_t st={0};
    if (!ssh_live_metadata(tool_syscall,fd,VFS_FILE,0600,&st)) goto done;
    size_t written=0;
    while (written<length) {
        long count=tool_syscall(SYS_WRITE,fd,(uintptr_t)(container+written),length-written);
        if (count<=0 || (size_t)count>length-written) goto done;
        written+=(size_t)count;
    }
    if (tool_syscall(SYS_CLOSE,fd,0,0)) {fd=-1;goto done;}
    fd=-1;
    n=ssh_live_read(tool_syscall,SSH_LIVE_STAGE,verify);
    if (n<=0 || ssh_hostkey_decode(verify,(size_t)n,secret,&workspace) ||
        crypto_verify32(secret+32,expected)) goto done;
    if (tool_syscall(SYS_RENAME,(uintptr_t)SSH_LIVE_STAGE,(uintptr_t)SSH_LIVE_KEY,0)) goto done;
    n=ssh_live_read(tool_syscall,SSH_LIVE_KEY,verify);
    if (n>0 && !ssh_hostkey_decode(verify,(size_t)n,secret,&workspace) &&
        !crypto_verify32(secret+32,expected)) result=0;
done:
    if (fd>=0) (void)tool_syscall(SYS_CLOSE,fd,0,0);
    if (tool_syscall(SYS_UMASK,(uintptr_t)mask,0,0)<0) result=1;
    if (result) tool_error("ssh-keygen","live provisioning incomplete; no readiness claimed; reboot to recover",NULL);
    return result;
}
int ssh_keygen_main(int argc,char **argv) {
    if(argc==2 && tool_equal(argv[1],"--help")) {
        static const char help[]="Usage: ssh-keygen --provision | --public | --fingerprint | --live [--public | --fingerprint]\n"
            "--live: explicit root-only RAM identity for this boot; never replaces an existing key.\n"
            "Root-only installed host identity. Provision preserves existing keys. SSH remains disabled.\n";
        return tool_write("ssh-keygen",help,sizeof(help)-1) ? 1 : 0;
    }
    bool provision=argc==2 && tool_equal(argv[1],"--provision");
    bool pub=argc==2 && tool_equal(argv[1],"--public");
    bool fp=argc==2 && tool_equal(argv[1],"--fingerprint");
    bool live=(argc==2 || argc==3) && tool_equal(argv[1],"--live");
    if (live && argc==3 && !tool_equal(argv[2],"--public") && !tool_equal(argv[2],"--fingerprint")) live=false;
    if (live && argc==3) pub=tool_equal(argv[2],"--public");
    if(!provision && !pub && !fp && !live) return tool_error("ssh-keygen","expected --provision, --public, --fingerprint or --live",NULL);
    int result=1;size_t length=0;
    if (live) {
        if (live_identity(argc==2)) goto done;
        goto export;
    }
    long n=tool_syscall(SYS_HOSTKEY,HOSTKEY_READ,(uintptr_t)container,sizeof(container));
    if(n==SYSCALL_ENOENT && provision) {
        int r=ssh_hostkey_generate(container,sizeof(container),&length,&workspace);
        if(r) {tool_sys_error("ssh-keygen","strong random/key generation failed",NULL,r);goto done;}
        if(ssh_hostkey_decode(container,length,secret,&workspace)) goto invalid;
        for(unsigned i=0;i<32;i++) expected[i]=secret[i+32];
        r=(int)tool_syscall(SYS_HOSTKEY,HOSTKEY_PUBLISH,(uintptr_t)container,length);
        if(r) {
            tool_sys_error("ssh-keygen","publication failed; inspect existing key or pending provisioning",NULL,r);goto done;
        }
        n=tool_syscall(SYS_HOSTKEY,HOSTKEY_READ,(uintptr_t)verify,sizeof(verify));
        if(n<0) {tool_sys_error("ssh-keygen","durable readback failed",NULL,n);goto done;}
        if(ssh_hostkey_decode(verify,(size_t)n,secret,&workspace) || crypto_verify32(secret+32,expected)) goto invalid;
    } else {
        if(n<0) {tool_sys_error("ssh-keygen","identity unavailable; provision explicitly on an installed root",NULL,n);goto done;}
        if(ssh_hostkey_decode(container,(size_t)n,secret,&workspace)) goto invalid;
    }
export:
    if(pub) {
        if(ssh_hostkey_public(secret+32,public_text,sizeof(public_text),&length,&workspace)) goto invalid;
        result=tool_write("ssh-keygen",public_text,length) ? 1 : 0;
    } else {
        if(ssh_hostkey_fingerprint(secret+32,fingerprint,sizeof(fingerprint),&length,&workspace)) goto invalid;
        result=tool_write("ssh-keygen",fingerprint,length) || tool_write("ssh-keygen","\n",1) ? 1 : 0;
    }
    goto done;
invalid:
    tool_error("ssh-keygen","invalid host identity; SSH disabled; existing key preserved",NULL);
done:
    cleanup();return result;
}
