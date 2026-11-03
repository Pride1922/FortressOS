/* Actual JBD2 preview with an already-validated synthetic plan and mock disk.
 * Parser/replay/crash-cut coverage remains in the existing journal suites. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/fs/jbd2.c"
static uint8_t disk[65536],expected[65536];
static unsigned singles,runs;static bool fail_run;
bool block_read_sector(block_dev_t *d,uint64_t l,void *b){assert(l<d->sector_count);singles++;memcpy(b,disk+l*d->sector_size,d->sector_size);return true;}
static bool read_run(block_dev_t *d,uint64_t l,uint32_t n,void *b){runs++;assert(n&&n<=4096/d->sector_size&&l<d->sector_count&&n<=d->sector_count-l);if(fail_run){memset(b,0x12,128);return false;}memcpy(b,disk+l*d->sector_size,n*d->sector_size);return true;}
static uint32_t reference(uint32_t c,const uint8_t *p,size_t n){while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0x82f63b78u&(0u-(c&1)));}return c;}
int main(void){
 for(unsigned ss=512;ss<=4096;ss*=8)for(unsigned bs=1024;bs<=4096;bs*=4){
  block_dev_t dev={.sector_size=ss,.sector_count=sizeof disk/ss,.read_sector=block_read_sector,.read_sectors=read_run,.max_run_bytes=4096};
  jbd2_plan_t *p=calloc(1,sizeof(*p));assert(p);p->dev=&dev;p->bs=bs;p->blocks=sizeof disk/bs;p->super_block=8;p->first=1;p->report.next_sequence=42;
  for(unsigned i=0;i<sizeof disk;i++)disk[i]=(uint8_t)(i*17+3);
  memcpy(expected,disk,sizeof disk);
  uint8_t images[3][4096];memset(images[0],0x62,4096);memset(images[1],0x22,4096);memset(images[2],0x63,4096);
  p->images=3;p->image[0]=(j_image_t){6,1,images[0]};p->image[1]=(j_image_t){2,3,images[1]};p->image[2]=(j_image_t){6,4,images[2]};
  p->revokes=1;p->revoke[0]=(j_revoke_t){2,4};memcpy(expected+6*bs,images[2],bs);
  memset(p->super,0x99,bs);j_put(p->super+28,100);
  uint8_t synthetic[4096];memcpy(synthetic,p->super,bs);j_put(synthetic+24,42);j_put(synthetic+28,0);j_put(synthetic+88,1);j_put(synthetic+252,0);j_put(synthetic+252,reference(UINT32_MAX,synthetic,1024));memcpy(expected+8*bs,synthetic,bs);
  for(unsigned n=1;n<=4096/ss;n++)for(uint64_t l=0;l+n<=dev.sector_count;l++){
   uint8_t buffer[4098];memset(buffer,0xa5,sizeof buffer);singles=runs=0;
   assert(jbd2_preview_read_sectors(p,l,n,buffer+1));assert(!memcmp(buffer+1,expected+l*ss,n*ss));assert(buffer[0]==0xa5&&buffer[1+n*ss]==0xa5);
   assert(singles==(n==1)&&runs==(n>1));
  }
  uint8_t buffer[4096];singles=runs=0;
  assert(!jbd2_preview_read_sectors(p,0,0,buffer));assert(!jbd2_preview_read_sectors(p,0,4096/ss+1,buffer));
  assert(!jbd2_preview_read_sectors(p,dev.sector_count,1,buffer));assert(!jbd2_preview_read_sectors(p,UINT64_MAX,1,buffer));assert(!jbd2_preview_read_sectors(NULL,0,1,buffer));assert(!jbd2_preview_read_sectors(p,0,1,NULL));
  uint64_t saved_count=dev.sector_count;dev.sector_count=UINT64_MAX;
  assert(!jbd2_preview_read_sectors(p,UINT64_MAX/ss+1,1,buffer));
  assert(!jbd2_preview_read_sectors(p,UINT64_MAX/ss,1,buffer));dev.sector_count=saved_count;
  p->poisoned=true;assert(!jbd2_preview_read_sectors(p,0,1,buffer));p->poisoned=false;assert(!singles&&!runs);
  if(ss==512){
   fail_run=true;assert(!jbd2_preview_read_sectors(p,(6u*bs)/ss,8,buffer));assert(runs==1&&!singles&&buffer[0]==0x12);fail_run=false;
   dev.read_sectors=NULL;singles=runs=0;assert(jbd2_preview_read_sectors(p,0,8,buffer));assert(singles==8&&!runs&&!memcmp(buffer,expected,4096));
   dev.max_run_bytes=2048;singles=runs=0;assert(!jbd2_preview_read_sectors(p,0,8,buffer));assert(!singles&&!runs);
  }
  free(p);
 }
 puts("PASS JBD2 bulk preview: overlaps, revoked/superseded images, synthetic superblock, 512/4096 sectors, bounds, canaries and no failure retry");return 0;
}
