/* Actual planners at exact physical block/inode reservation margins. */
static void limit_before(ext4_engine_t *e,ext4_fault_disk_t *disk){
    uint64_t handle;assert(!ext4_engine_handle_open(e,pn_objects[0].inode,&handle));assert(!ext4_engine_handle_dup(e,handle));
    uint64_t flags=spin_lock_irqsave(&e4_lock);size_t events=disk->events;
    assert(pn_step(e)==-VFS_EAGAIN&&disk->events==events);
    spin_unlock_irqrestore(&e4_lock,flags);assert(!ext4_engine_handle_close(e,handle));
    flags=spin_lock_irqsave(&e4_lock);assert(pn_step(e)==-VFS_EAGAIN&&disk->events==events);
    spin_unlock_irqrestore(&e4_lock,flags);assert(!ext4_engine_handle_close(e,handle));
    flags=spin_lock_irqsave(&e4_lock);assert(!native_validate(e));assert(!e4_journal_mark(e,false));events=disk->events;uint32_t ino=0;
    assert(pn_input.free[0]==pn_view.budget.remaining[0]+64);
    assert(pn_input.free[1]==pn_view.budget.remaining[1]+16);
    assert(ext4_engine_namespace_create(e,2,"denied-by-inode-reserve",false,&ino)==-VFS_ENOSPC);
    assert(disk->events==events);assert(!native_validate(e));
    e4_inode_t pad;assert(!identity_path(e->fs,"/ordinary-pad",&pad));uint64_t offset=pad.size;
    memset(pn_scratch,0x51,4096);
    assert(ext4_engine_file_write(e,pad.ino,&offset,true,pn_scratch,4096)==-VFS_ENOSPC);
    assert(disk->events==events);assert(!native_validate(e));
    /* Rewriting reserved content also fails with ample logical write credits. */
    uint32_t grant=e4_u32(pn_workspace.baseline.parts[0].p+416+128+4);offset=0;
    assert(ext4_engine_file_write(e,grant,&offset,false,pn_scratch,1)==-VFS_EACCES);
    assert(disk->events==events);assert(!native_validate(e));
    spin_unlock_irqrestore(&e4_lock,flags);
    puts("PASS actual block/inode reserve margins and protected-content fence, denied writes=0");
}
static void limit_after(ext4_engine_t *e,ext4_fault_disk_t *disk){
    uint64_t flags=spin_lock_irqsave(&e4_lock);uint32_t ino=0;size_t events=disk->events;
    assert(!native_validate(e)&&pn_view.action==PP_DONE);
    assert(!ext4_engine_namespace_create(e,2,"allowed-after-release",false,&ino));
    assert(ino&&disk->events>events);assert(!native_validate(e));
    spin_unlock_irqrestore(&e4_lock,flags);
    puts("PASS final durable release restores ordinary allocation without losing executable baseline ownership");
}
