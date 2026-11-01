/* Actual ordinary EXT4/JBD2 code; only disk and credentials are host adapters. */
#define PX_COST_DIAGNOSTIC(s,o) fprintf(stderr,"cost phase=%u made=%u gone=%u trash=%u owned=%llu/%llu charged=%llu/%llu\n",(s)->phase,(s)->made,(s)->gone,(s)->trash,(unsigned long long)(o)[0],(unsigned long long)(o)[1],(unsigned long long)(s)->charged[0],(unsigned long long)(s)->charged[1])
#define PKG_ORDINARY_INCLUDED
#include "pkg_ordinary_install_host.c"
static uint8_t hashes[160];
static uint8_t *art[5],*cat[4];static size_t an[5],cn[4];
static void command(const char *name,unsigned which) {
    char path[128];snprintf(path,sizeof path,"/mnt/usr/bin/%s",name);
    file_t *f=vfs_open(path,VFS_O_RDONLY);assert(f);vfs_metadata_t m;assert(!vfs_metadata(f->node,&m));
    unsigned index=!strcmp(name,"toolone")?(which==1?0:which==2?2:3):(which==1?1:4);
    uint8_t *data=malloc(m.size),digest[32];assert(data);
    assert(vfs_read(f,data,m.size)==(int64_t)m.size&&!crypto_sha256(data,m.size,digest)&&!memcmp(digest,hashes+index*32,32));
    free(data);assert(!vfs_close(f));
}
static int install(ext4_mount_t *fs,const char *name,unsigned ci,unsigned ai) {
    uint8_t package[32]={0};memcpy(package,name,strlen(name));pkg_transaction_status st={0};
    return ext4_pkg_install_source(fs,package,cat[ci],cn[ci],art[ai],an[ai],&st);
}
static int upgrade(ext4_mount_t *fs,const char *name,unsigned ci,unsigned ai,unsigned previous) {
    uint8_t package[32]={0};memcpy(package,name,strlen(name));pkg_transaction_status st={0};
    return ext4_pkg_upgrade_source(fs,package,cat[ci],cn[ci],art[ai],an[ai],art[previous],an[previous],&st);
}
int main(int argc,char **argv) {
    assert(argc==15);size_t hn;uint8_t *h=load(argv[13],&hn);assert(hn==sizeof hashes);memcpy(hashes,h,hn);free(h);size_t bytes,tn;uint8_t *initial=load(argv[1],&bytes),*trust=load(argv[2],&tn);
    for(unsigned i=0;i<5;i++)art[i]=load(argv[3+i],an+i);
    for(unsigned i=0;i<4;i++)cat[i]=load(argv[8+i],cn+i);
    ext4_fault_disk_t d={.stable=malloc(bytes),.volatile_bytes=malloc(bytes),.bytes=bytes,.cut=-1};
    assert(d.stable&&d.volatile_bytes);memcpy(d.stable,initial,bytes);memcpy(d.volatile_bytes,initial,bytes);
    block_dev_t dev={.sector_size=512,.sector_count=bytes/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    vfs_init();ext4_mount_t *fs=NULL;assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!px_ensure_dir(fs,"/etc/pkg",0755));file_t *f=vfs_open("/mnt/etc/pkg/trust.conf",VFS_O_CREAT|VFS_O_WRONLY);
    assert(f&&vfs_write(f,trust,tn)==(int64_t)tn&&!vfs_close(f));
    assert(!install(fs,"toolone",0,0));command("toolone",1);
    uint8_t legacy[8192];f=vfs_open("/mnt/var/lib/pkg/install.v2",VFS_O_RDONLY);assert(f&&vfs_read(f,legacy,sizeof legacy)==sizeof legacy&&!vfs_close(f));
    uint8_t *only_a=malloc(bytes);assert(only_a);memcpy(only_a,d.stable,bytes);
    d.events=d.writes=d.flushes=0;
    assert(!install(fs,"tooltwo",0,1));command("tooltwo",1);
    size_t install_events=d.events;
    uint8_t *both=malloc(bytes);assert(both);memcpy(both,d.stable,bytes);
    uint8_t unchanged[8192];f=vfs_open("/mnt/var/lib/pkg/install.v2",VFS_O_RDONLY);assert(f&&vfs_read(f,unchanged,sizeof unchanged)==sizeof unchanged&&!vfs_close(f));assert(!memcmp(legacy,unchanged,sizeof legacy));
    px_inventory *all=calloc(1,sizeof *all);assert(all&&!px_inventory_read(fs,all));assert(all->records[0].floor==1&&all->records[1].floor==1);
    d.events=d.writes=d.flushes=0;
    assert(!upgrade(fs,"toolone",1,2,0));size_t upgrade_events=d.events;command("toolone",2);command("tooltwo",1);
    assert(!upgrade(fs,"toolone",2,3,2));command("toolone",3);command("tooltwo",1);
    assert(!upgrade(fs,"tooltwo",3,4,1));command("tooltwo",2);command("toolone",3);
    size_t events=d.events;assert(install(fs,"toolone",0,0)&&d.events==events);
    assert(!install(fs,"tooltwo",3,4)&&d.events==events);
    assert(!px_ensure_dir(fs,"/bin",0755));
    f=vfs_open("/mnt/bin/tooltwo",VFS_O_CREAT|VFS_O_WRONLY);assert(f&&vfs_write(f,"base",4)==4&&!vfs_close(f));
    events=d.events;assert(install(fs,"tooltwo",3,4)==-VFS_EACCES&&d.events==events);
    assert(!vfs_unlink("/mnt/bin/tooltwo"));
    /* Inventory discovery rejects duplicate identities/command owners and
     * malformed records before any new package can publish a command. */
    uint8_t saved_record[8192],changed[8192];
    f=vfs_open("/mnt/var/lib/pkg/install-01.v2",VFS_O_RDONLY);assert(f&&vfs_read(f,saved_record,8192)==8192&&!vfs_close(f));
    for(unsigned kind=0;kind<3;kind++) {
        memcpy(changed,saved_record,8192);
        if(kind<2){unsigned offset=kind?352:320;memset(changed+offset,0,32);memcpy(changed+offset,"toolone",7);assert(!crypto_sha256(changed,8160,changed+8160));}
        else changed[17]^=1;
        f=vfs_open("/mnt/var/lib/pkg/install-01.v2",VFS_O_WRONLY);assert(f&&vfs_write(f,changed,8192)==8192&&!vfs_close(f));
        assert(px_inventory_read(fs,all));
        f=vfs_open("/mnt/var/lib/pkg/install-01.v2",VFS_O_WRONLY);assert(f&&vfs_write(f,saved_record,8192)==8192&&!vfs_close(f));
    }
    assert(!px_inventory_read(fs,all));
    char path[512];size_t extra_n,extra_catalog_n;
    snprintf(path,sizeof path,"%s/capacity-cat",argv[14]);uint8_t *extra_catalog=load(path,&extra_catalog_n);
    snprintf(path,sizeof path,"%s/thief",argv[14]);uint8_t *extra=load(path,&extra_n);
    uint8_t extra_name[32]={0};memcpy(extra_name,"thief",5);pkg_transaction_status status={0};events=d.events;
    assert(ext4_pkg_install_source(fs,extra_name,extra_catalog,extra_catalog_n,extra,extra_n,&status)==-VFS_EACCES&&d.events==events);free(extra);
    f=vfs_open("/mnt/usr/bin/foreign",VFS_O_CREAT|VFS_O_WRONLY);assert(f&&vfs_write(f,"foreign",7)==7&&!vfs_close(f));
    snprintf(path,sizeof path,"%s/foreign",argv[14]);extra=load(path,&extra_n);memset(extra_name,0,32);memcpy(extra_name,"foreign",7);
    assert(ext4_pkg_install_source(fs,extra_name,extra_catalog,extra_catalog_n,extra,extra_n,&status));free(extra);
    for(unsigned i=0;i<15;i++) {
        snprintf(path,sizeof path,"%s/capacity-art%u",argv[14],i);extra=load(path,&extra_n);memset(extra_name,0,32);snprintf((char *)extra_name,32,"c%02u",i);
        events=d.events;int r=ext4_pkg_install_source(fs,extra_name,extra_catalog,extra_catalog_n,extra,extra_n,&status);
        if(i<14)assert(!r);else assert(r==-VFS_ENOSPC&&d.events==events);
        free(extra);
    }
    free(extra_catalog);assert(!px_inventory_read(fs,all));
    puts("PASS duplicate identity/command, malformed inventory, cross-package ownership, foreign file, 16-package capacity refusal");
    assert(!ext4_freeze_and_sync(fs));teardown();ext4_fault_restart(&d);vfs_init();assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    command("toolone",3);command("tooltwo",2);assert(!px_inventory_read(fs,all));assert(all->records[0].floor==3&&all->records[1].floor==2);
    assert(!ext4_freeze_and_sync(fs));FILE *out=fopen(argv[12],"wb");assert(out&&fwrite(d.stable,1,bytes,out)==bytes&&!fclose(out));teardown();
    unsigned cuts=0;
    for(unsigned operation=0;operation<2;operation++)for(unsigned mode=0;mode<2;mode++) {
        size_t total=operation?upgrade_events:install_events;
        for(size_t cut=0;cut<=total;cut++) {
            const char *only_cut=getenv("PKG_TEST_CUT");
            if(only_cut&&(operation||mode||cut!=strtoull(only_cut,NULL,10)))continue;
            if(!only_cut&&cut%47&&cut+64<total)continue;
            memcpy(d.stable,operation?both:only_a,bytes);ext4_fault_restart(&d);d.persistence=mode;vfs_init();
            assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));d.events=d.writes=d.flushes=0;d.cut=(long)cut;
            if(operation)(void)upgrade(fs,"toolone",1,2,0);else (void)install(fs,"tooltwo",0,1);
            teardown();ext4_fault_restart(&d);d.persistence=0;vfs_init();assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
            if(operation) {
                assert(!px_inventory_read(fs,all));unsigned build=all->records[0].floor;assert(build==1||build==2);
                command("toolone",build);command("tooltwo",1);
                if(build==1)assert(!upgrade(fs,"toolone",1,2,0));
            } else {
                command("toolone",1);
                f=vfs_open("/mnt/var/lib/pkg/install.v2",VFS_O_RDONLY);assert(f&&vfs_read(f,unchanged,sizeof unchanged)==sizeof unchanged&&!vfs_close(f));assert(!memcmp(legacy,unchanged,sizeof legacy));
                int retry=install(fs,"tooltwo",0,1);if(retry)fprintf(stderr,"install retry mode=%u cut=%zu/%zu error=%d\n",mode,cut,total,retry);assert(!retry);command("tooltwo",1);
            }
            assert(!ext4_freeze_and_sync(fs));teardown();cuts++;
        }
    }
    printf("PASS %u install/upgrade interruption-replay-retry cuts with independent package preservation\n",cuts);
    free(only_a);free(both);
    free(all);for(unsigned i=0;i<5;i++)free(art[i]);for(unsigned i=0;i<4;i++)free(cat[i]);free(initial);free(trust);free(d.stable);free(d.volatile_bytes);
    puts("PASS legacy record preservation, two same-build packages, independent consecutive upgrades, cross-package catalog floor, no-op and reboot");return 0;
}
