/* Trusted factory-image producer only. Never compiled into the OS. No signing
 * key or catalog bypass is added to the runtime package service. The image is
 * initial authority; verified FPK hashes become ordinary installed records. */
#define PKG_ORDINARY_INCLUDED
#include "pkg_ordinary_install_host.c"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "install_payload.h"
static const char *packages[]={"fortress-calc","fortress-desktop","fortress-files","fortress-notepad","fortress-terminal"};
static const char *commands[]={"calc","desktop","files","notepad","terminal"};
enum { PACKAGE_COUNT = sizeof packages / sizeof packages[0], SEED_ARGC = 3 + PACKAGE_COUNT, UPGRADE_ARGC = SEED_ARGC + 2 + PACKAGE_COUNT };
_Static_assert(sizeof commands / sizeof commands[0] == PACKAGE_COUNT, "factory command count");
static void write_file(ext4_mount_t *fs,const char *path,const uint8_t *bytes,size_t n,unsigned mode,bool existing){
    creds_t root;creds_init_root(&root);int error;char parent[192],leaf[64];size_t at=strlen(path);
    while(at&&path[at-1]!='/')at--;
    assert(at&&strlen(path+at)<sizeof leaf);
    memcpy(parent,path,at);parent[at]=0;memcpy(leaf,path+at,strlen(path+at)+1);
    vfs_node_t *dir=vfs_lookup_at_creds(&fs->cached[0]->node,parent,&root,&error);assert(dir);
    vfs_node_t *file=vfs_lookup_at_creds(&fs->cached[0]->node,path,&root,&error);
    if(existing)assert(file);else{assert(!file&&error==-VFS_ENOENT);vfs_create_attrs_t attrs={.mode=mode,.uid=0,.gid=0};file=dir->create_attrs_ref(dir,leaf,VFS_FILE,&attrs,&error);}
    assert(file);vfs_metadata_t meta;assert(!vfs_metadata(file,&meta)&&!meta.uid&&!meta.gid&&(meta.mode&07777)==mode);
    if(existing)assert(meta.size==n);
    uint64_t off=0;while(off<n){size_t take=n-off;if(take>16384)take=16384;int64_t got=file->write(file,&off,false,bytes+off,take);assert(got>0&&(uint64_t)got<=take);}
    vfs_node_put(file);vfs_node_put(dir);
}
static void seed(ext4_mount_t *fs,unsigned slot,uint8_t *artifact,size_t an){
    px_context *c=px_allocate(sizeof *c);pu_context *u=px_allocate(sizeof *u);assert(c&&u);
    c->fs=fs;c->artifact=px_allocate(an);assert(c->artifact);memcpy(c->artifact,artifact,an);c->artifact_n=an;c->policy.slot=slot;
    uint8_t hash[32];assert(!crypto_sha256(artifact,an,hash));px_input input={artifact,an,0};
    assert(!fpk_verify(&c->verify.verify,px_source_read,&input,an,hash));
    const fpk_manifest *m=&c->verify.verify.manifest;
    assert(m->package.length==strlen(packages[slot])&&!memcmp(m->package.bytes,packages[slot],m->package.length));
    assert(m->command.length==strlen(commands[slot])&&!memcmp(m->command.bytes,commands[slot],m->command.length));
    uint8_t trust_bytes[1024];size_t trust_n=0;pkg_repository_trust trust;
    assert(!px_read_vfs_file(&fs->cached[0]->node,"/etc/pkg/trust.conf",trust_bytes,sizeof trust_bytes,&trust_n));
    assert(!pkg_repository_trust_decode(trust_bytes,trust_n,1792929600,&trust));
    assert(!memcmp(m->platform_id,trust.platform,32)&&m->base_min<=trust.base_version&&m->base_max>=trust.base_version);
    assert(!px_base_command(fs,m->command));assert(!px_ensure_database_slot(fs,slot));assert(!pu_shape(c));
    c->policy.resources.capacity[0]=fs->blocks;c->policy.resources.capacity[1]=fs->inodes;
    assert(e4_bytes(fs,1128,c->policy.filesystem,16));
    u->next=c;
    for(unsigned i=0;i<c->directory_count;i++){
        assert(!pu_directory(fs,c->directories[i]));e4_inode_t in;assert(!px_lookup(fs,c->directories[i],&in));
        u->record.directories[i].inode=in.ino;u->record.directories[i].generation=in.generation;
    }
    for(unsigned i=0;i<c->shape.file_count;i++)assert(!pu_stage(u,i));
    c->state=u->record;c->state.sequence=2;c->state.generation=1;c->state.floor=m->build;c->state.installed=1;
    c->state.made=c->state.published=(1u<<m->file_count)-1;
    memcpy(c->state.floor_hash,hash,32);memcpy(c->state.package,c->shape.package,32);memcpy(c->state.command,c->shape.command,32);memcpy(c->state.platform,trust.platform,32);
    memcpy(c->shape.artifact,hash,32);
    /* Factory authority is the installer image. No repository revision/key is
     * fabricated; first online mutation must verify the real repository policy. */
    assert(!c->state.root&&!c->state.catalog);
    px_encode_for(c,&c->state,c->encoded);char path[192];px_record_path(slot,path);write_file(fs,path,c->encoded,8192,0600,true);
    assert(!pu_check(c));
    assert(!px_ensure_dir(fs,"/var/lib/pkg/sources",0700));
    const char digits[]="0123456789abcdef";memcpy(path,"/var/lib/pkg/sources/",21);size_t at=21;
    for(unsigned i=0;i<32;i++){
        if(i==4){path[at]=0;assert(!px_ensure_dir(fs,path,0700));path[at++]='/';}
        path[at++]=digits[hash[i]>>4];path[at++]=digits[hash[i]&15];
    }memcpy(path+at,".fpk",5);
    write_file(fs,path,artifact,an,0600,false);
    pu_drop(c);kfree(u);
}
/* Test-only whole-disk adapter for the actual exclusive installer expansion.
 * Writes persist immediately; no power-loss/driver claim is made here. */
static int grow_read(void *ctx,uint64_t lba,uint32_t n,void *p){
    ext4_fault_disk_t *d=ctx;assert(lba>=INSTALL_ROOT_LBA);uint64_t at=(lba-INSTALL_ROOT_LBA)*512;
    assert(at<=d->bytes&&(uint64_t)n*512<=d->bytes-at);memcpy(p,d->stable+at,n*512);return 0;
}
static int grow_write(void *ctx,uint64_t lba,uint32_t n,const void *p){
    ext4_fault_disk_t *d=ctx;assert(lba>=INSTALL_ROOT_LBA);uint64_t at=(lba-INSTALL_ROOT_LBA)*512;
    assert(at<=d->bytes&&(uint64_t)n*512<=d->bytes-at);memcpy(d->stable+at,p,n*512);memcpy(d->volatile_bytes+at,p,n*512);return 0;
}
static int grow_flush(void *ctx){(void)ctx;return 0;}
static void output(const char *path,ext4_fault_disk_t *d){
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);assert(fd>=0);FILE *f=fdopen(fd,"wb");
    assert(f&&fwrite(d->stable,1,d->bytes,f)==d->bytes&&!fflush(f)&&!fsync(fileno(f))&&!fclose(f));
}
int main(int argc,char **argv){
    assert(argc==SEED_ARGC||argc==UPGRADE_ARGC);struct stat st;
    assert(!lstat(argv[1],&st)&&S_ISREG(st.st_mode)&&st.st_size==256*1024*1024);
    for(unsigned i=0;i<PACKAGE_COUNT;i++)assert(!lstat(argv[3+i],&st)&&S_ISREG(st.st_mode)&&st.st_size>128&&st.st_size<=FPK_HEADER_SIZE+FPK_MANIFEST_MAX+FPK_COMPRESSED_MAX);
    size_t n;uint8_t *initial=load(argv[1],&n);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable&&d.volatile_bytes);
    memcpy(d.stable,initial,n);memcpy(d.volatile_bytes,initial,n);
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    vfs_init();ext4_mount_t *fs=NULL;assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    px_inventory *existing=px_allocate(sizeof *existing);assert(existing&&!px_inventory_read(fs,existing));
    for(unsigned i=0;i<PX_PACKAGES;i++)assert(!existing->present[i]);
    kfree(existing);
    uint8_t *artifacts[PACKAGE_COUNT];size_t sizes[PACKAGE_COUNT];
    for(unsigned i=0;i<PACKAGE_COUNT;i++){artifacts[i]=load(argv[3+i],sizes+i);seed(fs,i,artifacts[i],sizes[i]);}
    pkg_transaction_status status;assert(!ext4_pkg_status(fs,&status)&&status.phase==PKG_TX_IDLE);
    assert(!ext4_freeze_and_sync(fs));output(argv[2],&d);teardown();
    if(argc==UPGRADE_ARGC){
        size_t bytes=320u*1024*1024;
        d.stable=realloc(d.stable,bytes);d.volatile_bytes=realloc(d.volatile_bytes,bytes);assert(d.stable&&d.volatile_bytes);
        memset(d.stable+d.bytes,0,bytes-d.bytes);memset(d.volatile_bytes+d.bytes,0,bytes-d.bytes);d.bytes=bytes;
        install_target_t target={.context=&d,.sector_size=512,.sectors=bytes/512+INSTALL_ROOT_LBA+33,.read=grow_read,.write=grow_write,.flush=grow_flush};
        install_expand_work_t *growth=calloc(1,sizeof *growth);assert(growth&&!install_root_expand(&target,bytes/512,growth));free(growth);dev.sector_count=bytes/512;
        vfs_init();fs=NULL;assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
        size_t tn,cn;uint8_t *trust=load(argv[SEED_ARGC],&tn),*catalog=load(argv[SEED_ARGC+1],&cn);
        /* Test copy only: factory output above retains the real trust config. */
        int error;creds_t root;creds_init_root(&root);
        vfs_node_t *tf=vfs_lookup_at_creds(&fs->cached[0]->node,"/etc/pkg/trust.conf",&root,&error);assert(tf&&tf->truncate&&!tf->truncate(tf,0));
        uint64_t off=0;assert(tf->write(tf,&off,false,trust,tn)==(int64_t)tn);vfs_node_put(tf);
        for(unsigned i=0;i<PACKAGE_COUNT;i++){
            size_t next_n;uint8_t *next=load(argv[SEED_ARGC+2+i],&next_n),name[32]={0};memcpy(name,packages[i],strlen(packages[i]));
            size_t before=d.events;
            assert(ext4_pkg_upgrade_source(fs,name,catalog,cn,next,next_n,artifacts[(i+1)%PACKAGE_COUNT],sizes[(i+1)%PACKAGE_COUNT],&status));assert(d.events==before&&!px);
            catalog[cn-1]^=1;assert(ext4_pkg_upgrade_source(fs,name,catalog,cn,next,next_n,artifacts[i],sizes[i],&status));assert(d.events==before&&!px);catalog[cn-1]^=1;
            int upgraded=ext4_pkg_upgrade_source(fs,name,catalog,cn,next,next_n,artifacts[i],sizes[i],&status);
            fprintf(stderr,"factory upgrade %s result=%d free-inodes=%llu\n",packages[i],upgraded,(unsigned long long)px_free(fs->engine,true,false));
            assert(!upgraded&&status.phase==PKG_TX_IDLE&&!px);
            before=d.events;assert(!ext4_pkg_upgrade_source(fs,name,catalog,cn,next,next_n,next,next_n,&status)&&d.events==before&&!px);
            free(next);
        }
        assert(!ext4_freeze_and_sync(fs));char path[1024];snprintf(path,sizeof path,"%s.upgraded.img",argv[2]);output(path,&d);teardown();free(trust);free(catalog);
        puts("PASS all factory packages upgraded through actual signed-source callbacks; foreign predecessor/signature zero-write refusals and verified repeat no-ops");
    }
    for(unsigned i=0;i<PACKAGE_COUNT;i++)free(artifacts[i]);
    free(initial);free(d.stable);free(d.volatile_bytes);
    puts("PASS factory FPK verification, installed inventory and local source retention; no catalog/signing policy injected");return 0;
}
