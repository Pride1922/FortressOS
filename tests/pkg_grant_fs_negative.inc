/* Included only by the private actual-filesystem host adapter. */
static void negative_tests(ext4_mount_t *fs){
    static uint8_t good_l[1024],good_t[8192];
    pg_view v,sentinel;
    uint64_t lock=spin_lock_irqsave(&e4_lock);assert(!validate(fs,&v));
    memcpy(good_l,ledger,sizeof ledger);memcpy(good_t,table,sizeof table);
    spin_unlock_irqrestore(&e4_lock,lock);
    ext4_fault_disk_t *d=fs->dev->priv;
    unsigned negatives=0;
    for(unsigned test=0;test<5;test++){
        memcpy(ledger,good_l,sizeof ledger);memcpy(table,good_t,sizeof table);
        if(test==0)e4_p32(table+264,e4_u32(table+264)+1); /* stale generation */
        if(test==1){p64(table+296,2);p64(ledger+328,2);p64(ledger+312,6);}
        if(test==2)p64(table+288,fs->bs+1); /* false logical size */
        if(test==3)e4_p32(table+268,2); /* linked inode claimed as orphan */
        if(test==4){ /* Baseline victim is structurally legal, unsupported here. */
            e4_p16(table+256,21);e4_p16(table+258,2);
            p64(ledger+328,0);p64(ledger+296,7);p64(ledger+336,0);p64(ledger+304,0);
        }
        seal_records();
        assert(!pg_validate_set(marker,sizeof marker,ledger,sizeof ledger,table,sizeof table,&binding,&v));
        write_file(1,ledger,sizeof ledger);write_file(2,table,sizeof table);
        size_t events=d->events;lock=spin_lock_irqsave(&e4_lock);
        memset(&sentinel,0xa5,sizeof sentinel);v=sentinel;
        assert(validate(fs,&v));assert(!memcmp(&v,&sentinel,sizeof v));
        assert(d->events==events);spin_unlock_irqrestore(&e4_lock,lock);negatives++;
    }
    memcpy(ledger,good_l,sizeof ledger);memcpy(table,good_t,sizeof table);
    write_file(1,ledger,sizeof ledger);write_file(2,table,sizeof table);assert(!ext4_sync(fs));
    /* Inject checksum-correct inode corruption directly into the host's
     * volatile image, never into a physical device or production fault hook.
     * The exact valid stable image is restored after each read-only refusal. */
    uint32_t ino=ids[4],group=(ino-1)/fs->ipg;
    uint64_t where=(uint64_t)e4_u32(fs->gd[group].raw+8)*fs->bs+(ino-1)%fs->ipg*256;
    uint8_t original[256],raw[256];memcpy(original,d->volatile_bytes+where,256);
    vfs_node_t *foreign=vfs_lookup("/mnt/target.bin");assert(foreign);
    lock=spin_lock_irqsave(&e4_lock);e4_inode_t foreign_in;
    assert(!e4_inode(fs,((e4_inode_t *)foreign->fs_private)->ino,&foreign_in));
    fs->rw_ino=0;assert(!e4_mapping(&foreign_in));
    uint32_t foreign_block=foreign_in.map->entries[0].physical;
    spin_unlock_irqrestore(&e4_lock,lock);
    for(unsigned test=0;test<7;test++){
        memcpy(raw,original,256);
        if(test==0)e4_p32(raw+100,e4_u32(raw+100)+1);
        if(test==1)e4_p16(raw+2,1);
        if(test==2)e4_p16(raw,0x8000|0644);
        if(test==3)e4_p16(raw+26,2);
        if(test==4)e4_p32(raw+28,e4_u32(raw+28)+fs->bs/512);
        if(test==5)e4_p32(raw+4,e4_u32(raw+4)+1);
        if(test==6)e4_p32(raw+60,foreign_block); /* cross-inode block alias */
        e4_inode_sum(fs,ino,raw);memcpy(d->volatile_bytes+where,raw,256);
        size_t events=d->events;lock=spin_lock_irqsave(&e4_lock);e4_cache_drop(fs,true);fs->rw_ino=0;
        memset(&sentinel,0xa5,sizeof sentinel);v=sentinel;
        assert(validate(fs,&v));assert(!memcmp(&v,&sentinel,sizeof v));
        assert(d->events==events);
        memcpy(d->volatile_bytes+where,original,256);e4_cache_drop(fs,true);fs->rw_ino=0;
        assert(!validate(fs,&v));spin_unlock_irqrestore(&e4_lock,lock);negatives++;
    }
    /* Repair bitmap, descriptor and superblock checksums/counters so this is a
     * dangling extent, rather than merely a bad checksum/free-count fixture. */
    uint32_t block=e4_u32(original+60),bg=(block-fs->first)/fs->bpg,bit=(block-fs->first)%fs->bpg;
    uint8_t original_gd[32];memcpy(original_gd,fs->gd[bg].raw,32);
    uint8_t *gd=fs->gd[bg].raw,*map=d->volatile_bytes+(uint64_t)e4_u32(gd)*fs->bs;
    assert(e4_bit(map,bit));e4_set_bit(map,bit,false);
    e4_p16(gd+12,e4_u16(gd+12)+1);e4_p16(gd+24,(uint16_t)e4_crc(fs->seed,map,fs->bpg/8));
    uint8_t number[4];e4_p32(number,bg);e4_p16(gd+30,0);e4_p16(gd+30,(uint16_t)e4_crc(e4_crc(fs->seed,number,4),gd,32));
    memcpy(d->volatile_bytes+(uint64_t)(fs->first+1)*fs->bs+bg*32,gd,32);
    uint8_t *sb=d->volatile_bytes+1024;e4_p32(sb+12,e4_u32(sb+12)+1);e4_p32(sb+1020,e4_crc(UINT32_MAX,sb,1020));
    size_t events=d->events;lock=spin_lock_irqsave(&e4_lock);e4_cache_drop(fs,true);fs->rw_ino=0;
    memset(&sentinel,0xa5,sizeof sentinel);v=sentinel;assert(validate(fs,&v));
    assert(!memcmp(&v,&sentinel,sizeof v)&&d->events==events);
    memcpy(d->volatile_bytes,d->stable,d->bytes);memcpy(gd,original_gd,32);e4_cache_drop(fs,true);fs->rw_ino=0;
    assert(!validate(fs,&v));spin_unlock_irqrestore(&e4_lock,lock);negatives++;
    for(unsigned test=0;test<2;test++){
        events=d->events;lock=spin_lock_irqsave(&e4_lock);e4_cache_drop(fs,true);fs->rw_ino=0;
        memset(&sentinel,0xa5,sizeof sentinel);v=sentinel;
        if(test)fail_alloc=0;else fail_read=0;
        assert(validate(fs,&v));fail_read=fail_alloc=-1;
        assert(!memcmp(&v,&sentinel,sizeof v)&&d->events==events);
        assert(!validate(fs,&v));spin_unlock_irqrestore(&e4_lock,lock);negatives++;
    }
    for(unsigned test=0;test<2;test++){
        uint64_t where=(uint64_t)(fs->first+fs->bpg+(test?1:0))*fs->bs;
        uint8_t saved=d->volatile_bytes[where];d->volatile_bytes[where]^=1;
        events=d->events;lock=spin_lock_irqsave(&e4_lock);e4_cache_drop(fs,true);
        assert(!eligible(fs)&&d->events==events);
        d->volatile_bytes[where]=saved;e4_cache_drop(fs,true);assert(eligible(fs));
        spin_unlock_irqrestore(&e4_lock,lock);negatives++;
    }
    /* Empty payload makes UINT64_MAX escrow structurally representable. The
     * filesystem headroom check must reject without an overflowing G+floor. */
    assert(!change(fs,true));lock=spin_lock_irqsave(&e4_lock);assert(!validate(fs,&v));
    memcpy(good_l,ledger,sizeof ledger);spin_unlock_irqrestore(&e4_lock,lock);
    p64(ledger+296,UINT64_MAX);p64(ledger+312,UINT64_MAX);seal_records();
    assert(!pg_validate_set(marker,sizeof marker,ledger,sizeof ledger,table,sizeof table,&binding,&v));
    write_file(1,ledger,sizeof ledger);events=d->events;lock=spin_lock_irqsave(&e4_lock);
    memset(&sentinel,0xa5,sizeof sentinel);v=sentinel;assert(validate(fs,&v));
    assert(!memcmp(&v,&sentinel,sizeof v)&&d->events==events);
    spin_unlock_irqrestore(&e4_lock,lock);negatives++;
    memcpy(ledger,good_l,sizeof ledger);write_file(1,ledger,sizeof ledger);assert(!ext4_sync(fs));
    printf("PASS actual-filesystem negative-refinements=%u read-only-refusals\n",negatives);
}
