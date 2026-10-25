"""Private actual-filesystem executor hooks; never edits production sources."""
def overlay(work,replace):
    replace('src/fs/ext4.c','static spinlock_t e4_lock =','\n#include "../../tests/pkg_install_forward.inc"\nstatic spinlock_t e4_lock =')
    replace('src/fs/ext4_transaction.inc','static int e4_semantic_commit(ext4_engine_t *e) {','static int px_core_commit(ext4_engine_t *e) {')
    replace('src/fs/ext4_namespace.inc','    e4_inode_sum(e->fs,ino,raw);\n    r=e4_dir_edit(e,&dir,name,ino,directory ? 2 : 1);','    e4_inode_sum(e->fs,ino,raw);\n    px_created_inode=ino;\n    r=e4_dir_edit(e,&dir,name,ino,directory ? 2 : 1);')
    replace('src/fs/ext4.c','static bool e4_cache_room(ext4_mount_t *fs) {','static bool e4_cache_room(ext4_mount_t *fs) {\n    if(!px_cache_room(fs))return false;')
    replace('src/fs/ext4.c','e4_node_t *n=kcalloc(1,sizeof(*n));','e4_node_t *n=px_node_new(fs);')
    replace('src/fs/ext4.c','kfree(node);return true;','px_node_drop(node);return true;')
    replace('src/fs/ext4.c','kfree(fs->cached[i]);','px_node_drop(fs->cached[i]);')
    replace('src/fs/ext4_orphan.inc','    if (e4_orphan_index(e,ino)>=0) return -VFS_EAGAIN;\n    if (e->orphan_count==64) return -VFS_EFBIG;','    if (e4_orphan_index(e,ino)>=0) return -VFS_EAGAIN;\n    if(!px_orphan_room(e))return -VFS_EAGAIN;\n    if (e->orphan_count==64) return -VFS_EFBIG;')
    replace('src/fs/ext4_orphan.inc','    for (;;) {\n        int r=e4_orphan_begin(e);','    for (;;) {\n        if(px_orphan_pause(e))return -VFS_EAGAIN;\n        int r=e4_orphan_begin(e);')
    replace('src/fs/ext4_directory_move.inc','e4_move_cache_t *p=kcalloc(1,sizeof(*p));','e4_move_cache_t *p=px_publication_new();')
    replace('src/fs/ext4_directory_move.inc','    kfree(p);return r;','    px_publication_drop(p);return r;')
    replace('src/fs/ext4_mount_journal.inc','if (!r) {node=kcalloc(1,sizeof(*node));if (!node) r=-VFS_ENOMEM;}','if (!r) {node=px_node_new(fs);if (!node) r=-VFS_ENOMEM;}')
    replace('src/fs/ext4_mount_journal.inc','if (r) { kfree(node);vfs_set_last_create_error(r);','if (r) { px_node_drop(node);vfs_set_last_create_error(r);')
    for name in ('src/fs/ext4_mount_journal.inc','src/fs/ext4_write.inc'):
        p=work/name;text=p.read_text();assert 'kfree(publication)' in text;replace(name,text,text.replace('kfree(publication)','px_publication_drop(publication)'))
    with (work/'src/fs/ext4.c').open('a') as f:f.write('\n#include "../../tests/pkg_install_fs.inc"\n')
