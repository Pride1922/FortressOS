/* Host payload producer uses the actual native validator before sealing hashes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "install_fat.h"
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    uint8_t *bytes=malloc(INSTALL_FAT_BYTES);install_fat_work_t *work=calloc(1,sizeof(*work));assert(bytes && work);
    assert(fread(bytes,1,INSTALL_FAT_BYTES,f)==INSTALL_FAT_BYTES && fgetc(f)==EOF);fclose(f);
    int result=install_fat_validate(bytes,INSTALL_FAT_BYTES,work);
    if (!result) puts("PASS actual native fixed-slot FAT32 template validation");
    free(work);free(bytes);return result ? 1 : 0;
}
