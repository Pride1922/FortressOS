/* Actual EXT4/JBD2 upgrade, reboot replay and sector-atomic fault injection.
 * Includes host disk/credentials; no scheduler or physical-device claim. */
#define PKG_ORDINARY_INCLUDED
#include "pkg_ordinary_install_host.c"

static void verify_command(const uint8_t hash[32]) {
    file_t *f=vfs_open("/mnt/usr/bin/rot13",VFS_O_RDONLY);assert(f);
    vfs_metadata_t meta;assert(!vfs_metadata(f->node,&meta));
    uint8_t *data=malloc(meta.size),digest[32];assert(data);
    assert(vfs_read(f,data,meta.size)==(int64_t)meta.size);
    assert(!crypto_sha256(data,meta.size,digest)&&!memcmp(hash,digest,32));
    free(data);assert(!vfs_close(f));
}
static void status_build(ext4_mount_t *fs,unsigned expected) {
    px_context *c=px_allocate(sizeof *c);assert(c);c->fs=fs;
    assert(e4_bytes(fs,1128,c->policy.filesystem,16));
    uint64_t flags=spin_lock_irqsave(&e4_lock);px=c;assert(!px_record_read(fs));px=NULL;spin_unlock_irqrestore(&e4_lock,flags);
    assert(!c->state.phase&&c->state.installed&&c->state.floor==expected);pu_drop(c);
}
int main(int argc,char **argv) {
    assert(argc==11||argc==12);size_t n,an,bn,cn,dn,en,fn,tn,hn;
    uint8_t *initial=load(argv[1],&n),*a=load(argv[2],&an),*b=load(argv[3],&bn),*c=load(argv[4],&cn);
    uint8_t *ca=load(argv[5],&dn),*cb=load(argv[6],&en),*cc=load(argv[7],&fn),*trust=load(argv[8],&tn),*hashes=load(argv[9],&hn);
    assert(hn==96);ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable&&d.volatile_bytes);
    memcpy(d.stable,initial,n);memcpy(d.volatile_bytes,initial,n);
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    vfs_init();ext4_mount_t *fs=NULL;assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    if(argc==12) {
        uint8_t package[32]={0};memcpy(package,"rot13",5);pkg_transaction_status s={0};size_t events=d.events;
        int r=ext4_pkg_upgrade_source(fs,package,cb,en,b,bn,a,an,&s);
        fprintf(stderr,"full filesystem upgrade result=%d\n",r);
        assert(r==-VFS_ENOSPC&&d.events==events);verify_command(hashes);status_build(fs,1);
        assert(!ext4_freeze_and_sync(fs));teardown();
        free(initial);free(a);free(b);free(c);free(ca);free(cb);free(cc);free(trust);free(hashes);free(d.stable);free(d.volatile_bytes);
        puts("PASS actual full-filesystem refusal before staging; old executable/inventory preserved");return 0;
    }
    assert(!px_ensure_dir(fs,"/etc/pkg",0755));
    file_t *f=vfs_open("/mnt/etc/pkg/trust.conf",VFS_O_CREAT|VFS_O_WRONLY);assert(f&&vfs_write(f,trust,tn)==(int64_t)tn&&!vfs_close(f));
    f=vfs_open("/mnt/root/sentinel",VFS_O_CREAT|VFS_O_WRONLY);assert(f&&vfs_write(f,"settings",8)==8&&!vfs_close(f));
    uint8_t package[32]={0};memcpy(package,"rot13",5);pkg_transaction_status s={0};
    assert(!ext4_pkg_install_source(fs,package,ca,dn,a,an,&s));verify_command(hashes);status_build(fs,1);
    size_t refused_events=d.events;
    a[an-1]^=1;assert(ext4_pkg_upgrade_source(fs,package,cb,en,b,bn,a,an,&s)==-VFS_EACCES&&d.events==refused_events);a[an-1]^=1;
    fail_alloc=0;assert(ext4_pkg_upgrade_source(fs,package,cb,en,b,bn,a,an,&s)==-VFS_ENOMEM&&d.events==refused_events);fail_alloc=-1;
    uint8_t *installed=malloc(n);assert(installed);memcpy(installed,d.stable,n);
    char initial_path[512];assert(strlen(argv[10])+11<sizeof initial_path);
    snprintf(initial_path,sizeof initial_path,"%s.installed",argv[10]);
    FILE *snapshot_file=fopen(initial_path,"wb");assert(snapshot_file&&fwrite(installed,1,n,snapshot_file)==n&&!fclose(snapshot_file));
    /* Hold the old executable open through activation. Its retained bytes
     * remain readable while future opens resolve the new inode. */
    file_t *pin=vfs_open("/mnt/usr/bin/rot13",VFS_O_RDONLY);assert(pin);
    d.events=d.writes=d.flushes=0;
    int r=ext4_pkg_upgrade_source(fs,package,cb,en,b,bn,a,an,&s);
    fprintf(stderr,"upgrade result=%d events=%zu\n",r,d.events);assert(!r);
    size_t total=d.events;verify_command(hashes+32);status_build(fs,2);
    uint8_t *oldbytes=malloc(pin->node->size),digest[32];assert(oldbytes);
    assert(vfs_read(pin,oldbytes,pin->node->size)==(int64_t)pin->node->size);
    assert(!crypto_sha256(oldbytes,pin->node->size,digest)&&!memcmp(digest,hashes,32));free(oldbytes);assert(!vfs_close(pin));
    size_t events=d.events;
    assert(!ext4_pkg_upgrade_source(fs,package,cb,en,b,bn,b,bn,&s)&&d.events==events);
    assert(ext4_pkg_upgrade_source(fs,package,ca,dn,a,an,b,bn,&s)&&d.events==events);
    cb[en-1]^=1;assert(ext4_pkg_upgrade_source(fs,package,cb,en,b,bn,b,bn,&s)&&d.events==events);cb[en-1]^=1;
    assert(!ext4_pkg_upgrade_source(fs,package,cc,fn,c,cn,b,bn,&s));verify_command(hashes+64);status_build(fs,3);
    assert(!ext4_freeze_and_sync(fs));teardown();ext4_fault_restart(&d);vfs_init();
    assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));verify_command(hashes+64);status_build(fs,3);
    assert(!ext4_freeze_and_sync(fs));
    FILE *out=fopen(argv[10],"wb");assert(out&&fwrite(d.stable,1,n,out)==n&&!fclose(out));teardown();
    printf("PASS two consecutive upgrades, no-op, rollback/bad signature refusal, old-reader retention and reboot\n");fflush(stdout);
    /* Sample the full staging campaign plus every event in the final atomic
     * activation/checkpoint window. Try cached and write-through media. */
    unsigned cuts=0;
    for(unsigned mode=0;mode<2;mode++)for(size_t cut=0;cut<=total;cut++) {
        if(cut%97&&cut+200<total)continue;
        memcpy(d.stable,installed,n);ext4_fault_restart(&d);d.persistence=mode;
        vfs_init();assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
        d.events=d.writes=d.flushes=0;d.cut=(long)cut;
        (void)ext4_pkg_upgrade_source(fs,package,cb,en,b,bn,a,an,&s);
        teardown();ext4_fault_restart(&d);d.persistence=0;vfs_init();
        assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
        f=vfs_open("/mnt/usr/bin/rot13",VFS_O_RDONLY);assert(f);vfs_metadata_t meta;assert(!vfs_metadata(f->node,&meta));
        uint8_t *data=malloc(meta.size);assert(data&&vfs_read(f,data,meta.size)==(int64_t)meta.size&&!vfs_close(f));
        assert(!crypto_sha256(data,meta.size,digest));free(data);
        bool new_version=!memcmp(digest,hashes+32,32);assert(new_version||!memcmp(digest,hashes,32));status_build(fs,new_version?2:1);
        if(!new_version){assert(!ext4_pkg_upgrade_source(fs,package,cb,en,b,bn,a,an,&s));verify_command(hashes+32);status_build(fs,2);}
        f=vfs_open("/mnt/root/sentinel",VFS_O_RDONLY);char sentinel[8];assert(f&&vfs_read(f,sentinel,8)==8&&!memcmp(sentinel,"settings",8)&&!vfs_close(f));
        assert(!ext4_freeze_and_sync(fs));teardown();cuts++;
    }
    printf("PASS %u interruption/replay/retry cases; executable and inventory always agree\n",cuts);
    free(installed);free(initial);free(a);free(b);free(c);free(ca);free(cb);free(cc);free(trust);free(hashes);free(d.stable);free(d.volatile_bytes);
    return 0;
}
