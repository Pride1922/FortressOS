#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "base_update.h"
#include "install_archive.h"
#include "root_boot.h"
#include "crypto.h"
static base_update_work work,saved;
static uint8_t *disk_bytes,*prefix,*initial;static size_t disk_size;
static unsigned events,cut;static uint8_t ready[512];
static const char recovery[]="FORTRESS-RECOVERY-1\ninstall_id=00112233445566778899aabbccddeeff\n"
 "root_partuuid=12345678-1234-4234-8234-123456789abc\nfilesystem_uuid=813ea876-9933-4e1c-ae62-2f6c0f90a441\n"
 "password_hash=$5$rounds=100000$1234567890abcdef$...........................................\n";
static bool allowed(uint64_t lba,unsigned n){
    if(lba<saved.disk.esp)return false;
    uint64_t off=(lba-saved.disk.esp)*512,bytes=(uint64_t)n*512;unsigned slot=saved.running^1;
    if(off>=saved.disk.kernel[slot]&&off+bytes<=saved.disk.kernel[slot]+BASE_BANK_BYTES)return true;
    if(off>=saved.bootstrap[slot]&&off+bytes<=saved.bootstrap[slot]+BASE_BANK_BYTES)return true;
    if(bytes==512&&(off==saved.floor_offset[0]||off==saved.floor_offset[1]||off==saved.disk.readiness[slot]||off==saved.state_offset[slot]||off==saved.state_offset[saved.running]))return true;
    for(unsigned i=0;i<4;i++)if(bytes==512&&off==saved.disk.configs[i])return true;
    return false;
}
static int rd(void *ctx,uint64_t lba,void *p,unsigned n){(void)ctx;assert(n&&n<=8&&lba<disk_size/512&&n<=disk_size/512-lba);memcpy(p,disk_bytes+lba*512,n*512);return 0;}
static int wr(void *ctx,uint64_t lba,const void *p,unsigned n){(void)ctx;assert(allowed(lba,n));events++;if(events==cut){memcpy(disk_bytes+lba*512,p,n*256);return -1;}memcpy(disk_bytes+lba*512,p,n*512);return 0;}
static int flush(void *ctx){(void)ctx;return ++events==cut?-1:0;}
static void *load(const char *path,size_t *n){FILE *f=fopen(path,"rb");assert(f&&!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>0);rewind(f);uint8_t *p=malloc(size);assert(p&&fread(p,1,size,f)==(size_t)size&&!fclose(f));*n=size;return p;}
static void hexhash(const void *p,size_t n,char out[65]){uint8_t hash[32];assert(!crypto_sha256(p,n,hash));for(unsigned i=0;i<32;i++)sprintf(out+2*i,"%02x",hash[i]);}
static void restore(void){memcpy(disk_bytes+4096*512,initial,BASE_ESP_BYTES);memcpy(prefix,initial,BASE_ESP_BYTES);work=saved;events=cut=0;}
static void make_ready(const uint8_t *kernel,const uint8_t *boot){
    char kh[65],bh[65],sum[65];hexhash(kernel,BASE_BANK_BYTES,kh);hexhash(boot,BASE_BANK_BYTES,bh);
    int n=snprintf((char *)ready,sizeof ready,"FORTRESS-READY-1\ninstall_id=00112233445566778899aabbccddeeff\nroot_partuuid=12345678-1234-4234-8234-123456789abc\nfilesystem_uuid=813ea876-9933-4e1c-ae62-2f6c0f90a441\npayload_version=1\nkernel_sha256=%s\nbootstrap_sha256=%s\n",kh,bh);
    assert(n>0&&n<440);hexhash(ready,(size_t)n,sum);assert(snprintf((char *)ready+n,sizeof ready-(size_t)n,"checksum=%s\n",sum)>0);
}
int main(int argc,char **argv){
    assert(argc==8);int fd=open(argv[1],O_RDONLY);struct stat st;assert(fd>=0&&!fstat(fd,&st)&&S_ISREG(st.st_mode));disk_size=st.st_size;
    disk_bytes=mmap(NULL,disk_size,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);assert(disk_bytes!=MAP_FAILED);close(fd);
    prefix=malloc(BASE_ESP_BYTES);initial=malloc(BASE_ESP_BYTES);uint8_t *boot=malloc(BASE_BANK_BYTES);assert(prefix&&initial&&boot);
    size_t factory_n,an,cn,kn;uint8_t *factory=load(argv[2],&factory_n),*artifact=load(argv[3],&an),*catalog=load(argv[4],&cn),*key=load(argv[5],&kn);assert(kn==32);
    fpk_catalog_policy policy={.key_count=1,.threshold=1,.root_version=1,.now=1793188800};for(unsigned i=0;i<16;i++)policy.repository[i]=(uint8_t)i;memcpy(policy.keys[0],key,32);
    base_artifact a;assert(!base_artifact_verify(artifact,an,catalog,cn,&policy,&a));
    uint8_t original=artifact[an-1];artifact[an-1]^=1;assert(base_artifact_verify(artifact,an,catalog,cn,&policy,&a));artifact[an-1]=original;
    policy.keys[0][0]^=1;assert(base_artifact_verify(artifact,an,catalog,cn,&policy,&a));policy.keys[0][0]^=1;
    assert(!base_artifact_verify(artifact,an,catalog,cn,&policy,&a));
    unsigned malformed[]={8,12,24,32,40,48,56,152,160,168,255};
    for(unsigned i=0;i<sizeof malformed/sizeof malformed[0];i++){
        unsigned off=malformed[i];uint8_t old=artifact[off];artifact[off]^=0x80;
        assert(base_artifact_parse(artifact,an,&a));artifact[off]=old;
    }
    assert(!base_artifact_verify(artifact,an,catalog,cn,&policy,&a));
    policy.revision_floor=6;assert(base_artifact_verify(artifact,an,catalog,cn,&policy,&a));policy.revision_floor=5;
    assert(base_artifact_verify(artifact,an,catalog,cn,&policy,&a));assert(!crypto_sha256(catalog,cn,policy.floor_sha256));
    assert(!base_artifact_verify(artifact,an,catalog,cn,&policy,&a));policy.revision_floor=0;
    /* Construct personalized installation using the actual archive/FAT code. */
    size_t written;assert(!install_archive_personalize(factory,factory_n,recovery,sizeof recovery-1,boot,BASE_BANK_BYTES,&written));
    char menu[4096];size_t mn=base_update_menu(menu,sizeof menu,"12345678-1234-4234-8234-123456789abc",0);assert(mn);
    memcpy(prefix,disk_bytes+4096*512,BASE_ESP_BYTES);
    install_fat_patch_t patch={.bootstrap=boot,.bootstrap_size=BASE_BANK_BYTES,.config=menu,.config_size=mn};uint64_t ready_lba;
    assert(!install_fat_personalize(prefix,BASE_ESP_BYTES,&patch,&work.disk.fat,&ready_lba));
    /* Locate the primary bank via the independently inspected FAT. */
    install_fat_work_t *fat=&work.disk.fat;
    make_ready(prefix+fat->data+(fat->kernel_first-2)*fat->cluster_bytes,boot);
    uint8_t platform[32];memset(platform,0x42,32);
    assert(!base_update_seed(prefix,BASE_ESP_BYTES,&work,1,1,platform,ready));
    for(unsigned i=0;i<2;i++)memcpy(prefix+work.disk.readiness[i],ready,512);
    memcpy(disk_bytes+4096*512,prefix,BASE_ESP_BYTES);
    kernel_update_io_t io={.sectors=disk_size/512,.read=rd,.write=wr,.flush=flush};
    assert(!base_update_prepare(&io,prefix,&work,ready));assert(!work.mixed&&!work.invalid&&work.running==0);saved=work;
    memcpy(initial,disk_bytes+4096*512,BASE_ESP_BYTES);
    base_state release={.build=a.build,.abi=a.abi,.catalog=5,.root=1};memcpy(release.platform,a.platform,32);
    assert(!crypto_sha256(artifact,an,release.artifact));memset(release.catalog_hash,0x11,32);memset(release.root_hash,0x22,32);
    base_artifact wrong=a;wrong.predecessor_min=2;events=0;
    assert(base_update_apply(&io,prefix,&work,&wrong,&release,recovery,sizeof recovery-1));assert(!events);restore();
    assert(!base_update_apply(&io,prefix,&work,&a,&release,recovery,sizeof recovery-1));unsigned total=events;assert(total>4100);
    /* Every selector change may expose only complete independently validated
     * kernel+bootstrap+readiness+release records. Tear/flush cuts are bounded. */
    unsigned cuts[]={1,2,3,1026,2050,2051,2052,3076,4099,4100,4101,4102,4103,4104,4105,4106,4107,4108,4109,4110,4111,4112,4113,4114};
    for(unsigned j=0;j<sizeof cuts/sizeof cuts[0];j++){
        restore();cut=cuts[j];assert(base_update_apply(&io,prefix,&work,&a,&release,recovery,sizeof recovery-1));
        uint8_t *esp=disk_bytes+4096*512;
        assert(!memcmp(esp+saved.disk.kernel[0],initial+saved.disk.kernel[0],BASE_BANK_BYTES));
        assert(!memcmp(esp+saved.bootstrap[0],initial+saved.bootstrap[0],BASE_BANK_BYTES));
        assert(!memcmp(esp+saved.disk.readiness[0],ready,512));
        for(unsigned m=0;m<4;m++)if(esp[saved.disk.configs[m]+15]=='3'){
            base_state candidate;assert(!base_state_decode(esp+saved.state_offset[1],&candidate)&&candidate.phase==BASE_STATE_READY);
            root_recovery_t r;assert(root_recovery_parse(recovery,sizeof recovery-1,&r));
            assert(root_readiness_validate(esp+saved.disk.readiness[1],512,&r,esp+saved.disk.kernel[1],BASE_BANK_BYTES,esp+saved.bootstrap[1],BASE_BANK_BYTES));
        }
        cut=0;assert(!base_update_prepare(&io,prefix,&work,ready));
        assert(work.floors[work.floor].build==(cuts[j]==1?1:2));
        if(work.invalid){assert(!base_update_select(&io,prefix,&work,true));assert(!base_update_prepare(&io,prefix,&work,ready)&&!work.invalid&&work.floors[work.floor].build==2);}
        if(work.mixed){assert(!base_update_select(&io,prefix,&work,true));assert(!base_update_prepare(&io,prefix,&work,ready)&&!work.mixed&&work.disk.active==0);}
    }
    restore();assert(!base_update_apply(&io,prefix,&work,&a,&release,recovery,sizeof recovery-1));
    assert(!base_update_prepare(&io,prefix,&work,ready)&&work.disk.active==1&&work.running==0);
    assert(base_update_apply(&io,prefix,&work,&a,&release,recovery,sizeof recovery-1)); /* pending reboot */
    uint8_t next_ready[512];memcpy(next_ready,disk_bytes+4096*512+saved.disk.readiness[1],512);
    assert(!base_update_prepare(&io,prefix,&work,next_ready)&&work.running==1);
    events=0;cut=1;assert(base_update_select(&io,prefix,&work,false));cut=0;
    assert(!base_update_prepare(&io,prefix,&work,next_ready)&&work.running==1&&work.states[1].phase==BASE_STATE_READY);
    assert(!base_update_select(&io,prefix,&work,false));
    assert(!base_update_prepare(&io,prefix,&work,next_ready)&&work.states[1].phase==BASE_STATE_CONFIRMED);
    size_t an3;uint8_t *artifact3=load(argv[6],&an3);base_artifact a3;assert(!base_artifact_parse(artifact3,an3,&a3));
    saved=work;release.build=a3.build;release.abi=a3.abi;release.catalog=6;assert(!crypto_sha256(artifact3,an3,release.artifact));
    assert(!base_update_apply(&io,prefix,&work,&a3,&release,recovery,sizeof recovery-1));
    assert(!memcmp(disk_bytes+4096*512+saved.disk.kernel[1],prefix+saved.disk.kernel[1],BASE_BANK_BYTES));
    assert(!memcmp(disk_bytes+4096*512+saved.bootstrap[1],prefix+saved.bootstrap[1],BASE_BANK_BYTES));
    FILE *out=fopen(argv[7],"wb");assert(out&&fwrite(disk_bytes+4096*512,1,BASE_ESP_BYTES,out)==BASE_ESP_BYTES&&!fclose(out));
    printf("PASS signed base format, wrong signature/hash, predecessor zero-write rejection, %zu tear/flush boundaries, ESP write-range oracle, complete old/new sets, mixed-selector recovery, boot confirmation, two consecutive upgrades (%u I/O events)\n",sizeof cuts/sizeof cuts[0],total);
    free(artifact3);free(artifact);free(catalog);free(key);free(factory);free(boot);free(prefix);free(initial);munmap(disk_bytes,disk_size);return 0;
}
