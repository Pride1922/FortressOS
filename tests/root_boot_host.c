#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "root_boot.h"
#include "boot_info.h"
void serial_puts(const char *s) {(void)s;}
void serial_print_dec(uint64_t n) {(void)n;}
static void hash(const void *p,size_t n,char out[65]);
#include "../user/tools/digest.h"
static void hash(const void *p,size_t n,char out[65]) {
    digest_t d;uint8_t raw[32];digest_init(&d,DIGEST_SHA256);assert(digest_update(&d,p,n));digest_final(&d,raw);
    for (unsigned i=0;i<32;i++) sprintf(out+i*2,"%02x",raw[i]);
}
int main(void) {
    const char *guid="11223344-5566-7788-99aa-bbccddeeff00";
    char cmd[600];snprintf(cmd,sizeof(cmd),"boot_mode=installed root=PARTUUID=%s",guid);
    root_boot_config_t c=root_boot_parse(cmd,sizeof(cmd),false);assert(c.valid && c.mode==ROOT_BOOT_INSTALLED && c.has_root);
    assert(root_boot_parse("",1,false).valid && root_boot_parse("boot_mode=live",15,false).valid);
    const char *bad[]={"boot_mode=installed","boot_mode=nope","root=PARTUUID=11223344-5566-7788-99aa-bbccddeeff00",
        "boot_mode=live boot_mode=live","boot_mode=installed root=PARTUUID=00000000-0000-0000-0000-000000000000",
        "boot_mode=live root=x","boot_mode=installed root=x root=x"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(!root_boot_parse(bad[i],strlen(bad[i])+1,false).valid);
    assert(!root_boot_parse(cmd,sizeof(cmd),true).valid && !root_boot_parse(cmd,4,false).valid);
    char identity[256];snprintf(identity,sizeof(identity),"install_id=00112233445566778899aabbccddeeff\nroot_partuuid=%s\nfilesystem_uuid=%s\n",guid,guid);
    char record[512];snprintf(record,sizeof(record),"FORTRESS-RECOVERY-1\n%spassword_hash=$5$rounds=100000$1234567890abcdef$...........................................\n",identity);
    root_recovery_t r;assert(root_recovery_parse(record,strlen(record),&r));
    assert(!root_recovery_parse(record,strlen(record)-1,&r));
    char kh[65],bh[65],sum[65];hash("kernel",6,kh);hash("bootstrap",9,bh);
    char ready[512]={0};snprintf(ready,sizeof(ready),"FORTRESS-READY-1\n%spayload_version=1\nkernel_sha256=%s\nbootstrap_sha256=%s\n",identity,kh,bh);
    size_t prefix=strlen(ready);hash(ready,prefix,sum);snprintf(ready+prefix,sizeof(ready)-prefix,"checksum=%s\n",sum);
    assert(root_readiness_validate(ready,512,&r,"kernel",6,"bootstrap",9));
    assert(!root_readiness_validate(ready,511,&r,"kernel",6,"bootstrap",9));
    for (unsigned i=0;i<512;i++) {char before=ready[i];ready[i]^=1;
        assert(!root_readiness_validate(ready,512,&r,"kernel",6,"bootstrap",9));ready[i]=before;}
    assert(!root_readiness_validate(ready,512,&r,"changed",7,"bootstrap",9));
    char manifest[512];snprintf(manifest,sizeof(manifest),"FORTRESS-INSTALL-1\n%spayload_version=1\n",identity);
    assert(root_manifest_validate(manifest,strlen(manifest),&r));
    assert(!root_manifest_validate(manifest,strlen(manifest)+1,&r));
    struct limine_file kernel={.cmdline=cmd,.address=(void *)"kernel",.size=6};
    struct limine_kernel_file_response kr={.kernel_file=&kernel};
    struct limine_file bootstrap={.path="/boot/bootstrap.tar",.cmdline="role=bootstrap",.address=(void *)"bootstrap",.size=9};
    struct limine_file readiness={.path="/boot/readiness.v1",.cmdline="role=readiness",.address=ready,.size=512};
    struct limine_file *modules[]={&readiness,&bootstrap};
    struct limine_module_response mr={.module_count=2,.modules=modules};boot_info_t info;
    boot_info_init(&info,NULL,NULL,NULL,NULL,NULL,&mr,&kr);
    assert(info.installed_modules_valid && info.initramfs_vaddr==(uint64_t)bootstrap.address);
    modules[0]=&bootstrap;boot_info_init(&info,NULL,NULL,NULL,NULL,NULL,&mr,&kr);assert(!info.installed_modules_valid);
    memset(cmd,'a',599);cmd[599]=0;boot_info_init(&info,NULL,NULL,NULL,NULL,NULL,&mr,&kr);assert(info.cmdline_truncated && !info.installed_modules_valid);
    puts("PASS root boot selection, binding, readiness corruption and module roles");
}
