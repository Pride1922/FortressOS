/* Exercise production transport splitting without granting hardware authority. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/install_nvme.c"
static unsigned calls,fail_at;static uint64_t next_lba;static uint8_t *next_buffer;
uint64_t storage_get_sector_count(void){return 100;}
static bool transfer(uint64_t l,uint32_t n,const void *p){
 assert(n && n<=8 && l==next_lba && p==next_buffer);calls++;next_lba+=n;next_buffer+=n*512;
 return calls!=fail_at;
}
bool storage_read_sectors(uint64_t l,uint32_t n,void *p){return transfer(l,n,p);}
bool storage_write_sectors(uint64_t l,uint32_t n,const void *p){return transfer(l,n,p);}
/* Rendering is unreachable because deploying remains false. */
void console_terminal_write(const char *p,size_t n){(void)p;(void)n;}
static void reset(uint64_t l,void *p,unsigned cut){failed=false;calls=0;fail_at=cut;next_lba=l;next_buffer=p;}
int main(void){
 uint8_t bytes[16384];
 for(unsigned n=1;n<=32;n++){
  reset(100-n,bytes,0);assert(!read_run(NULL,100-n,n,bytes));assert(calls==(n+7)/8);
  reset(100-n,bytes,0);assert(!write_run(NULL,100-n,n,bytes));assert(calls==(n+7)/8);
 }
 for(unsigned cut=1;cut<=4;cut++){
  reset(1,bytes,cut);assert(read_run(NULL,1,32,bytes)<0&&failed&&calls==cut);assert(read_run(NULL,1,32,bytes)<0&&calls==cut);
  reset(1,bytes,cut);assert(write_run(NULL,1,32,bytes)<0&&failed&&calls==cut);assert(write_run(NULL,1,32,bytes)<0&&calls==cut);
 }
 reset(0,bytes,0);
 assert(read_run(NULL,0,0,bytes)<0);assert(read_run(NULL,0,33,bytes)<0);assert(read_run(NULL,100,1,bytes)<0);assert(read_run(NULL,99,2,bytes)<0);assert(read_run(NULL,UINT64_MAX,32,bytes)<0);assert(read_run(NULL,0,1,NULL)<0);
 assert(write_run(NULL,99,2,bytes)<0);assert(!calls);
 puts("PASS installer transport: complete bounds, 1..32 sectors, 4 KiB hardware runs, every failed prefix fences further I/O");return 0;
}
