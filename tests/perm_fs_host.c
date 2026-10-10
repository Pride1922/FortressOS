/* Actual VFS/TarFS/runfs/devfs with pthread lock adapters. No IRQ claim. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ext4_host/spinlock.h"
#define HOST_SPINLOCK_H
#include "../src/fs/gpt.h"
static size_t live;
void *kmalloc(size_t n) { void *p=malloc(n);if(p)live++;return p; }
void *kcalloc(size_t n,size_t z) { void *p=calloc(n,z);if(p)live++;return p; }
void kfree(void *p) { if(p){assert(live);live--;free(p);} }
void serial_puts(const char *s) { (void)s; }
void serial_print_dec(uint64_t n) { (void)n; }
void serial_print_hex(uint64_t n) { (void)n; }
void serial_putc(char c) { (void)c; }
void serial_raw_putc(char c) { (void)c; }
void console_terminal_write(const char *s,size_t n) { (void)s;(void)n; }
void console_inc_generation(void) {}
bool console_is_quiet(void) {return false;}
int64_t input_read(void *p,size_t n) {(void)p;(void)n;return 0;}
static block_dev_t usb={.name="sda"},nvme={.name="nvme0"};
static gpt_partition_t parts[2];
size_t gpt_get_partition_count(void) {return 2;}
gpt_partition_t *gpt_get_partition(size_t i) {return &parts[i];}
bool block_read_sector(block_dev_t *d,uint64_t l,void *p) {assert(l<d->sector_count);memset(p,(int)l,d->sector_size);return true;}
/* Routing-only adapter here; actual filesystem lock ownership has its own test. */
static spinlock_t fake_device_lock=SPINLOCK_RANKED(1,"mock-device");
bool ext4_device_read_sector(block_dev_t *d,uint64_t l,void *b,bool *handled) {
 (void)d;(void)l;(void)b;*handled=false;return false;
}
bool ext2_device_read_sector(block_dev_t *d,uint64_t l,void *b) {
 uint64_t flags=spin_lock_irqsave(&fake_device_lock);bool ok=block_read_sector(d,l,b);
 spin_unlock_irqrestore(&fake_device_lock,flags);return ok;
}
#include "../src/fs/runfs.c"
#include "../src/fs/devfs.c"
#include "../src/fs/vfs.c"
#include "../src/fs/tarfs.c"
static void check(const char *path,uint32_t mode,uint32_t uid,uint32_t gid) {
 int e;vfs_node_t *n=vfs_lookup_ref(path,&e);assert(n && !e);
 vfs_metadata_t m;assert(!vfs_metadata(n,&m));assert(m.mode==mode && m.uid==uid && m.gid==gid && !m.reserved);
 vfs_stat_t old;assert(!vfs_stat(n,&old) && old.mode==mode);vfs_node_put(n);
}

static void *append_worker(void *arg) {
 file_t *file=arg;for(unsigned i=0;i<500;i++) assert(vfs_write(file,"xy",2)==2);return NULL;
}
int main(void) {
 vfs_init();check("/run",VFS_S_IFDIR|0755,0,0);check("/tmp",VFS_S_IFDIR|01777,0,0);
 check("/dev/tty",VFS_S_IFCHR|0666,0,5);check("/dev/console",VFS_S_IFCHR|0620,0,5);
 check("/dev/null",VFS_S_IFCHR|0666,0,0);
 parts[0].parent=&usb;parts[0].block_dev=(block_dev_t){.name="sdap1",.sector_size=512,.sector_count=8};
 parts[1].parent=&nvme;parts[1].block_dev=(block_dev_t){.name="nvme0p1",.sector_size=512,.sector_count=8};
 devfs_add_usb_partitions();check("/dev/sdap1",VFS_S_IFBLK|0660,0,6);assert(!vfs_lookup("/dev/nvme0p1"));
 file_t *f=vfs_open("/dev/sdap1",VFS_O_RDONLY);assert(f);unsigned char b[4096];f->offset=511;
 assert(vfs_read(f,b,3)==3 && !b[0] && b[1]==1 && b[2]==1);assert(vfs_write(f,b,1)<0);vfs_close(f);
 int e;vfs_create_attrs_t a={.mode=07777,.uid=0x12345678,.gid=0x87654321};
 vfs_node_t *n=vfs_create_attrs_ref("/run/a",VFS_FILE,&a,&e);assert(n && !e);vfs_node_put(n);
 check("/run/a",VFS_S_IFREG|07777,a.uid,a.gid);
 f=vfs_open("/run/a",VFS_O_RDWR);assert(f);assert(vfs_write(f,"abc",3)==3);
 assert(!vfs_unlink("/run/a"));assert(!vfs_lookup_ref("/run/a",&e) && e==-VFS_ENOENT);
 f->offset=0;assert(vfs_read(f,b,3)==3 && !memcmp(b,"abc",3));
 assert(!vfs_truncate(f->node,4096));f->offset=3;assert(vfs_read(f,b,4093)==4093);
 for(unsigned i=0;i<4093;i++) { assert(!b[i]); }
 assert(vfs_write(f,"x",1)==-VFS_ENOSPC);
 assert(vfs_truncate(f->node,4097)==-VFS_EFBIG);vfs_close(f);
 for(unsigned i=0;i<61;i++){char p[32];snprintf(p,sizeof(p),"/run/n%u",i);n=vfs_create_attrs_ref(p,VFS_FILE,&a,&e);assert(n);vfs_node_put(n);}
 assert(!vfs_create_attrs_ref("/run/full",VFS_FILE,&a,&e) && e==-VFS_ENOSPC);
 for(unsigned i=0;i<61;i++){char p[32];snprintf(p,sizeof(p),"/run/n%u",i);assert(!vfs_unlink(p));}
 for(unsigned mode=0;mode<=07777;mode++){a.mode=mode;n=vfs_create_attrs_ref("/tmp/m",VFS_FILE,&a,&e);assert(n);vfs_node_put(n);check("/tmp/m",VFS_S_IFREG|mode,a.uid,a.gid);assert(!vfs_unlink("/tmp/m"));}

 f=vfs_open("/run/append",VFS_O_CREAT|VFS_O_RDWR|VFS_O_APPEND);assert(f);
 pthread_t x,y;assert(!pthread_create(&x,NULL,append_worker,f));assert(!pthread_create(&y,NULL,append_worker,f));
 assert(!pthread_join(x,NULL) && !pthread_join(y,NULL));assert(f->offset==2000);
 f->offset=0;assert(vfs_read(f,b,sizeof(b))==2000);for(unsigned i=0;i<2000;i++)assert(b[i]==(i&1 ? 'y' : 'x'));
 vfs_close(f);assert(!vfs_unlink("/run/append"));
 unsigned char tar[1536]={0};struct ustar_header *h=(void *)tar;
 memcpy(h->magic,"ustar",5);memcpy(h->name,"metadata",9);memcpy(h->mode,"0006754",7);
 memcpy(h->uid,"0001234",7);memcpy(h->gid,"0005670",7);memcpy(h->size,"00000000000",11);h->typeflag='0';
 assert(tarfs_init(tar,sizeof(tar))==0);check("/metadata",VFS_S_IFREG|06754,01234,05670);
 h->mode[0]='8';assert(tarfs_init(tar,sizeof(tar))==-7);
 n=vfs_lookup("/metadata");assert(n);g_vfs_root->children=n->next;kfree(n);kfree(g_vfs_root);
 assert(!live);
 puts("PASS Phase0 namespaces: full mode matrix/owners, bounded pool/reuse, detached file, zero fill, USB-only read-only nodes, TarFS metadata/rejection");
 return 0;
}
