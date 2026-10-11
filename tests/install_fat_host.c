#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "install_fat.h"
static uint8_t image[INSTALL_FAT_BYTES],saved[INSTALL_FAT_BYTES];
static install_fat_work_t work;
static void reject(unsigned offset,uint8_t xor) {
    memcpy(image,saved,sizeof(image));image[offset]^=xor;
    uint8_t *before=malloc(sizeof(image));assert(before);memcpy(before,image,sizeof(image));
    const char data[]="test";install_fat_patch_t p={data,data,4,4};uint64_t lba=UINT64_MAX;
    assert(install_fat_personalize(image,sizeof(image),&p,&work,&lba));
    assert(!memcmp(image,before,sizeof(image)) && lba==UINT64_MAX);free(before);
}
int main(int argc,char **argv) {
    assert(argc==3);FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(image,1,sizeof(image),f)==sizeof(image));assert(fgetc(f)==EOF);fclose(f);
    assert(!install_fat_validate(image,sizeof(image),&work));memcpy(saved,image,sizeof(image));
    uint32_t fat=work.fat,dir=work.entry[0];
    reject(11,1);reject(16,1);reject(40,1);reject(510,1);reject(fat+work.fat_sectors*512+8,1);
    reject(work.fsinfo,1);reject(work.backup,1);reject(dir+28,1);reject(dir-32+13,1);
    memcpy(image,saved,sizeof(image));
    const char bootstrap[]="native-bootstrap-bytes\n",config[]="timeout: 0\n/FortressOS\n";
    install_fat_patch_t p={bootstrap,config,sizeof(bootstrap)-1,sizeof(config)-1};uint64_t lba=0;
    assert(!install_fat_personalize(image,sizeof(image),&p,&work,&lba));
    assert(lba && !memcmp(image+lba*512,(uint8_t[512]){0},512));
    f=fopen(argv[2],"wb");assert(f);assert(fwrite(image,1,sizeof(image),f)==sizeof(image));assert(!fclose(f));
    puts("PASS native FAT32 graph/VFAT/slot validation, zero-mutation rejection, shrinking and readiness slot");
}
