/* Real TarFS validator with publication/allocator adapters. No boot/IRQ claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/fs/tarfs.c"
static unsigned publications;
void serial_puts(const char *s) {(void)s;}
void serial_print_dec(uint64_t n) {(void)n;}
void serial_print_hex(uint64_t n) {(void)n;}
void serial_putc(char c) {(void)c;}
void vfs_init(void) {publications++;}
vfs_node_t *vfs_create_node(const char *p,vfs_node_type_t t,uint64_t n,const void *d) {
    (void)p;(void)t;(void)n;(void)d;assert(0);return NULL;
}
static void checksum(unsigned char *data) {
    struct ustar_header *h=(void *)data;memset(h->chksum,' ',8);
    unsigned sum=0;for(unsigned i=0;i<512;i++)sum+=data[i];
    snprintf(h->chksum,8,"%06o",sum);h->chksum[7]=' ';
}
int main(void) {
    unsigned char seed[1536]={0},text[1536];struct ustar_header *h=(void *)seed;
    memcpy(h->magic,"ustar",5);memcpy(h->version,"00",2);memcpy(h->name,"bin/sudo",9);
    memcpy(h->mode,"0004755",7);memcpy(h->uid,"0000000",7);memcpy(h->gid,"0000000",7);
    memcpy(h->size,"00000000000",11);h->typeflag='0';checksum(seed);
    assert(!tarfs_validate(seed,sizeof(seed)));
    unsigned rng=0x5590,denied=0;
    for(unsigned i=0;i<30000;i++) {
        memcpy(text,seed,sizeof(text));rng=rng*1664525+1013904223;
        text[rng%512]^=(unsigned char)((rng>>24)|1);
        if(i&1)checksum(text);
        size_t length=i%3 ? sizeof(text):rng%sizeof(text);
        int result=tarfs_validate(text,length);
        if(result) {assert(tarfs_init(text,length)<0 && !publications);denied++;}
    }
    memcpy(text,seed,sizeof(text));memset(((struct ustar_header *)text)->prefix,'p',155);
    memset(((struct ustar_header *)text)->name,'n',100);checksum(text);
    assert(tarfs_validate(text,sizeof(text))<0);
    memcpy(text,seed,sizeof(text));memcpy(((struct ustar_header *)text)->name,"../bin/sudo",12);checksum(text);
    assert(tarfs_validate(text,sizeof(text))<0);
    /* A valid first entry cannot publish before a malformed later header. */
    memcpy(text,seed,sizeof(text));text[512]=1;
    assert(tarfs_init(text,sizeof(text))<0 && !publications);
    printf("PASS 30000 deterministic USTAR header/length mutations, %u rejected before publication\n",denied);
}
