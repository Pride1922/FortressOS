#include "install_log.h"
#include "vfs.h"
#include "string.h"
#include "../../user/tools/digest.h"
static char log_bytes[INSTALL_LOG_BYTES];
static size_t used;
static block_dev_t *device;
static bool failed;
static uint8_t scratch[512],array[16384];
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t le64(const uint8_t *p) {return (uint64_t)le32(p)|((uint64_t)le32(p+4)<<32);}
static uint32_t crc(const uint8_t *p,size_t n) {
    uint32_t c=UINT32_MAX;for (size_t i=0;i<n;i++) {c^=p[i];for (unsigned b=0;b<8;b++) c=(c>>1)^((0u-(c&1))&0xedb88320u);}return ~c;
}
static bool token(const boot_info_t *b,const char *key,char *out,size_t size) {
    if (!b || b->cmdline_truncated) return false;
    size_t k=strlen(key);unsigned found=0;
    for (size_t at=0;at<sizeof(b->cmdline) && b->cmdline[at];) {
        while (at<sizeof(b->cmdline) && b->cmdline[at]==' ') at++;
        size_t start=at;while (at<sizeof(b->cmdline) && b->cmdline[at] && b->cmdline[at]!=' ') at++;
        size_t n=at-start;
        if (n>=k && !memcmp(b->cmdline+start,key,k)) {
            if (++found!=1 || n-k!=size-1) return false;
            memcpy(out,b->cmdline+start+k,size-1);out[size-1]=0;
        }
    }
    return found==1;
}
bool install_log_requested(const boot_info_t *b) {
    char h[65];return b && b->installer_source_valid && !b->installer_source_optical && token(b,"installer_log_sha256=",h,sizeof(h));
}
static bool decode(const char *s,uint8_t *out,size_t n) {
    for (size_t i=0;i<n;i++) {
        unsigned v=0;for (unsigned j=0;j<2;j++) {char c=s[i*2+j];unsigned x=c>='0' && c<='9' ? (unsigned)(c-'0') : c>='a' && c<='f' ? (unsigned)(c-'a'+10) : 16;if (x==16) return false;v=v*16+x;}out[i]=(uint8_t)v;
    }return true;
}
static void append(const char *s) {
    size_t n=strlen(s);if (n>=sizeof(log_bytes)-64) return;
    /* Preserve the format header and the newest events across supervisor retries. */
    const size_t header=23;
    while (n>=sizeof(log_bytes)-used && used>header) {
        size_t end=header;while (end<used && log_bytes[end]!='\n') end++;
        if (end<used) end++;
        memmove(log_bytes+header,log_bytes+end,used-end);used-=end-header;log_bytes[used]=0;
    }
    memcpy(log_bytes+used,s,n);used+=n;log_bytes[used]=0;
}
static void number(int64_t value) {
    char digits[24];unsigned n=0;uint64_t v=value<0 ? 0-(uint64_t)value : (uint64_t)value;
    if (value<0) append("-");
    do {digits[n++]=(char)('0'+v%10);v/=10;} while (v);
    while (n) {char c[2]={digits[--n],0};append(c);}
}
static void start(void) {used=0;memset(log_bytes,0,sizeof(log_bytes));append("FORTRESS-INSTALL-LOG-1\nSession started\n");}
static bool usb_failure(const char *reason,uint64_t lba,bool sector) {
    if (!used) start();
    append("USB log unavailable: ");append(reason);
    if (sector) {append("; sector=");number((int64_t)lba);}
    append(". USB logging disabled for this boot; installation can continue.\n");
    failed=true;return false;
}
void install_log_unavailable(install_log_unavailable_t reason) {
    static const char *const reasons[]={
        "boot source is not a selected USB log image (ISO, unknown source or missing log configuration)",
        "no initialized USB mass-storage transport",
        "USB cache durability could not be established",
        "USB transport is read-only under the durability policy"};
    if ((unsigned)reason<sizeof(reasons)/sizeof(*reasons)) {
        device=NULL;(void)usb_failure(reasons[reason],0,false);
    }
}
static bool persist(void) {
    if (!device || failed) return false;
    uint8_t bytes[512];
    /* File size/allocation never change. Space padding leaves a normal text file. */
    for (unsigned i=0;i<INSTALL_LOG_BYTES/512;i++) {
        memset(bytes,' ',sizeof(bytes));size_t at=i*512,n=used>at ? used-at : 0;if (n>512) n=512;
        memcpy(bytes,log_bytes+at,n);
        if (i==INSTALL_LOG_BYTES/512-1) bytes[511]='\n';
        uint64_t lba=INSTALL_LOG_VOLUME_LBA+INSTALL_LOG_DATA_LBA+i;
        if (!block_write_sector(device,lba,bytes)) return usb_failure("writing the log data failed",lba,true);
    }
    if (!block_flush(device)) return usb_failure("flushing USB writes failed",0,false);
    for (unsigned i=0;i<INSTALL_LOG_BYTES/512;i++) {
        memset(bytes,' ',sizeof(bytes));size_t at=i*512,n=used>at ? used-at : 0;if (n>512) n=512;
        memcpy(bytes,log_bytes+at,n);
        if (i==INSTALL_LOG_BYTES/512-1) bytes[511]='\n';
        uint64_t lba=INSTALL_LOG_VOLUME_LBA+INSTALL_LOG_DATA_LBA+i;
        if (!block_read_sector(device,lba,scratch)) return usb_failure("reading back the written log failed",lba,true);
        if (memcmp(bytes,scratch,512)) return usb_failure("written log did not match its read-back",lba,true);
    }
    return true;
}
bool install_log_bind(block_dev_t *d,const boot_info_t *b) {
    device=NULL;failed=false;start();
    const char *reason="USB log configuration, geometry or write/flush support is invalid";
    char text[65];uint8_t expected[32];
    if (!install_log_requested(b) || !token(b,"installer_log_sha256=",text,sizeof(text)) || !decode(text,expected,32) ||
        !d || d->sector_size!=512 || d->sector_count<(528u*2048u) || !d->write_sector || !d->flush) return usb_failure(reason,0,false);
    static const uint8_t fixed_metadata[32]={0x63,0x44,0xb6,0x60,0x05,0x85,0x9b,0x74,0x13,0x3e,0x2d,0x21,0x7b,0x46,0x76,0xbe,0xae,0x03,0x34,0x89,0xc7,0xad,0xdf,0x8b,0xc0,0x83,0x8b,0xc2,0x5a,0x82,0xe8,0x27};
    reason="USB log metadata hash is not the supported installer layout";
    if (memcmp(expected,fixed_metadata,32)) return usb_failure(reason,0,false);
    /* Validate both GPT copies at the image-declared boundary, also on larger
     * flashed sticks. No GPT repair, degraded fallback or registry publication. */
    uint64_t last;uint8_t primary[512];
    reason="reading the primary USB partition header failed";
    if (!block_read_sector(d,1,primary)) return usb_failure(reason,0,false);
    last=le64(primary+32);
    bool combined=last==592u*2048u-1;
    if ((!combined && last!=528u*2048u-1) || last>=d->sector_count) return usb_failure(reason,0,false);
    reason="primary USB partition header or boot-source identity does not match the installer image";
    uint32_t sum=le32(primary+16);memset(primary+16,0,4);
    if (memcmp(primary,"EFI PART",8) || le32(primary+8)!=0x10000 || le32(primary+12)!=92 || le32(primary+20) || crc(primary,92)!=sum ||
        le64(primary+24)!=1 || le64(primary+32)!=last || le64(primary+40)!=34 || le64(primary+48)!=last-33 ||
        memcmp(primary+56,b->installer_source_disk_guid,16) || le64(primary+72)!=2 || le32(primary+80)!=128 || le32(primary+84)!=128) return usb_failure(reason,0,false);
    reason="reading the USB partition entries failed";
    for (unsigned i=0;i<32;i++) if (!block_read_sector(d,2+i,array+i*512)) return usb_failure(reason,0,false);
    reason="USB partition entries checksum or log partition layout is invalid";
    if (crc(array,sizeof(array))!=le32(primary+88)) return usb_failure(reason,0,false);
    const uint64_t starts[]={4096,2048,INSTALL_LOG_VOLUME_LBA},counts[]={512u*2048u,2048,INSTALL_LOG_VOLUME_SECTORS};
    for (unsigned i=0;i<3;i++) {
        const uint8_t *e=array+i*128;
        if (le64(e+32)!=starts[i] || le64(e+40)!=starts[i]+counts[i]-1 || le64(e+48)) return usb_failure(reason,0,false);
        bool nonzero=false;for (unsigned j=0;j<16;j++) if (e[j]) nonzero=true;if (!nonzero) return usb_failure(reason,0,false);
    }
    if (combined) {
        static const uint8_t linux_type[16]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
        const uint8_t *e=array+384;bool nonzero=false;
        for (unsigned j=0;j<16;j++) if (e[16+j]) nonzero=true;
        if (memcmp(e,linux_type,16) || !nonzero || le64(e+32)!=522u*2048u ||
            le64(e+40)!=586u*2048u-1 || le64(e+48)) return usb_failure(reason,0,false);
        for (unsigned i=0;i<3;i++) if (!memcmp(e+16,array+i*128+16,16)) return usb_failure(reason,0,false);
    }
    for (size_t i=combined ? 512:384;i<sizeof(array);i++) if (array[i]) return usb_failure(reason,0,false);
    reason="reading the image-boundary backup USB partition header failed";
    if (!block_read_sector(d,last,scratch)) return usb_failure(reason,0,false);
    reason="backup USB partition header does not match the original image (the flashing tool may have relocated or changed GPT)";
    sum=le32(scratch+16);memset(scratch+16,0,4);
    if (crc(scratch,92)!=sum || le64(scratch+24)!=last || le64(scratch+32)!=1 || le64(scratch+72)!=last-32) return usb_failure(reason,0,false);
    /* Apart from locations and CRC, backup must be byte-identical. */
    memcpy(scratch+24,primary+24,16);memcpy(scratch+72,primary+72,8);
    if (memcmp(scratch,primary,512)) return usb_failure(reason,0,false);
    reason="backup USB partition entries could not be read or differ from the primary entries";
    for (unsigned i=0;i<32;i++) if (!block_read_sector(d,last-32+i,scratch) || memcmp(scratch,array+i*512,512)) return usb_failure(reason,0,false);
    /* Producer supplies the exact fixed FAT16 metadata hash in its installer
     * entry. This checks both FATs, directory/LFN, allocation and absence of aliases. */
    reason="reading USB log filesystem metadata failed";
    digest_t h;digest_init(&h,DIGEST_SHA256);
    for (unsigned i=0;i<INSTALL_LOG_DATA_LBA;i++) {
        if (!block_read_sector(d,INSTALL_LOG_VOLUME_LBA+i,scratch) || !digest_update(&h,scratch,512)) return usb_failure(reason,0,false);
    }
    reason="USB log filesystem metadata changed; the preallocated log slot cannot be verified";
    uint8_t actual[32];digest_final(&h,actual);if (memcmp(actual,expected,32)) return usb_failure(reason,0,false);
    reason="USB flush failed while validating the log volume";
    if (!block_flush(d)) return usb_failure(reason,0,false);
    device=d;return true;
}
int install_log_event(const install_log_event_v1_t *e) {
    if (!e || e->size!=sizeof(*e) || e->version!=1 || e->event>INSTALL_LOG_FAILED || e->stage>7 || e->reserved) return -VFS_EINVAL;
    if (e->event==INSTALL_LOG_START) {if (!used) start();else append("\nSession started\n");}
    else if (e->event==INSTALL_LOG_CANCELLED) append("Installation cancelled; no success reported\n");
    else if (e->event==INSTALL_LOG_FAILED) {append("Installation failed: stage=");number(e->stage);append(" error=");number(e->error);append(" call=");number(e->call);append("\n");}
    return persist() ? 0 : -VFS_EIO;
}
void install_log_target(const install_identity_v1_t *id,const char *user) {
    if (!id || id->sector_size!=512 || !user) return;
    if (!used) start();
    char model[41];unsigned n=40;memcpy(model,id->model,40);
    while (n && (model[n-1]==' ' || !model[n-1])) n--;
    for (unsigned i=0;i<n;i++) if ((uint8_t)model[i]<32 || (uint8_t)model[i]>126) model[i]='?';
    model[n]=0;
    append("disk=");append(model);append("\ncapacity_mib=");number((int64_t)(id->sector_count/(1024*1024/id->sector_size)));
    append("\nuser=");if (install_username_valid(user)) append(user);append("\nlayout=bios 1MiB, esp 64MiB; root size selected by deployment profile\n");(void)persist();
}
void install_log_stage(unsigned stage) {
    static const char *const labels[]={"Checking files","Checking selected disk","Preparing disk","Creating partitions","Copying files","Creating account","Verifying installed files","Ready to boot"};
    if (stage>7) return;
    append("stage=");number(stage);append(" ");append(labels[stage]);append("\n");(void)persist();
}
void install_log_result(unsigned stage,int error,bool complete) {
    append(complete && !error ? "result=installation complete\n" : "result=installation failed\n");
    if (error) {
        static const char *const reasons[]={"Installer file validation failed","Selected disk validation failed",
            "Preparing or writing the disk failed","Partition creation failed","File copying failed",
            "Root filesystem or account provisioning failed","Installed-file verification failed","Final boot readiness or log durability failed"};
        if (stage<8) {append(reasons[stage]);append("\n");}
        append("stage=");number(stage);append(" error=");number(error);append("\n");
    }(void)persist();
}
const char *install_log_snapshot(size_t *n) {if (!used) start();*n=used;return log_bytes;}
bool install_log_usb_ok(void) {return device && !failed;}
