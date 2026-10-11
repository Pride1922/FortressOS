#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "install_archive.h"
/* Test-only header constructor is independent of the native implementation. */
static void header(unsigned char h[512],const char *name,unsigned type,size_t size) {
    memset(h,0,512);assert(strlen(name)<100);memcpy(h,name,strlen(name));
    snprintf((char *)h+100,8,"%07o",0755);snprintf((char *)h+108,8,"%07o",0);
    snprintf((char *)h+116,8,"%07o",0);snprintf((char *)h+124,12,"%011lo",(unsigned long)size);
    snprintf((char *)h+136,12,"%011o",0);memset(h+148,' ',8);h[156]=(unsigned char)type;
    memcpy(h+257,"ustar\0" "00",8);unsigned sum=0;for (unsigned i=0;i<512;i++) sum+=h[i];
    snprintf((char *)h+148,7,"%06o",sum);h[155]=' ';
}
static const char record[]="FORTRESS-RECOVERY-1\ninstall_id=00112233445566778899aabbccddeeff\n"
    "root_partuuid=11223344-5566-7788-99aa-bbccddeeff00\nfilesystem_uuid=11223344-5566-7788-99aa-bbccddeeff00\n"
    "password_hash=$5$rounds=100000$1234567890abcdef$...........................................\n";
int main(int argc,char **argv) {
    unsigned char src[4096]={0},dst[8192];size_t written;
    header(src,"bin",'5',0);header(src+512,"bin/shell",'0',3);memcpy(src+1024,"ELF",3);
    assert(!install_archive_personalize(src,4096,record,sizeof(record)-1,dst,sizeof(dst),&written));
    assert(written==4096 && !memcmp(dst,src,1536));
    assert(!strcmp((char *)dst+1536,"bootstrap") && !strcmp((char *)dst+2048,"bootstrap/recovery.v1"));
    assert(!memcmp(dst+2560,record,sizeof(record)-1));
    unsigned cases=1;
    for (unsigned i=0;i<1536;i++) {
        /* Data bytes can change; header, padding and trailer corruptions cannot. */
        if (i>=1024 && i<1027) continue;
        unsigned char old=src[i];src[i]^=128;
        int r=install_archive_personalize(src,4096,record,sizeof(record)-1,dst,sizeof(dst),&written);
        /* uname/gname are optional text; other semantically unused fields can
         * change if header checksum is also updated. This single mutation never
         * preserves the original header checksum. */
        assert(r<0 && !written);for (size_t j=0;j<sizeof(dst);j++) assert(!dst[j]);
        src[i]=old;cases++;
    }
    const char *bad[]={"/bin","../bin","bin/../shell","bin//shell","bin/./shell","bootstrap","bootstrap/recovery.v1","bin/"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        memset(src,0,sizeof(src));header(src,bad[i],'0',0);
        assert(install_archive_personalize(src,sizeof(src),record,sizeof(record)-1,dst,sizeof(dst),&written)<0);cases++;
    }
    memset(src,0,sizeof(src));header(src,"bin",'5',0);header(src+512,"bin/",'5',0);
    assert(install_archive_personalize(src,sizeof(src),record,sizeof(record)-1,dst,sizeof(dst),&written)<0);cases++;
    header(src+512,"bin/shell",'2',0);
    assert(install_archive_personalize(src,sizeof(src),record,sizeof(record)-1,dst,sizeof(dst),&written)<0);cases++;
    header(src,"bin",'0',0);header(src+512,"bin/shell",'0',0);
    assert(install_archive_personalize(src,sizeof(src),record,sizeof(record)-1,dst,sizeof(dst),&written)<0);cases++;
    memset(src,0,sizeof(src));header(src,"bin",'5',0);src[2048]=1;
    assert(install_archive_personalize(src,sizeof(src),record,sizeof(record)-1,dst,sizeof(dst),&written)<0);cases++;
    src[2048]=0;
    assert(install_archive_personalize(src,sizeof(src),record,sizeof(record)-2,dst,sizeof(dst),&written)<0);cases++;
    assert(install_archive_personalize(src,sizeof(src),record,sizeof(record)-1,src,sizeof(src),&written)<0);cases++;
    assert(!install_archive_personalize(src,sizeof(src),record,sizeof(record)-1,dst,sizeof(dst),&written));
    if (argc==2) {FILE *f=fopen(argv[1],"wb");assert(f && fwrite(dst,1,written,f)==written && !fclose(f));}
    printf("installer archive workbench: %u cases PASS\n",cases);return 0;
}
