#define TEST_PERMISSIONS_ENFORCEMENT
#define TEST_PERMISSIONS_INTERLEAVING
#define main permissive_ext4_fixture_main
#include "ext4_mount_host.c"
#undef main
#include "perm_interleave.h"
static creds_t root,user,other;
static void identities(void) {
    creds_init_root(&root);
    user=(creds_t){.uid=1001,.euid=1001,.suid=1001,.gid=1001,.egid=1001,.sgid=1001,.umask=0027};
    other=user;other.uid=other.euid=other.suid=2001;
}
static void denied_unchanged(ext4_fault_disk_t *d,const uint8_t *bytes,size_t events,
                              ext4_engine_t *e,unsigned images,bool ready) {
    assert(!memcmp(bytes,d->stable,d->bytes));
    assert(!memcmp(bytes+d->bytes,d->volatile_bytes,d->bytes));
    assert(d->events==events && e->images==images && e->ready==ready && !e->tainted);
}
static int acceptance(const char *source,const char *prefix) {
    size_t n;uint8_t *initial=load(source,&n);identities();
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);ext4_mount_t *fs;int error;
    assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    assert(!vfs_mkdir_creds("/mnt/public",0777,&root));assert(!vfs_chmod_creds("/mnt/public",0777,&root));
    assert(!vfs_mkdir_creds("/mnt/private",0700,&root));
    assert(!vfs_mkdir_creds("/mnt/group",02777,&root));assert(!vfs_chmod_creds("/mnt/group",02777,&root));
    creds_id_change_t no_uid={.keep=true},group44={.value=44};
    assert(!vfs_chown_creds("/mnt/group",no_uid,group44,&root));
    file_t *group_file=vfs_open_mode_creds("/mnt/group/file",VFS_O_CREAT|VFS_O_RDWR,06777,&user,&error);assert(group_file);
    vfs_metadata_t group_meta;assert(!vfs_metadata(group_file->node,&group_meta) && group_meta.gid==44 && group_meta.mode==(VFS_S_IFREG|04750));vfs_close(group_file);
    assert(!vfs_mkdir_creds("/mnt/group/sub",0777,&user));
    vfs_node_t *group_dir=vfs_lookup_creds("/mnt/group/sub",&user,&error);assert(group_dir);
    assert(!vfs_metadata(group_dir,&group_meta) && group_meta.gid==44 && group_meta.mode==(VFS_S_IFDIR|02750));vfs_node_put(group_dir);
    assert(!vfs_mkdir_creds("/mnt/sticky",01777,&root));assert(!vfs_chmod_creds("/mnt/sticky",01777,&root));
    file_t *f=vfs_open_mode_creds("/mnt/private/secret",VFS_O_CREAT|VFS_O_RDWR,0600,&root,&error);assert(f);vfs_close(f);
    f=vfs_open_mode_creds("/mnt/sticky/victim",VFS_O_CREAT|VFS_O_RDWR,0600,&root,&error);assert(f);vfs_close(f);
    uint8_t *before=malloc(n*2);assert(before);memcpy(before,d.stable,n);memcpy(before+n,d.volatile_bytes,n);
    size_t events=d.events;unsigned images=fs->engine->images;bool ready=fs->engine->ready;
#define DENIED(expr,expected) do {assert((expr)==(expected));denied_unchanged(&d,before,events,fs->engine,images,ready);} while (0)
    DENIED(vfs_mkdir_creds("/mnt/private/denied",0777,&user),-VFS_EACCES);
    DENIED(vfs_chmod_creds("/mnt/private",0777,&user),-VFS_EPERM);
    creds_id_change_t uid={.value=1001},keep={.keep=true};
    DENIED(vfs_chown_creds("/mnt/private",uid,keep,&user),-VFS_EPERM);
    DENIED(vfs_unlink_creds("/mnt/sticky/victim",&user),-VFS_EPERM);
    DENIED(vfs_rename_creds("/mnt/sticky/victim","/mnt/sticky/victim",&user),-VFS_EPERM);
    assert(!vfs_open_creds("/mnt/private/secret",VFS_O_RDONLY,&user,&error) && error==-VFS_EACCES);
    denied_unchanged(&d,before,events,fs->engine,images,ready);
    assert(!vfs_lookup_creds("/mnt/private/../public",&user,&error) && error==-VFS_EACCES);
    assert(!vfs_lookup_creds("/mnt/missing/../public",&root,&error) && error==-VFS_ENOENT);
    vfs_node_t *up=vfs_lookup_creds("/mnt/..",&root,&error);assert(up==g_vfs_root);vfs_node_put(up);
    assert(!vfs_rename_creds("/mnt/private","/mnt/private",&root));
    f=vfs_open_mode_creds("/mnt/public/mode",VFS_O_CREAT|VFS_O_RDWR,06777,&user,&error);assert(f);
    vfs_metadata_t m;assert(!vfs_metadata(f->node,&m) && m.mode==(VFS_S_IFREG|06750) && m.uid==1001 && m.gid==1001);
    assert(vfs_write_creds(f,"abc",3,&user)==3);
    assert(!vfs_metadata(f->node,&m) && m.mode==(VFS_S_IFREG|0750));
    assert(!vfs_fchmod_creds(f,06770,&root));
    assert(vfs_write_creds(f,NULL,0,&user)==0 && !vfs_metadata(f->node,&m) && m.mode==(VFS_S_IFREG|06770));
    assert(!vfs_fchmod_creds(f,0,&user));assert(vfs_write_creds(f,"d",1,&user)==1);
    assert(!vfs_fchmod_creds(f,06770,&root));
    file_t *trunc=vfs_open_creds("/mnt/public/mode",VFS_O_WRONLY|VFS_O_TRUNC,&user,&error);assert(trunc);
    assert(!vfs_metadata(trunc->node,&m) && !m.size && m.mode==(VFS_S_IFREG|0770));vfs_close(trunc);
    assert(!vfs_fchmod_creds(f,06770,&root));
    trunc=vfs_open_creds("/mnt/public/mode",VFS_O_WRONLY|VFS_O_TRUNC,&user,&error);assert(trunc);
    assert(!vfs_metadata(trunc->node,&m) && !m.size && m.mode==(VFS_S_IFREG|0770));vfs_close(trunc);
    uid.value=0xffffffff;creds_id_change_t gid={.value=0xabcdef01};
    assert(!vfs_chown_creds("/mnt/public/mode",uid,gid,&root));
    assert(!vfs_metadata(f->node,&m) && m.uid==uid.value && m.gid==gid.value);
    assert(!vfs_close(f));
    f=vfs_open_creds("/mnt/public/unlinked",VFS_O_CREAT|VFS_O_RDWR,&user,&error);assert(f);
    assert(!vfs_unlink_creds("/mnt/public/unlinked",&user));
    assert(!vfs_fchmod_creds(f,0600,&user));
    assert(vfs_write_creds(f,"x",1,&user)==1 && !vfs_metadata(f->node,&m) && m.mode==(VFS_S_IFREG|0600));
    assert(!vfs_close(f));
    pthread_t worker=admission_start(1,"/mnt/public/race",&user);
    assert(!vfs_chmod_creds("/mnt/public",0700,&root));
    memcpy(before,d.stable,n);memcpy(before+n,d.volatile_bytes,n);events=d.events;images=fs->engine->images;ready=fs->engine->ready;
    assert(admission_finish(worker)==-VFS_EACCES);denied_unchanged(&d,before,events,fs->engine,images,ready);
    assert(!vfs_chmod_creds("/mnt/public",0777,&root));
    f=vfs_open_creds("/mnt/sticky/race-victim",VFS_O_CREAT|VFS_O_RDWR,&user,&error);assert(f);vfs_close(f);
    worker=admission_start(2,"/mnt/sticky/race-victim",&user);
    assert(!vfs_rename_creds("/mnt/sticky/race-victim","/mnt/public/previous",&root));
    f=vfs_open_creds("/mnt/sticky/race-victim",VFS_O_CREAT|VFS_O_RDWR,&root,&error);assert(f);vfs_close(f);
    memcpy(before,d.stable,n);memcpy(before+n,d.volatile_bytes,n);events=d.events;images=fs->engine->images;ready=fs->engine->ready;
    assert(admission_finish(worker)==-VFS_EPERM);denied_unchanged(&d,before,events,fs->engine,images,ready);
    puts("PASS EXT4 controlled VFS interleavings: chmod/new victim ownership revalidated under e4_lock, no disk or journal effects on denial");
    assert(!ext4_freeze_and_sync(fs));clean_check(fs);save(prefix,"acceptance",d.stable,n);
    teardown();free(before);free(initial);free(d.stable);free(d.volatile_bytes);
    puts("PASS actual journaled EXT4 actor admission: full-disk denied invariance, original walk/mount parent, sticky/no-op, creation/metadata/content transaction");
    return 0;
}
static int mutate(unsigned kind,file_t *file) {
    creds_id_change_t uid={.value=0x12345678},gid={.value=0x87654321};
    if (kind==0) return vfs_fchmod_creds(file,02640,&root);
    if (kind==1) return vfs_chown_creds("/mnt/metadata",uid,gid,&root);
    if (kind==2) {file->offset=3;return vfs_write_creds(file,"N",1,&user)==1 ? 0 : -VFS_EIO;}
    int error;file_t *trunc=vfs_open_creds("/mnt/metadata",VFS_O_WRONLY|VFS_O_TRUNC,&user,&error);
    if (!trunc) return error;
    return vfs_close(trunc);
}
static int metadata_cuts(const char *source,const char *prefix) {
    size_t n;uint8_t *initial=load(source,&n);identities();
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);ext4_mount_t *fs;int error;
    assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    file_t *f=vfs_open_mode_creds("/mnt/metadata",VFS_O_CREAT|VFS_O_RDWR,06777,&root,&error);assert(f);
    assert(vfs_write_creds(f,"abc",3,&root)==3);
    creds_id_change_t uid={.value=1001},gid={.value=1001};
    assert(!vfs_chown_creds("/mnt/metadata",uid,gid,&root));assert(!vfs_fchmod_creds(f,06777,&root));vfs_close(f);
    assert(!ext4_freeze_and_sync(fs));memcpy(initial,d.stable,n);teardown();
    size_t count=0;
    for (unsigned kind=0;kind<4;kind++) {
        reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
        f=vfs_open_creds("/mnt/metadata",VFS_O_RDWR,&user,&error);assert(f);d.events=0;
        assert(!mutate(kind,f));size_t events=d.events;vfs_close(f);
        bool saved[2]={false,false};
        for (unsigned persistence=0;persistence<4;persistence++) for (size_t cut=0;cut<events;cut++) for (unsigned after=0;after<2;after++) {
            reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
            f=vfs_open_creds("/mnt/metadata",VFS_O_RDWR,&user,&error);assert(f);
            d.events=0;d.cut=(long)cut;d.after=after;d.persistence=persistence;
            (void)mutate(kind,f);assert(d.offline);vfs_close(f);
            teardown();ext4_fault_restart(&d);vfs_init();
            assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
            vfs_node_t *node=vfs_lookup_ref("/mnt/metadata",&error);assert(node);
            vfs_metadata_t m;assert(!vfs_metadata(node,&m));vfs_node_put(node);
            bool old=m.mode==(VFS_S_IFREG|06777) && m.uid==1001 && m.gid==1001 && m.size==3;
            bool next=m.mode==(VFS_S_IFREG|(kind==0 ? 02640 : 0777)) &&
                m.uid==(kind==1 ? 0x12345678 : 1001) && m.gid==(kind==1 ? 0x87654321 : 1001) &&
                m.size==(kind==2 ? 4 : kind==3 ? 0 : 3);
            assert(old || next);unsigned state=next;
            f=vfs_open("/mnt/metadata",VFS_O_RDONLY);assert(f);char bytes[4]={0};
            assert(vfs_read(f,bytes,4)==(int64_t)m.size);
            if (m.size) assert(!memcmp(bytes,m.size==4 ? "abcN" : "abc",m.size));
            vfs_close(f);
            assert(!ext4_freeze_and_sync(fs));clean_check(fs);
            if (!saved[state]) {
                char suffix[32];snprintf(suffix,sizeof(suffix),"op%u-%s",kind,state ? "new" : "old");
                save(prefix,suffix,d.stable,n);saved[state]=true;
            }
            count++;
        }
        assert(saved[0] && saved[1]);
        printf("PASS EXT4 metadata/content op%u: %zu events, all atomic cuts, full old-or-new mode/IDs/size/bytes\n",kind,events);fflush(stdout);
    }
    teardown();free(initial);free(d.stable);free(d.volatile_bytes);
    printf("PASS EXT4 metadata/content crash inventory: %zu cuts\n",count);return 0;
}
int main(int argc,char **argv) {
    if (argc==4 && !strcmp(argv[3],"--cuts")) return metadata_cuts(argv[1],argv[2]);
    assert(argc==3);return acceptance(argv[1],argv[2]);
}
