/* Entirely excluded from ordinary builds. One synchronous planned append;
 * no deferred state, new lock, barrier removal or ordinary-write limit change. */
int64_t ext4_stage_test_write(file_t *file,const void *bytes,size_t count,
                             uint64_t expected,const creds_t *actor) {
    if (!file || !file->node || (file->flags&VFS_O_ACCMODE)==VFS_O_RDONLY) return -VFS_EBADF;
    if (!bytes || !actor || !count || count>EXT4_STAGE_TEST_MAX ||
        expected>8ULL*1024*1024*1024 || count>8ULL*1024*1024*1024-expected) return -VFS_EINVAL;
    if (file->flags&VFS_O_APPEND) return -VFS_EOPNOTSUPP;
    if (file->node->write_actor!=e4_write_actor || file->node->type!=VFS_FILE) return -VFS_EOPNOTSUPP;
    spin_debug_assert_unheld();uint64_t flags=spin_lock_irqsave(&e4_lock);
    e4_node_t *node=(e4_node_t *)file->node;ext4_mount_t *fs=node->inode.fs;
    int r=e4_state(&node->node);int64_t result=r;
    ext4_engine_t *e=fs->engine;e4_inode_t in;
    if (r) goto done;
    if (!fs->journal_mounted || node->removed) {result=-VFS_EOPNOTSUPP;goto done;}
    if (e->orphan_recovery_required || e->ready || jbd2_writer_state(e->writer)!=JBD2_WRITE_IDLE) {result=-VFS_EAGAIN;goto done;}
    r=e4_inode(fs,node->inode.ino,&in);if (r) {result=r;goto done;}
    if (in.generation!=node->inode.generation) {result=-VFS_EIO;goto done;}
    if (in.mode!=VFS_S_IFREG) {result=-VFS_EOPNOTSUPP;goto done;}
    if (in.size!=expected || file->offset!=expected) {result=-VFS_EAGAIN;goto done;}
    for (unsigned i=0;i<e->orphan_count;i++) if(e->orphans[i]==in.ino) {result=-VFS_EAGAIN;goto done;}
    size_t limit=32u*fs->bs-(size_t)(expected%fs->bs);if(count>limit)count=limit;
    uint64_t next=expected;e->file_planning=true;
    r=e4_file_plan(e,in.ino,&next,false,bytes,count);e->file_planning=false;
    if (!r) {
        uint8_t *raw=e4_inode_image(e,in.ino);
        if (!raw) r=e->error;
        else if(e4_u16(raw+26)!=1) r=-VFS_EOPNOTSUPP;
        else {
            vfs_metadata_t value=e4_value(&in);
            e4_p16(raw,permission_content_mode(&value,actor));e4_inode_sum(fs,in.ino,raw);
        }
    }
    if (!r) r=e4_semantic_commit(e);
    if (r) ext4_engine_abort(e);
    e4_semantic_invalidate(e);
    if (!r && e4_journal_refresh(&node->node)) {e4_jtaint(fs);r=-VFS_EIO;}
    if (!r) file->offset=next;
    result=r ? r : (int64_t)count;
done:
    spin_unlock_irqrestore(&e4_lock,flags);return result;
}
int ext4_stage_test_sync(file_t *file) {
    if (!file || !file->node) return -VFS_EBADF;
    if (file->node->metadata!=e4_metadata ||
        (file->node->type!=VFS_FILE && file->node->type!=VFS_DIRECTORY)) return -VFS_EOPNOTSUPP;
    e4_node_t *node=(e4_node_t *)file->node;
    if (!node->inode.fs->journal_mounted) return -VFS_EOPNOTSUPP;
    return ext4_sync(node->inode.fs);
}
