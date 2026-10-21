/* Actual mounted EXT4/JBD2 + account redo. Host memory, locks and device I/O;
 * byte-limit profile uses synthetic validation, not account-service admission. */
#define TEST_PERMISSIONS_ENFORCEMENT
#define main unused_mount_main
#include "ext4_mount_host.c"
#undef main
#include "../src/kernel/creds.c"
#include "../src/fs/permission_values.c"
#include "../src/include/account_redo.h"
#include "../user/tools/userdb.h"
static FILE *image;
static ext4_mount_t *mounted;
static account_io_t backend;
static account_workspace_t workspace;
static uint64_t min_blocks,min_inodes,baseline_blocks,baseline_inodes;
static unsigned max_orphans,callbacks;
static unsigned writes_seen,flushes_seen,cut_flush;
static unsigned update_flushes;
static bool offline;
static bool byte_profile;
static unsigned char fill_bytes[32768];
static const char *small[3]={"root:x:0:0:root:/root:/bin/shell\n","root:x:0:\n","root:!:::::::\n"};
static bool rd(block_dev_t *dev,uint64_t l,void *p) {
    return !fseeko(image,l*dev->sector_size,SEEK_SET) && fread(p,1,dev->sector_size,image)==dev->sector_size;
}
static bool wr(block_dev_t *dev,uint64_t l,const void *p) {
    if(offline)return false;
    writes_seen++;
    return !fseeko(image,l*dev->sector_size,SEEK_SET) && fwrite(p,1,dev->sector_size,image)==dev->sector_size;
}
static bool flush(block_dev_t *dev) {
    (void)dev;if(offline)return false;
    if(cut_flush && ++flushes_seen==cut_flush){offline=true;return false;}
    if(!cut_flush)flushes_seen++;
    return !fflush(image);
}
static void sample(void) {
    if(offline)return;
    uint8_t sb[1024];assert(e4_bytes(mounted,1024,sb,sizeof(sb)));
    uint64_t b=e4_u32(sb+12),n=e4_u32(sb+16);
    if(b<min_blocks)min_blocks=b;
    if(n<min_inodes)min_inodes=n;
    if(mounted->engine->orphan_count>max_orphans)max_orphans=mounted->engine->orphan_count;
}
static int read_role(void *p,unsigned role,void *out,size_t cap,size_t *n) {
    (void)p;return backend.read(backend.context,role,out,cap,n);
}
static int write_role(void *p,unsigned role,const void *in,size_t n) {
    (void)p;int r=backend.write(backend.context,role,in,n);callbacks++;sample();return r;
}
static int remove_role(void *p,unsigned role) {
    (void)p;int r=backend.remove(backend.context,role);callbacks++;sample();return r;
}
static int rename_role(void *p,unsigned a,unsigned b) {
    (void)p;int r=backend.rename(backend.context,a,b);callbacks++;sample();return r;
}
static int sync_roles(void *p) {
    (void)p;int r=backend.sync(backend.context);callbacks++;sample();return r;
}
static int valid(void *p,const char *const values[3],const size_t lengths[3]) {
    (void)p;
    if(byte_profile && lengths[0]==8192 && lengths[1]==8192 && lengths[2]==8192) return 0;
    userdb_t db={0};
    bool ok=db_passwd(&db,values[0],lengths[0]) && db_group(&db,values[1],lengths[1]) &&
        db_shadow(&db,values[2],lengths[2]) && db_validate(&db);
    db_wipe(&db,sizeof(db));return ok ? 0 : -VFS_EINVAL;
}
int main(int argc,char **argv) {
    assert(argc==4);unsigned sector=(unsigned)strtoul(argv[2],NULL,10);
    byte_profile=strcmp(argv[3],"small")!=0;bool pins=!strcmp(argv[3],"pins");
    image=fopen(argv[1],"r+b");assert(image && !fseeko(image,0,SEEK_END));
    size_t bytes=ftello(image);assert(bytes && bytes%sector==0);
    block_dev_t dev={.sector_size=sector,.sector_count=bytes/sector,.read_sector=rd,.write_sector=wr,.flush=flush};
    vfs_init();assert(!ext4_prepare_root(&dev,true,&mounted));
    uint8_t sb[1024];assert(e4_bytes(mounted,1024,sb,sizeof(sb)));
    baseline_blocks=min_blocks=e4_u32(sb+12);baseline_inodes=min_inodes=e4_u32(sb+16);
    assert(!ext4_root_account_io(mounted,&backend));sample();
    account_io_t io={NULL,read_role,write_role,remove_role,rename_role,sync_roles,valid};
    assert(!account_redo_recover(&io,&workspace));
    creds_t actor;creds_init_root(&actor);vfs_node_t *nodes[4];int error;
    const char *paths[4]={"/etc","/etc/passwd","/etc/group","/etc/shadow"};
    for(unsigned i=0;i<4;i++){nodes[i]=vfs_lookup_at_creds(ext4_prepared_root(mounted),paths[i],&actor,&error);assert(nodes[i]);}
    assert(!ext4_root_lock_accounts(mounted,nodes));for(unsigned i=0;i<4;i++)vfs_node_put(nodes[i]);
    assert(!ext4_publish_root(mounted));
    if(!strcmp(argv[3],"directory")) {
        vfs_node_t *dir=vfs_lookup_ref("/etc",&error);assert(dir);uint64_t before=dir->size;
        unsigned created=0;
        while(dir->size==before) {
            char path[120];snprintf(path,sizeof(path),"/etc/growth-%04u-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",created++);
            file_t *f=vfs_open_creds(path,VFS_O_CREAT|VFS_O_RDONLY,&actor,&error);assert(f);assert(!vfs_close(f));
            assert(created<256);
        }
        assert(dir->size==before+mounted->bs);vfs_node_put(dir);
        printf("DIRECTORY PASS forced_growth_blocks=1 new_names=%u\n",created);
    }
    if(!strcmp(argv[3],"resources")) {
        unsigned before=writes_seen;fail_alloc=0;
        assert(backend.write(backend.context,ACCOUNT_STAGE_PASSWD,"x",1)==-VFS_ENOMEM);
        fail_alloc=-1;assert(writes_seen==before);
        uint32_t length=mounted->engine->writer->io->length;
        mounted->engine->writer->io->length=mounted->engine->writer->io->first+1;
        assert(backend.write(backend.context,ACCOUNT_STAGE_PASSWD,"x",1)==-VFS_ENOSPC);
        mounted->engine->writer->io->length=length;assert(writes_seen==before);
        file_t *held[64];
        for(unsigned i=0;i<64;i++) {
            char path[64];snprintf(path,sizeof(path),"/orphan-%u",i);
            held[i]=vfs_open_creds(path,VFS_O_CREAT|VFS_O_RDONLY,&actor,&error);assert(held[i]);
            assert(!vfs_unlink_creds(path,&actor));
        }
        assert(mounted->engine->orphan_count==64);before=writes_seen;
        assert(backend.remove(backend.context,ACCOUNT_PASSWD)==-VFS_EFBIG && writes_seen==before);
        for(unsigned i=0;i<64;i++)assert(!vfs_close(held[i]));
        assert(!ext4_sync(mounted) && !mounted->engine->orphan_count);
        puts("RESOURCE PASS heap ENOMEM, journal ENOSPC, orphan EFBIG; zero writes on each denial");
    }
    if(!strcmp(argv[3],"cache")) {
        for(unsigned i=0;mounted->nodes<E4_NODES;i++) {
            char path[64];snprintf(path,sizeof(path),"/cache-%u",i);assert(vfs_create(path,VFS_FILE));
            assert(i<2048);
        }
        for(unsigned i=0;i<mounted->nodes;i++)mounted->cached[i]->legacy=true;
        unsigned before=writes_seen;
        assert(backend.write(backend.context,ACCOUNT_STAGE_PASSWD,"x",1)==-VFS_EFBIG && writes_seen==before);
        for(unsigned i=1;i<mounted->nodes;i++)mounted->cached[i]->legacy=false;
        puts("RESOURCE PASS 1024 pinned cache nodes; zero writes on EFBIG; private legacy pins released");
    }
    if(!strcmp(argv[3],"inode")) {
        for(unsigned i=0;;i++) {
            char path[64];snprintf(path,sizeof(path),"/inode-%u",i);
            file_t *f=vfs_open_creds(path,VFS_O_CREAT|VFS_O_RDONLY,&actor,&error);
            if(!f){assert(error==-VFS_ENOSPC);break;}assert(!vfs_close(f));assert(i<4096);
        }
        assert(e4_bytes(mounted,1024,sb,sizeof(sb)));
        assert(e4_u32(sb+16)==EXT4_ACCOUNT_RESERVE_INODES && e4_u32(sb+12)>1024);
        baseline_blocks=min_blocks=e4_u32(sb+12);baseline_inodes=min_inodes=e4_u32(sb+16);
    }
    if(!strcmp(argv[3],"fragment")) {
        file_t *fragment=vfs_open_creds("/fragment",VFS_O_CREAT|VFS_O_RDWR,&actor,&error);assert(fragment);
        for(unsigned i=0;i<8;i++) {
            assert(vfs_write(fragment,fill_bytes,1024)==1024);
            char path[64];snprintf(path,sizeof(path),"/separator-%u",i);
            file_t *sep=vfs_open_creds(path,VFS_O_CREAT|VFS_O_WRONLY,&actor,&error);assert(sep);
            assert(vfs_write(sep,fill_bytes,mounted->bs)==(int64_t)mounted->bs);assert(!vfs_close(sep));
        }
        e4_inode_t in;assert(!e4_inode(mounted,((e4_node_t *)fragment->node)->inode.ino,&in));
        assert(mounted->bs==1024 && e4_u16(in.extent+6)==1);
        uint64_t off=0;ext4_engine_t *e=mounted->engine;
        uint64_t lock=spin_lock_irqsave(&e4_lock);e->file_planning=true;
        assert(!e4_file_plan(e,in.ino,&off,false,"Z",1));e->file_planning=false;
        assert(e->old_count>=1 && e->frees>=1);
        unsigned fresh=0;for(unsigned i=0;i<e->images;i++)fresh+=e->image[i].role==E4_NEW;
        assert(fresh>=2);ext4_engine_abort(e);e4_semantic_invalidate(e);
        spin_unlock_irqrestore(&e4_lock,lock);assert(!vfs_close(fragment));
        puts("FRAGMENT PASS depth=1; existing-data overwrite plans new tree storage before old frees; plan aborted");
    }
    if(!strcmp(argv[3],"floor")) {
        file_t *fill=vfs_open_creds("/fill",VFS_O_CREAT|VFS_O_WRONLY,&actor,&error);assert(fill);
        /* Real allocation exhaustion, first in batches and then one block. */
        for(size_t chunk=sizeof(fill_bytes);;chunk=mounted->bs) {
            unsigned attempts=0;int64_t r;
            do {r=vfs_write(fill,fill_bytes,chunk);assert(++attempts<32768);} while(r>0);
            assert(r==-VFS_ENOSPC);
            if(chunk==mounted->bs)break;
        }
        vfs_close(fill);assert(!ext4_sync(mounted));
        assert(e4_bytes(mounted,1024,sb,sizeof(sb)));
        baseline_blocks=min_blocks=e4_u32(sb+12);baseline_inodes=min_inodes=e4_u32(sb+16);
        assert(baseline_blocks==(EXT4_ACCOUNT_RESERVE_BYTES+mounted->bs-1)/mounted->bs);
    }
    bool recover_only=!strcmp(argv[3],"recover");
    if(!strncmp(argv[3],"cut=",4))cut_flush=(unsigned)strtoul(argv[3]+4,NULL,10);
    flushes_seen=0;
    file_t *retained[9]={0};unsigned retained_count=0;
    for(unsigned generation=1;generation<=(pins?3u:1u);generation++) {
        if(pins)for(unsigned i=0;i<3;i++) {
            retained[retained_count]=vfs_open_creds(paths[i+1],VFS_O_RDONLY,&actor,&error);
            assert(retained[retained_count++]);
        }
        for(unsigned i=0;i<3;i++) {
            workspace.lengths[i]=byte_profile?8192:strlen(small[i]);
            if(byte_profile)memset(workspace.files[i],'A'+i,workspace.lengths[i]);
            else memcpy(workspace.files[i],small[i],workspace.lengths[i]);
        }
        if(recover_only)break;
        int updated=account_redo_update(&io,&workspace,generation);
        update_flushes=flushes_seen;
        if(cut_flush){assert(updated<0 && offline);printf("CUT PASS flush=%u writes=%u\n",cut_flush,writes_seen);assert(!fclose(image));return 0;}
        assert(!updated);
        assert(!account_redo_recover(&io,&workspace));
    }
    unsigned pinned_orphans=mounted->engine->orphan_count;
    if(!strcmp(argv[3],"floor"))assert(min_blocks<baseline_blocks);
    if(pins)assert(pinned_orphans==9);
    for(unsigned i=0;i<retained_count;i++) {
        /* Initial retained generation is coherent small data; later ones are 8 KiB. */
        size_t expected=i<3?strlen(small[i%3]):8192;unsigned char old_bytes[8192];
        assert(vfs_read(retained[i],old_bytes,sizeof(old_bytes))==(int64_t)expected);
        for(size_t j=0;j<expected;j++)assert(old_bytes[j]==(i<3?small[i%3][j]:'A'+(int)(i%3)));
        vfs_close(retained[i]);
    }
    assert(!ext4_sync(mounted));sample();assert(!mounted->engine->orphan_count);
    for(unsigned i=0;i<3;i++) {
        size_t n;assert(!backend.read(backend.context,i,workspace.files[i],8192,&n));
        bool is_small=recover_only?n==strlen(small[i]):!byte_profile;
        assert(n==(is_small?strlen(small[i]):8192));
        for(size_t j=0;j<n;j++)assert(workspace.files[i][j]==(is_small?small[i][j]:'A'+(int)i));
    }
    printf("PROFILE block=%u sector=%u kind=%s peak_committed_block_delta=%llu peak_committed_inode_delta=%llu max_orphans=%u retained_before_close=%u callbacks=%u\n",
           mounted->bs,sector,argv[3],(unsigned long long)(baseline_blocks-min_blocks),
           (unsigned long long)(baseline_inodes-min_inodes),max_orphans,pinned_orphans,callbacks);
    printf("UPDATE_FLUSHES=%u\n",update_flushes);
    assert(!ext4_freeze_and_sync(mounted));assert(!fclose(image));
    puts("PASS actual EXT4 account redo; no package grant or boot acceptance");return 0;
}
