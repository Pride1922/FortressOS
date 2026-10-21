/* Included only by the explicit host admission gate. No kernel test hook. */
static vfs_node_t *ns_lookup(const char *path) {
    int error;vfs_node_t *node=e4_ac_lookup(mounted,path,&error);assert(node);return node;
}
static void ns_denials(void) {
    const char *paths[]={"/etc","/var/lib/fortress/accounts"};
    creds_t actor;creds_init_root(&actor);
    for (unsigned i=0;i<2;i++) {
        vfs_node_t *dir=ns_lookup(paths[i]);int error;unsigned before=writes_seen;
        assert(!e4_jcreate_common(dir,"foreign",VFS_FILE,&error,true,NULL,&actor));
        assert(error==-VFS_EROFS && writes_seen==before);
        assert(e4_junlink_common(dir,i ? "pending" : "passwd",false,&actor)==-VFS_EROFS);
        assert(writes_seen==before);
        vfs_node_t *root=ns_lookup("/");
        assert(e4_jrename_common(root,"sentinel",dir,"foreign",false,&actor)==-VFS_EROFS);
        assert(e4_jrename_common(dir,i ? "pending" : "passwd",root,"stolen",false,&actor)==-VFS_EROFS);
        assert(e4_jreplace(root,"sentinel",dir,i ? "pending" : "passwd",&actor)==-VFS_EROFS);
        assert(writes_seen==before);vfs_node_put(root);vfs_node_put(dir);
    }
    const char *ancestors[]={"/etc","/var","/var/lib","/var/lib/fortress","/var/lib/fortress/accounts"};
    for (unsigned i=0;i<5;i++) {
        vfs_node_t *node=ns_lookup(ancestors[i]);vfs_node_t *parent=node->parent;assert(parent);
        unsigned before=writes_seen;
        assert(e4_jrename_common(parent,node->name,parent,"relocated",true,&actor)==-VFS_EROFS);
        assert(e4_junlink_common(parent,node->name,true,&actor)==-VFS_EROFS);
        creds_id_change_t unchanged={0};
        assert(e4_setattr_actor(node,false,0700,unchanged,unchanged,&actor)==-VFS_EROFS);
        assert(writes_seen==before);vfs_node_put(node);
    }
    /* Guard ordinary data/metadata callbacks, including pre-existing references. */
    vfs_node_t *file=ns_lookup("/etc/passwd");unsigned before=writes_seen;uint64_t offset=0;
    assert(e4_jwrite(file,&offset,false,"x",1)==-VFS_EROFS);
    assert(e4_jtruncate(file,0)==-VFS_EROFS);
    creds_id_change_t unchanged={0};
    assert(e4_setattr_actor(file,false,0600,unchanged,unchanged,&actor)==-VFS_EROFS);
    assert(writes_seen==before);vfs_node_put(file);
    /* Ancestor fencing does not freeze unrelated entries under /var. */
    vfs_node_t *var=ns_lookup("/var");int error;
    vfs_node_t *ordinary=e4_jcreate_common(var,"ordinary",VFS_FILE,&error,true,NULL,&actor);
    assert(ordinary);vfs_node_put(ordinary);assert(!e4_junlink_common(var,"ordinary",false,&actor));vfs_node_put(var);
    puts("FENCE PASS create/unlink/rename/replace/ancestor/setattr/write/truncate; root actor; unrelated /var allowed");
}
static bool ns_kernel_test(const char *mode) {
    if (!strcmp(mode,"setup")) return false;
    ext4_account_namespace_report_t report={0};unsigned before=writes_seen;
    if (!strcmp(mode,"reject") || !strcmp(mode,"oom") || !strcmp(mode,"read-error")) {
        if (!strcmp(mode,"oom")) fail_alloc=0;
        if (!strcmp(mode,"read-error")) {e4_cache_drop(mounted,true);ns_deny_reads=true;}
        int result=ext4_root_account_namespace_admit(mounted,&report);fail_alloc=-1;ns_deny_reads=false;
        assert(result<0 && !mounted->account_namespace_fenced && writes_seen==before);
        assert(!report.slots[0] && !report.slots[1] && !report.present_roles);
        printf("REJECT PASS error=%d writes=0 fence=off\n",result);return true;
    }
    if (!strcmp(mode,"legacy")) {
        vfs_node_t *dir=ns_lookup("/etc");int error;
        vfs_node_t *node=e4_jcreate_common(dir,"foreign",VFS_FILE,&error,true,NULL,NULL);
        assert(node);vfs_node_put(node);assert(!e4_junlink_common(dir,"foreign",false,NULL));vfs_node_put(dir);
        assert(!mounted->account_namespace_fenced);puts("LEGACY PASS ordinary /etc mutation allowed");return true;
    }
    const char *paths[]={"/etc","/etc/passwd","/etc/group","/etc/shadow"};
    vfs_node_t *locked[4];for (unsigned i=0;i<4;i++) locked[i]=ns_lookup(paths[i]);
    assert(!ext4_root_lock_accounts(mounted,locked));
    for (unsigned i=0;i<4;i++) vfs_node_put(locked[i]);
    vfs_node_t *retained=ns_lookup("/etc/passwd");
    assert(!ext4_root_account_namespace_admit(mounted,&report));
    assert(writes_seen==before && report.slots[0]>=6 && report.slots[1]>=5);
    assert(mounted->account_namespace_fenced);
    assert(!ext4_root_account_namespace_admit(mounted,&report) && writes_seen==before);
    uint64_t offset=0;assert(e4_jwrite(retained,&offset,false,"x",1)==-VFS_EROFS);
    vfs_node_put(retained);assert(writes_seen==before);
    mounted->engine->tainted=true;
    assert(ext4_root_account_namespace_admit(mounted,&report)==-VFS_EIO);
    assert(mounted->account_namespace_fenced && !report.slots[0]);mounted->engine->tainted=false;
    assert(!ext4_root_account_namespace_admit(mounted,&report) && writes_seen==before);
    printf("ADMIT PASS etc_slots=%u private_slots=%u roles=%u writes=0\n",report.slots[0],report.slots[1],report.present_roles);
    ns_denials();
    assert(!ext4_publish_root(mounted));before=writes_seen;
    assert(ext4_root_account_namespace_admit(mounted,&report)==-VFS_EROFS);
    assert(mounted->account_namespace_fenced && writes_seen==before);
    return false;
}
