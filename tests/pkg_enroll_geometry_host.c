/* Actual GPT + EXT4/JBD2 read-only structural preview. Not boot admission. */
#define main retained_ext4_mount_main
#define gpt_get_partition_count retained_gpt_count_stub
#define gpt_get_partition retained_gpt_get_stub
#include "ext4_mount_host.c"
#undef main
#undef gpt_get_partition_count
#undef gpt_get_partition
#include "../src/fs/gpt.c"
#include "../src/lib/crc32.c"
#include "../user/tools/digest.h"
void serial_print_dec(uint64_t n) {(void)n;}
void serial_print_hex(uint64_t n) {(void)n;}
bool block_register_dev(block_dev_t *d) {(void)d;return true;}
bool block_unregister_dev(block_dev_t *d) {(void)d;return true;}
static bool denied_write(block_dev_t *d,uint64_t l,const void *b) {
    (void)l;(void)b;((ext4_fault_disk_t *)d->priv)->writes++;return false;
}
static bool denied_flush(block_dev_t *d) {
    ((ext4_fault_disk_t *)d->priv)->flushes++;return false;
}
static int identity_path(ext4_mount_t *fs,const char *path,e4_inode_t *out) {
    uint32_t ino=2;int r=0;
    while (*path) {
        if (*path=='/') {path++;continue;}
        char name[64];unsigned n=0;
        while (*path && *path!='/') {if(n==sizeof(name)-1)return -VFS_EINVAL;name[n++]=*path++;}
        name[n]=0;e4_inode_t parent;r=e4_inode(fs,ino,&parent);if(r)return r;
        uint32_t found=0;r=e4_scan(&parent,name,0,NULL,&found);
        if(r<0)return r;
        if(!found)return -VFS_ENOENT;
        ino=found;
    }
    return e4_inode(fs,ino,out);
}
static int identities(ext4_mount_t *fs,const uint8_t *wire,size_t n) {
    static const char *paths[]={"/pkg/enrollment.v1","/pkg/grant.v1","/pkg/ownership.v1",
        "/pkg/transaction.v1","/pkg/origin.v1","/pkg/account-generation.v1","/pkg/state.v1",
        "/pkg/trust.v1","/pkg","/etc","/var","/var/lib","/var/lib/fortress",
        "/var/lib/fortress/accounts","/usr/bin","/usr/share/pkg","/pkg/descriptor.v2",
        "/etc/passwd","/etc/group","/etc/shadow"};
    if(n!=32+20*128 || memcmp(wire,"FORT-PKG-MAP-v1\0",16) || e4_u32(wire+16)!=n ||
        e4_u16(wire+20)!=1 || e4_u16(wire+22)!=20)return -VFS_EINVAL;
    for(unsigned i=24;i<32;i++)if(wire[i])return -VFS_EINVAL;
    for(unsigned i=0;i<20;i++) {
        const uint8_t *p=wire+32+128*i;e4_inode_t in;int r=identity_path(fs,paths[i],&in);
        if(r)return r;
        unsigned kind=in.mode==VFS_S_IFDIR?2:in.mode==VFS_S_IFREG?1:0;
        uint64_t size=(uint64_t)e4_u32(p+16)|((uint64_t)e4_u32(p+20)<<32);
        uint64_t expected=(uint64_t)e4_u32(p+24)|((uint64_t)e4_u32(p+28)<<32);
        if(e4_u16(p)!=i+1 || e4_u16(p+2)!=kind || !kind || in.ino!=e4_u32(p+4) ||
            !in.generation || in.generation!=e4_u32(p+8) || (in.full_mode&07777)!=e4_u32(p+12) ||
            in.uid || in.gid || in.size!=size || e4_u16(in.extent+6))return -VFS_EINVAL;
        for(unsigned k=34;k<40;k++)if(p[k])return -VFS_EINVAL;
        for(unsigned k=104;k<128;k++)if(p[k])return -VFS_EINVAL;
        r=e4_mapping(&in);if(r)return r;
        if(!in.map->count || in.map->count>4 || in.map->count!=e4_u16(p+32))return -VFS_EINVAL;
        uint64_t count=0;
        for(unsigned j=0;j<4;j++) {
            const uint8_t *x=p+40+16*j;
            if(j>=in.map->count) {for(unsigned k=0;k<16;k++)if(x[k])return -VFS_EINVAL;continue;}
            e4_extent_t actual=in.map->entries[j];
            if(actual.unwritten || actual.logical!=count || actual.logical!=e4_u32(x) ||
                actual.len!=e4_u32(x+4) || actual.physical!=e4_u32(x+8) || e4_u32(x+12))return -VFS_EINVAL;
            count+=actual.len;
        }
        if(count!=expected || count!=(in.size+fs->bs-1)/fs->bs)return -VFS_EINVAL;
    }
    return 0;
}
static int export_bundle(ext4_mount_t *fs,const char *path,const uint8_t *witness) {
    static const char *names[]={"/pkg/descriptor.v2","/pkg/enrollment.v1","/pkg/grant.v1",
        "/pkg/ownership.v1","/pkg/account-generation.v1","/pkg/origin.v1","/pkg/state.v1","/pkg/trust.v1"};
    static const unsigned sizes[]={2656,512,1024,8192,1024,1024,1024,1024};
    uint8_t *b=calloc(1,32768);assert(b);memcpy(b,"FORT-PKG-BND-v1\0",16);
    e4_p16(b+20,1);e4_p16(b+22,8);size_t at=96;int r=0;
    for(unsigned i=0;i<8 && !r;i++) {
        e4_inode_t in;r=identity_path(fs,names[i],&in);
        if(!r && (in.mode!=VFS_S_IFREG || in.size!=sizes[i]))r=-VFS_EINVAL;
        if(!r)r=e4_mapping(&in);
        size_t copied=0;
        for(unsigned j=0;!r && j<in.map->count;j++) {
            e4_extent_t x=in.map->entries[j];size_t take=(size_t)x.len*fs->bs;
            if(take>in.size-copied)take=(size_t)in.size-copied;
            if(x.unwritten || (uint64_t)x.logical*fs->bs!=copied ||
                !e4_bytes(fs,(uint64_t)x.physical*fs->bs,b+at+copied,take))r=-VFS_EIO;
            copied+=take;
        }
        if(!r && copied!=sizes[i])r=-VFS_EIO;
        if(!r) {e4_p32(b+32+8*i,sizes[i]);e4_p16(b+36+8*i,i+1);at+=copied;}
    }
    if(!r) {
        uint8_t geometry[1024];
        if(!witness || memcmp(b+96,"FORT-PKG-ENR-v2\0",16) || e4_u32(b+112)!=2656 ||
            e4_u16(b+116)!=2 || e4_u16(b+118)!=2 || e4_u32(b+128)!=2 ||
            memcmp(b+96+416,witness+32,16*128) ||
            !e4_bytes(fs,1024,geometry,sizeof(geometry)) || memcmp(geometry+104,b+96+40,16) ||
            e4_u32(geometry)!=e4_u32(b+96+352) || e4_u32(geometry+4)!=e4_u32(b+96+344) ||
            e4_u32(b+96+348) || e4_u32(b+96+36)!=fs->bs)r=-VFS_EINVAL;
    }
    if(!r) {
        const uint8_t *account=b+96+2656+512+1024+8192;
        static const char *credentials[]={"/etc/passwd","/etc/group","/etc/shadow"};
        uint8_t *scratch=malloc(8192);assert(scratch);
        for(unsigned i=0;i<3 && !r;i++) {
            e4_inode_t in;r=identity_path(fs,credentials[i],&in);
            if(!r && (in.mode!=VFS_S_IFREG || !in.size || in.size>8192 || in.size!=e4_u32(account+424+4*i)))r=-VFS_EINVAL;
            if(!r)r=e4_mapping(&in);
            size_t copied=0;
            for(unsigned j=0;!r && j<in.map->count;j++) {
                e4_extent_t x=in.map->entries[j];size_t take=(size_t)x.len*fs->bs;
                if(take>in.size-copied)take=(size_t)in.size-copied;
                if(x.unwritten || (uint64_t)x.logical*fs->bs!=copied ||
                    !e4_bytes(fs,(uint64_t)x.physical*fs->bs,scratch+copied,take))r=-VFS_EIO;
                copied+=take;
            }
            if(!r) {
                digest_t d;uint8_t hash[32];digest_init(&d,DIGEST_SHA256);
                if(copied!=in.size || !digest_update(&d,scratch,copied))r=-VFS_EIO;
                else {digest_final(&d,hash);if(memcmp(hash,account+328+32*i,32))r=-VFS_EINVAL;}
            }
        }
        free(scratch);
    }
    if(!r) {
        e4_p32(b+16,(uint32_t)at);FILE *f=fopen(path,"wbx");
        if(!f)r=-VFS_EIO;
        else {if(fwrite(b,1,at,f)!=at)r=-VFS_EIO;if(fclose(f))r=-VFS_EIO;}
    }
    free(b);return r;
}
int main(int argc,char **argv) {
    assert(argc>=3 && argc<=5);size_t n;uint8_t *initial=load(argv[1],&n);
    size_t witness_n=0;uint8_t *witness=argc>=4?load(argv[3],&witness_n):NULL;
    ext4_fault_disk_t d={.stable=initial,.volatile_bytes=initial,.bytes=n,.cut=-1};
    block_dev_t disk={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,
        .write_sector=denied_write,.flush=denied_flush,.priv=&d};
    gpt_policy_result_t policy;bool parsed=gpt_parse_ex(&disk,&policy);
    int r=-VFS_EINVAL;unsigned slots[2]={0};ext4_engine_t *e=NULL;
    if (parsed && policy==GPT_POLICY_PRIMARY_CONSISTENT && gpt_get_partition_count()==2) {
        gpt_partition_t *p=gpt_get_partition(0);gpt_guid_t wanted;
        assert(gpt_str_to_guid("12345678-1234-4567-89ab-123456789abc",&wanted));
        if (!strcmp(argv[2],"refuse-io")) fail_read=0;
        if (gpt_guid_equal(&p->unique_guid,&wanted) && gpt_guid_equal(&p->type_guid,&GPT_GUID_LINUX_FS) &&
            p->starting_lba==2048 && p->sector_count==65536)
            r=e4_open_journal_orphans_owned(&p->block_dev,true,&e,false,true);
        if (!r) {
            ext4_mount_t *fs=e->fs;e4_ns_workspace_t *w=kcalloc(1,sizeof(*w));assert(w);
            e4_inode_t root;r=e4_inode(fs,2,&root);
            if (!r && (root.mode!=VFS_S_IFDIR || root.uid || root.gid || (root.full_mode&07777)!=0755)) r=-VFS_EACCES;
            if (!r) r=e4_ns_child(fs,2,"etc",0755,&w->anchors[0]);
            if (!r) r=e4_ns_child(fs,2,"var",0755,&w->anchors[1]);
            if (!r) r=e4_ns_child(fs,w->anchors[1],"lib",0755,&w->anchors[2]);
            if (!r) r=e4_ns_child(fs,w->anchors[2],"fortress",0755,&w->anchors[3]);
            if (!r) r=e4_ns_child(fs,w->anchors[3],"accounts",0700,&w->anchors[4]);
            if (!r) r=e4_ns_directory(fs,w,0,2);
            if (!r) r=e4_ns_directory(fs,w,1,w->anchors[3]);
            if (!r && w->report.present_roles!=7) r=-VFS_EINVAL;
            if (!r && witness) r=identities(fs,witness,witness_n);
            if (!r && argc==5) r=export_bundle(fs,argv[4],witness);
            slots[0]=w->report.slots[0];slots[1]=w->report.slots[1];
            assert(!fs->account_namespace_fenced && !fs->journal_mounted && !e4_active);
            kfree(w);ext4_engine_close(e);
        }
    }
    assert(!d.writes && !d.flushes && !live && !e4_engine_busy);
    bool accept=!strcmp(argv[2],"accept");assert(accept ? !r : r!=0);
    printf("PASS structural-preview expected=%s result=%d slots=%u,%u writes=%zu flushes=%zu policy=%u\n",
        argv[2],r,slots[0],slots[1],d.writes,d.flushes,(unsigned)policy);
    free(witness);free(initial);return 0;
}
