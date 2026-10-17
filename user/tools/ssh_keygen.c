/* Fixed host identity command; intentionally no arbitrary output paths. */
#include "common.h"
#include "crypto.h"
#include "ssh_hostkey.h"
#include "hostkey_abi.h"
static ssh_hostkey_workspace workspace;
static char container[HOSTKEY_FILE_MAX],verify[HOSTKEY_FILE_MAX],public_text[128],fingerprint[51];
static uint8_t secret[64],expected[32];
static void cleanup(void) {
    crypto_wipe(&workspace,sizeof(workspace));crypto_wipe(container,sizeof(container));
    crypto_wipe(verify,sizeof(verify));crypto_wipe(secret,sizeof(secret));crypto_wipe(expected,sizeof(expected));
}
int ssh_keygen_main(int argc,char **argv) {
    if(argc==2 && tool_equal(argv[1],"--help")) {
        static const char help[]="Usage: ssh-keygen --provision | --public | --fingerprint\n"
            "Root-only installed host identity. Provision preserves existing keys. SSH remains disabled.\n";
        return tool_write("ssh-keygen",help,sizeof(help)-1) ? 1 : 0;
    }
    bool provision=argc==2 && tool_equal(argv[1],"--provision");
    bool pub=argc==2 && tool_equal(argv[1],"--public");
    bool fp=argc==2 && tool_equal(argv[1],"--fingerprint");
    if(!provision && !pub && !fp) return tool_error("ssh-keygen","expected --provision, --public or --fingerprint",NULL);
    int result=1;size_t length=0;
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
