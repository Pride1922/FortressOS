/* Actual driver with single-threaded memory/VFS/block adapters. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/fs/ext4.c"

static uint8_t *disk;
static size_t disk_len, live, reads, writes, flushes;
static long fail_alloc=-1, fail_read=-1;
static const char *stage;
static vfs_node_t root;
void *kmalloc(size_t len) {
    if (fail_alloc==0) return NULL;
    if (fail_alloc>0) fail_alloc--;
    void *p=malloc(len); if (p) live++; return p;
}
void *kcalloc(size_t n,size_t len) {
    if (n && len>SIZE_MAX/n) return NULL;
    void *p=kmalloc(n*len); if (p) memset(p,0,n*len); return p;
}
void kfree(void *p) { if (p) { assert(live); live--; free(p); } }
bool block_read_sector(block_dev_t *d,uint64_t lba,void *out) {
    assert(lba<d->sector_count && lba*d->sector_size<=disk_len-d->sector_size);
    reads++;
    if (fail_read==0) return false;
    if (fail_read>0) fail_read--;
    memcpy(out,disk+lba*d->sector_size,d->sector_size); return true;
}
static bool wr(block_dev_t *d,uint64_t lba,const void *p) {
    (void)d; (void)lba; (void)p; writes++; return false;
}
static bool fl(block_dev_t *d) { (void)d; flushes++; return false; }
bool block_write_sector(block_dev_t *d,uint64_t lba,const void *p) { return d->write_sector(d,lba,p); }
bool block_flush(block_dev_t *d) { return d->flush(d); }
void vfs_set_last_create_error(int err) { (void)err; }
vfs_node_t *vfs_lookup(const char *path) {
    if (!strcmp(path,"/")) return &root;
    if (!strcmp(path,"/mnt")) return root.children;
    return NULL;
}
static void reset(void) {
    if (e4_active) { e4_discard(e4_active); e4_active=NULL; }
    memset(&root,0,sizeof(root)); memcpy(root.path,"/",2);
    assert(!live && !writes && !flushes);
    fail_alloc=fail_read=-1; reads=0;
}
static uint32_t crc_reference(const uint8_t *p,size_t len) {
    /* Independent byte-wise polynomial division for mutated SB fixtures. */
    uint32_t r=~0u;
    for (size_t i=0;i<len;i++) {
        for (unsigned b=0;b<8;b++) {
            uint32_t bit=(r ^ (p[i]>>b)) & 1;
            r>>=1; if (bit) r^=0x82f63b78u;
        }
    }
    return r;
}
static void sb_checksum(void) { e4_p32(disk+1024+1020,crc_reference(disk+1024,1020)); }
static void inode_checksum(uint64_t offset,uint32_t number,uint32_t seed) {
    uint8_t *in=disk+offset, b[4];
    in[124]=in[125]=in[130]=in[131]=0;
    e4_p32(b,number); uint32_t crc=e4_crc(seed,b,4);
    crc=e4_crc(crc,in+100,4); crc=e4_crc(crc,in,256);
    in[124]=(uint8_t)crc; in[125]=(uint8_t)(crc>>8);
    in[130]=(uint8_t)(crc>>16); in[131]=(uint8_t)(crc>>24);
}
static void reject(block_dev_t *dev,int expected) {
    ext4_mount_t *m=(void *)1;
    int r=ext4_mount_ro(dev,"/mnt",&m);
    if (r!=expected) fprintf(stderr,"%s got %d expected %d reads=%zu fail=%ld\n",stage,r,expected,reads,fail_read);
    assert(r==expected && !m && !root.children && !e4_active && !live);
    assert(!writes && !flushes);
}
static void run_image(const char *path) {
    FILE *f=fopen(path,"rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); disk_len=(size_t)ftell(f); rewind(f);
    disk=malloc(disk_len); assert(disk && fread(disk,1,disk_len,f)==disk_len); fclose(f);
    uint8_t sb[1024]; memcpy(sb,disk+1024,1024);
    uint32_t bs=1024u<<e4_u32(sb+24);
    for (unsigned ss=512;ss<=4096;ss*=8) {
        reset();
        block_dev_t dev={.sector_size=ss,.sector_count=disk_len/ss,
                         .read_sector=block_read_sector,.write_sector=wr,.flush=fl};
        ext4_mount_t *m=NULL;
        int r=ext4_mount_ro(&dev,"/mnt",&m);
        if (r) fprintf(stderr,"mount %s ss=%u failed %d\n",path,ss,r);
        assert(!r && m && root.children);
        size_t mount_reads=reads;
        vfs_node_t *dir=root.children;
        assert(dir->type==VFS_DIRECTORY && !dir->write && !dir->create &&
               !dir->unlink && !dir->rename && !dir->truncate);
        assert(dir->can_write(dir)==-VFS_EROFS);
        size_t initial_live=live;
        fail_alloc=0;
        assert(!dir->lookup(dir,"data.bin") && live==initial_live && !dir->children);
        fail_alloc=-1;
        vfs_node_t *file=dir->lookup(dir,"data.bin");
        assert(file && file->size==1048576 && file->type==VFS_FILE);
        assert(dir->lookup(dir,"data.bin")==file && !dir->lookup(dir,"absent"));
        uint8_t bytes[65536];
        fail_alloc=0;
        assert(file->read(file,0,bytes,1)==-VFS_ENOMEM);
        fail_alloc=-1;
        for (uint64_t at=0;at<file->size;at+=sizeof(bytes)) {
            assert(file->read(file,at,bytes,sizeof(bytes))==(int64_t)sizeof(bytes));
            for (unsigned k=0;k<sizeof(bytes);k++) assert(bytes[k]==(uint8_t)(at+k));
        }
        assert(file->read(file,bs-5,bytes,19)==19);
        for (unsigned k=0;k<19;k++) assert(bytes[k]==(uint8_t)(bs-5+k));
        assert(file->read(file,0,bytes,65537)==65536);
        fail_read=(bs+ss-1)/ss;
        assert(file->read(file,0,bytes,bs*2)==bs); fail_read=-1;
        assert(!file->read(file,UINT64_MAX,bytes,1));
        assert(!file->read(file,0,NULL,0));
        assert(file->read(file,file->size-7,bytes,20)==7);
        fail_read=0; assert(file->read(file,0,bytes,1)==-VFS_EIO); fail_read=-1;
        vfs_node_t *fragment=dir->lookup(dir,"fragmented.bin");
        if (fragment) {
            assert(fragment->size==3200ULL*bs);
            for (uint64_t at=0;at<fragment->size;) {
                int64_t n=fragment->read(fragment,at,bytes,sizeof(bytes)); assert(n>0);
                for (int64_t k=0;k<n;k++) {
                    uint64_t block=(at+(uint64_t)k)/bs;
                    assert(bytes[k]==(block%2 ? 0 : (uint8_t)((block/2)%251+1)));
                }
                at+=(uint64_t)n;
            }
            assert(e4_u16(((e4_inode_t *)fragment->fs_private)->extent+6)==2);
            vfs_node_t *sparse=dir->lookup(dir,"sparse.bin"); assert(sparse);
            uint64_t pos=(1ULL<<32)+bs+17;
            assert(sparse->read(sparse,pos-17,bytes,100)==32);
            for (unsigned k=0;k<17;k++) assert(!bytes[k]);
            assert(!memcmp(bytes+17,"EXT4-above-4GiB",15));
            assert(sparse->read(sparse,1ULL<<32,bytes,bs)==bs);
            for (unsigned k=0;k<bs;k++) assert(!bytes[k]);
            vfs_node_t *unwritten=dir->lookup(dir,"unwritten.bin"); assert(unwritten);
            assert(unwritten->read(unwritten,0,bytes,8*bs)==8*bs);
            for (unsigned k=0;k<8*bs;k++) assert(!bytes[k]);
            fail_read=0;
            assert(unwritten->read(unwritten,0,bytes,8*bs)==8*bs);
            assert(sparse->read(sparse,1ULL<<32,bytes,bs)==bs);
            fail_read=-1; /* Cached zero ranges never read stale data sectors. */
            puts("PASS depth-2 fragmented bytes, sparse offsets above 4GiB, unwritten zeroes");
        }
        /* Checksummed inode fields alone cannot authorize hostile extent maps. */
        e4_inode_t hostile=*(e4_inode_t *)file->fs_private; hostile.map=NULL;
        uint8_t original_extent[60]; memcpy(original_extent,hostile.extent,60);
        e4_p32(hostile.extent+20,m->blocks); assert(e4_mapping(&hostile)==-VFS_EIO);
        memcpy(hostile.extent,original_extent,60);
        e4_p32(hostile.extent+20,e4_u32(m->gd[0].raw)); assert(e4_mapping(&hostile)==-VFS_EIO);
        memcpy(hostile.extent,original_extent,60);
        hostile.extent[16]=hostile.extent[17]=0; assert(e4_mapping(&hostile)==-VFS_EIO);
        memcpy(hostile.extent,original_extent,60);
        hostile.extent[6]=3; assert(e4_mapping(&hostile)==-VFS_EOPNOTSUPP);
        memcpy(hostile.extent,original_extent,60);
        hostile.extent[2]=2; memcpy(hostile.extent+24,hostile.extent+12,12);
        e4_p32(hostile.extent+24,e4_u16(hostile.extent+16));
        assert(e4_mapping(&hostile)==-VFS_EIO); /* Physical alias. */
        memcpy(hostile.extent,original_extent,60);
        hostile.extent[2]=2; memcpy(hostile.extent+24,hostile.extent+12,12);
        assert(e4_mapping(&hostile)==-VFS_EIO); /* Logical overlap. */
        if (fragment) {
            hostile=*(e4_inode_t *)fragment->fs_private; hostile.map=NULL;
            uint32_t child=e4_u32(hostile.extent+16);
            uint64_t offset=(uint64_t)child*bs;
            disk[offset+12+((bs-12)/12)*12+3]^=1;
            assert(e4_mapping(&hostile)==-VFS_EIO); disk[offset+12+((bs-12)/12)*12+3]^=1;
            uint8_t original_index[4096]; memcpy(original_index,disk+offset,bs);
            unsigned tail=12+((bs-12)/12)*12;
            uint32_t tree_seed=e4_inode_seed(m,hostile.ino,hostile.generation);
            /* A valid-checksum child cycle/alias fails before another traversal. */
            e4_p32(disk+offset+16,child);
            e4_p32(disk+offset+tail,e4_crc(tree_seed,disk+offset,tail));
            assert(e4_mapping(&hostile)==-VFS_EIO);
            memcpy(disk+offset,original_index,bs);
            uint32_t leaf=e4_u32(disk+offset+16);
            uint64_t leaf_offset=(uint64_t)leaf*bs;
            uint8_t original_leaf[4096]; memcpy(original_leaf,disk+leaf_offset,bs);
            disk[leaf_offset+16]=disk[leaf_offset+17]=0;
            e4_p32(disk+leaf_offset+tail,e4_crc(tree_seed,disk+leaf_offset,tail));
            assert(e4_mapping(&hostile)==-VFS_EIO);
            memcpy(disk+leaf_offset,original_leaf,bs);
            e4_p32(disk+leaf_offset+24,e4_u32(disk+leaf_offset+12));
            e4_p32(disk+leaf_offset+tail,e4_crc(tree_seed,disk+leaf_offset,tail));
            assert(e4_mapping(&hostile)==-VFS_EIO);
            memcpy(disk+leaf_offset,original_leaf,bs);
            size_t before=live; fail_alloc=0;
            assert(e4_mapping(&hostile)==-VFS_ENOMEM && !hostile.map && live==before);
            fail_alloc=-1; fail_read=0;
            assert(e4_mapping(&hostile)==-VFS_EIO && !hostile.map && live==before);
            fail_read=-1;
        }
        bool found=false;
        for (unsigned cookie=0;cookie<16;cookie++) {
            vfs_dirent_t de; int status=dir->readdir(dir,cookie,&de);
            assert(status>=0); if (!status) break;
            if (!strcmp(de.name,"data.bin")) { found=true; assert(de.size==1048576); }
        }
        assert(found && !ext4_sync(m) && !ext4_freeze_and_sync(m));
        ext4_mount_t *other=(void *)1;
        assert(ext4_mount_rw(&dev,"/mnt",&other)==-VFS_EEXIST && !other);
        assert(ext4_mount_ro(&dev,"/mnt",&other)==-VFS_EEXIST && !other);
        reset();
        for (long i=0;i<5;i++) { fail_alloc=i; reject(&dev,-VFS_ENOMEM); reset(); }
        stage="read injection";
        for (size_t i=0;i<mount_reads;i++) { fail_read=(long)i; reject(&dev,-VFS_EIO); reset(); }
        const unsigned offsets[]={92,96,100};
        stage="feature masks";
        for (unsigned i=0;i<3;i++) {
            e4_p32(disk+1024+offsets[i],e4_u32(sb+offsets[i])|0x80000000u);
            sb_checksum(); reject(&dev,-VFS_EOPNOTSUPP); memcpy(disk+1024,sb,1024);
        }
        stage="recovery"; e4_p32(disk+1024+96,0x46); sb_checksum(); reject(&dev,-VFS_EOPNOTSUPP); memcpy(disk+1024,sb,1024);
        disk[1024+58]=0; sb_checksum(); reject(&dev,-VFS_EIO); memcpy(disk+1024,sb,1024);
        disk[1024+120]^=1; reject(&dev,-VFS_EIO); memcpy(disk+1024,sb,1024);
        e4_p32(disk+1024+32,0); sb_checksum(); reject(&dev,-VFS_EIO); memcpy(disk+1024,sb,1024);
        uint64_t gdt=(uint64_t)(bs==1024 ? 2 : 1)*bs;
        uint8_t desc[32]; memcpy(desc,disk+gdt,32);
        disk[gdt+12]^=1; reject(&dev,-VFS_EIO); memcpy(disk+gdt,desc,32);
        /* Valid-checksum overlapping bitmap reservation must still reject. */
        memcpy(disk+gdt+4,disk+gdt,4); disk[gdt+30]=disk[gdt+31]=0;
        uint8_t group[4]={0}; uint32_t seed=e4_crc(UINT32_MAX,sb+104,16);
        uint16_t sum=(uint16_t)e4_crc(e4_crc(seed,group,4),disk+gdt,32);
        disk[gdt+30]=(uint8_t)sum; disk[gdt+31]=(uint8_t)(sum>>8);
        reject(&dev,-VFS_EIO); memcpy(disk+gdt,desc,32);
        uint64_t bitmap=(uint64_t)e4_u32(desc)*bs;
        stage="block bitmap"; disk[bitmap]^=1; reject(&dev,-VFS_EIO); disk[bitmap]^=1;
        bitmap=(uint64_t)e4_u32(desc+4)*bs;
        stage="inode bitmap"; disk[bitmap]^=1; reject(&dev,-VFS_EIO); disk[bitmap]^=1;
        uint64_t inode=(uint64_t)e4_u32(desc+8)*bs+256;
        stage="root inode"; disk[inode+4]^=1; reject(&dev,-VFS_EIO); disk[inode+4]^=1;
        uint64_t data=(uint64_t)e4_u32(disk+inode+60)*bs;
        stage="directory"; disk[data+8]^=1; reject(&dev,-VFS_EIO); disk[data+8]^=1;
        /* Parser checks must also reject correctly checksummed bad records. */
        uint8_t original_dir[4096]; memcpy(original_dir,disk+data,bs);
        uint8_t ino_bytes[256]; memcpy(ino_bytes,disk+inode,256);
        uint32_t dseed=e4_inode_seed(&(ext4_mount_t){.seed=seed},2,e4_u32(ino_bytes+100));
        disk[data+4]=disk[data+5]=0;
        e4_p32(disk+data+bs-4,e4_crc(dseed,disk+data,bs-12));
        stage="checksummed zero record"; reject(&dev,-VFS_EIO);
        memcpy(disk+data,original_dir,bs);
        disk[data+bs-5]=0;
        stage="bad directory tail"; reject(&dev,-VFS_EIO);
        memcpy(disk+data,original_dir,bs);
        disk[inode+46]=1; inode_checksum(inode,2,seed);
        stage="malformed external directory tree"; reject(&dev,-VFS_EIO);
        memcpy(disk+inode,ino_bytes,256);
        e4_p32(disk+inode+32,0x81000); inode_checksum(inode,2,seed);
        stage="indexed directory deferred"; reject(&dev,-VFS_EOPNOTSUPP);
        memcpy(disk+inode,ino_bytes,256);
        stage="truncated device";
        dev.sector_count=1; reject(&dev,-VFS_EIO);
        printf("PASS ext4 bs=%u sector=%u: admission/lookup/readdir, checksums, masks, overlap, OOM, every read failure, zero writes/flushes\n",bs,ss);
    }
    free(disk); disk=NULL;
}
int main(int argc,char **argv) {
    assert(~e4_crc(UINT32_MAX,"123456789",9)==0xe3069283u);
    assert(argc==4);
    reset();
    for (int i=1;i<argc;i++) run_image(argv[i]);
    puts("EXT4 Phases 1-2 host ASan/UBSan PASS (single-threaded adapters; no kernel/physical claim)");
    return 0;
}
