/* Actual private installer adapters; mock identity and device callbacks only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/install_runtime.c"
bool block_read_sector(block_dev_t *d,uint64_t l,void *p){return d->read_sector(d,l,p);}
bool block_write_sector(block_dev_t *d,uint64_t l,const void *p){return d->write_sector(d,l,p);}
static install_identity_v1_t identity_value;
static unsigned identities,reads,writes;static int identity_error,read_error,write_error;
static uint64_t seen_lba;static uint32_t seen_count;
static int identity_mock(void *c,install_identity_v1_t *out){(void)c;identities++;*out=identity_value;return identity_error;}
static int read_mock(void *c,uint64_t l,uint32_t n,void *p){(void)c;reads++;seen_lba=l;seen_count=n;memset(p,0x5a,n*512);return read_error;}
static int write_mock(void *c,uint64_t l,uint32_t n,const void *p){(void)c;(void)p;writes++;seen_lba=l;seen_count=n;return write_error;}
int main(void){
 install_backend_t backend={.identity=identity_mock,.read=read_mock,.write=write_mock};
 runtime_t owner={.backend=&backend};owner.identity=identity_value;
 block_dev_t dev={.sector_size=512,.sector_count=100,.priv=&owner,.read_sector=part_read,.write_sector=part_write,.read_sectors=part_read_run,.write_sectors=part_write_run,.max_run_bytes=4096};
 uint8_t bytes[4096];assert(block_read_sectors(&dev,92,8,bytes));assert(identities==1&&reads==1&&seen_count==8&&seen_lba==INSTALL_ROOT_LBA+92);
 assert(block_write_sectors(&dev,1,8,bytes));assert(identities==2&&writes==1&&seen_count==8);
 unsigned before=identities;
 assert(!part_read_run(NULL,0,1,bytes));assert(!part_read_run(&dev,0,0,bytes));assert(!part_read_run(&dev,0,9,bytes));assert(!part_write_run(&dev,93,8,bytes));assert(!part_read_run(&dev,100,1,bytes));assert(!part_read_run(&dev,UINT64_MAX-1,8,bytes));assert(!part_read_run(&dev,0,1,NULL));assert(identities==before);
 dev.sector_count=UINT64_MAX;assert(!part_read_run(&dev,UINT64_MAX-2,1,bytes));assert(identities==before);dev.sector_count=100;
 identity_value.generation++;assert(!part_read_run(&dev,0,8,bytes));assert(reads==1&&identities==before+1);identity_value=owner.identity;
 identity_error=-5;assert(!part_write_run(&dev,0,8,bytes));assert(writes==1);identity_error=0;
 read_error=-5;before=reads;assert(!block_read_sectors(&dev,0,8,bytes));assert(reads==before+1);read_error=0;
 write_error=-5;before=writes;assert(!block_write_sectors(&dev,0,8,bytes));assert(writes==before+1);write_error=0;
 dev.read_sectors=NULL;before=identities;unsigned prior_reads=reads;assert(block_read_sectors(&dev,0,8,bytes));assert(identities==before+8&&reads==prior_reads+8);
 uint8_t payload[16384];before=identities;prior_reads=reads;
 assert(!raw_read(&owner,1,32,payload));assert(identities==before+1&&reads==prior_reads+1&&seen_count==32);
 identity_value.generation++;before=reads;assert(raw_read(&owner,1,32,payload)<0&&reads==before);identity_value=owner.identity;
 puts("PASS actual installer partition: one identity per bounded run, complete bounds before I/O, changed identity refusal and no failure retry");return 0;
}
