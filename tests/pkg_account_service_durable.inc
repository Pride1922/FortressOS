/* Actual account service + redo; private host-only paired-record adapter.
 * Root-only profile, no production caller or trust provisioning. */
static account_io_t sv_backend;
static bool sv_readers=true,sv_pending_move,sv_cancelled,sv_armed;
static unsigned sv_cut,sv_commits,sv_cancel_events;
static uint32_t sv_roles[ACCOUNT_ROLE_COUNT];
static char sv_paths[ACCOUNT_ROLE_COUNT][96];
static char sv_new[3][8193];
static const char *sv_old[]={"root:x:0:0:root:/root:/bin/shell\n","root:x:0:\n","root:!:::::::\n"};
static const char *sv_changed[]={"root:x:0:0:root2:/root:/bin/shell\n","root:x:0:root\n","root:*:::::::\n"};
static const char *sv_path(unsigned role){
    assert(role<ACCOUNT_ROLE_COUNT);
    if(!sv_paths[role][0])snprintf(sv_paths[role],sizeof sv_paths[role],"/mnt%s",e4_ac_paths[role]);
    return sv_paths[role];
}
static void sv_profile(void){
    for(unsigned i=0;i<3;i++){
        size_t n=strlen(sv_changed[i]);memcpy(sv_new[i],sv_changed[i],n);
        while(n<8192){size_t z=8192-n;if(z>513)z=513;memset(sv_new[i]+n,'p',z);if(z>1)sv_new[i][n]='#';sv_new[i][n+z-1]='\n';n+=z;}
    }
}
static int sv_raw_read(ext4_mount_t *fs,unsigned role,void *bytes,size_t *size){
    uint32_t ino;int r=ad_path(fs,sv_path(role),&ino);if(r)return r;
    e4_inode_t in;if(e4_inode(fs,ino,&in)||in.uid||in.gid||in.full_mode!=(VFS_S_IFREG|e4_ac_mode(role))||in.size>8192)return -VFS_EIO;
    uint8_t links[2];uint32_t g=(ino-1)/fs->ipg,b=(ino-1)%fs->ipg;
    if(!e4_bytes(fs,(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+b*256+26,links,2)||e4_u16(links)!=1)return -VFS_EIO;
    sv_roles[role]=ino;fs->rw_ino=0;if(e4_mapping(&in))return -VFS_EIO;
    size_t done=0;
    for(unsigned i=0;i<in.map->count;i++){
        e4_extent_t x=in.map->entries[i];if(x.unwritten||x.logical!=(done/fs->bs))return -VFS_EIO;
        size_t take=(size_t)x.len*fs->bs;if(take>in.size-done)take=in.size-done;
        if(!e4_bytes(fs,(uint64_t)x.physical*fs->bs,(uint8_t *)bytes+done,take))return -VFS_EIO;
        done+=take;
    }
    if(done!=in.size)return -VFS_EIO;
    *size=done;return 0;
}
static bool sv_intent(const uint8_t *p){
    uint8_t h[32];ad_hash(p,128,h);
    if(memcmp(p,"FOSACCT1",8)||memcmp(h,p+128,32)||!ad_u64(p+8))return false;
    for(unsigned i=28;i<32;i++)if(p[i])return false;
    for(unsigned i=0;i<3;i++)if(!e4_u32(p+16+4*i)||e4_u32(p+16+4*i)>8192)return false;
    return true;
}
static int sv_state_extra(void){
    uint64_t auth=ad_u64(ad_state+144),target=ad_u64(ad_state+152);unsigned phase=e4_u16(ad_state+160);
    if(!auth||auth==UINT64_MAX||phase>2||(target&&(target!=auth||target<2))||(phase&&!target))return -VFS_EIO;
    for(unsigned i=162;i<168;i++)if(ad_state[i])return -VFS_EIO;
    for(unsigned i=436;i<992;i++)if(ad_state[i])return -VFS_EIO;
    if(target){if(!sv_intent(ad_state+168)||ad_u64(ad_state+176)!=target)return -VFS_EIO;}
    else for(unsigned i=168;i<328;i++)if(ad_state[i])return -VFS_EIO;
    for(unsigned i=0;i<3;i++)if(!e4_u32(ad_state+424+4*i)||e4_u32(ad_state+424+4*i)>8192)return -VFS_EIO;
    if(!e4_u16(ad_state+22)&&(phase||target))return -VFS_EIO;
    return 0;
}
static void sv_old_hashes(ext4_mount_t *fs){
    static uint8_t b[8192];size_t n;
    for(unsigned i=0;i<3;i++){assert(!sv_raw_read(fs,i,b,&n));ad_hash(b,n,ad_state+328+32*i);e4_p32(ad_state+424+4*i,(uint32_t)n);}
}
static int sv_db(ext4_mount_t *fs,account_workspace_t *w){
    static userdb_t db;memset(&db,0,sizeof db);
    bool ok=db_passwd(&db,w->files[0],w->lengths[0])&&db_group(&db,w->files[1],w->lengths[1])&&db_shadow(&db,w->files[2],w->lengths[2])&&db_validate(&db);
    const db_user_t *u=ok?db_user_id(&db,0):NULL;const db_shadow_t *s=u?db_shadow_name(&db,u->name):NULL;
    uint32_t ino;e4_inode_t home;
    ok=ok&&db.nusers==1&&db.ngroups==1&&db.nshadows==1&&u&&!strcmp(u->name,"root")&&!u->gid&&
        !strcmp(u->home,"/root")&&!strcmp(u->shell,"/bin/shell")&&s&&(s->hash[0]=='!'||s->hash[0]=='*')&&
        !ad_path(fs,"/mnt/root",&ino)&&!e4_inode(fs,ino,&home)&&home.full_mode==(VFS_S_IFDIR|0700)&&!home.uid&&!home.gid;
    db_wipe(&db,sizeof db);return ok?0:-VFS_EIO;
}
static bool sv_match(const void *p,size_t n,const uint8_t *h,uint32_t expected){
    uint8_t digest[32];ad_hash(p,n,digest);return n==expected&&!memcmp(digest,h,32);
}
static int sv_semantics(ext4_engine_t *e){
    static uint8_t bytes[ACCOUNT_ROLE_COUNT][8192];static size_t sizes[ACCOUNT_ROLE_COUNT];
    static bool present[ACCOUNT_ROLE_COUNT];memset(present,0,sizeof present);memset(sv_roles,0,sizeof sv_roles);
    for(unsigned i=0;i<ACCOUNT_ROLE_COUNT;i++){
        int r=sv_raw_read(e->fs,i,bytes[i],&sizes[i]);if(r&&r!=-VFS_ENOENT)return r;present[i]=!r;
        for(unsigned j=0;j<i;j++)if(present[i]&&present[j]&&sv_roles[i]==sv_roles[j])return -VFS_EIO;
    }
    unsigned phase=e4_u16(ad_state+160);uint64_t target=ad_u64(ad_state+152);
    bool open=e4_u16(ad_state+22)!=0;
    if((phase==1)!=present[ACCOUNT_PENDING])return -VFS_EIO;
    if(present[ACCOUNT_PENDING]&&(sizes[10]!=160||memcmp(bytes[10],ad_state+168,160)))return -VFS_EIO;
    if(!open)for(unsigned i=3;i<11;i++)if(present[i])return -VFS_EIO;
    account_workspace_t *w=kcalloc(1,sizeof(*w));if(!w)return -VFS_ENOMEM;
    int result=0;
    for(unsigned i=0;i<3;i++){
        const uint8_t *nh=ad_state+200+32*i;uint32_t nn=e4_u32(ad_state+184+4*i);
        const uint8_t *oh=ad_state+328+32*i;uint32_t on=e4_u32(ad_state+424+4*i);
        bool old=present[i]&&sv_match(bytes[i],sizes[i],oh,on);
        bool newer=target&&present[i]&&sv_match(bytes[i],sizes[i],nh,nn);
        /* During replay, remove(TEMP) may commit while canonical is absent.
         * The independently bound complete stage triple below authorizes redo;
         * requiring a linked TEMP here would strand that valid restart. */
        if(phase==0?!old:phase==2?!newer:present[i]&&!old&&!newer){result=-VFS_EIO;break;}
        for(unsigned role=3+i;role<=6+i;role+=3)if(present[role]){
            if(!target||(sizes[role]&&!sv_match(bytes[role],sizes[role],nh,nn))||(phase==2&&role>=6)){result=-VFS_EIO;break;}
        }
        if(result)break;
        unsigned source=phase==1?3+i:i;
        if(!present[source]||(phase==1&&!sv_match(bytes[source],sizes[source],nh,nn))){result=-VFS_EIO;break;}
        memcpy(w->files[i],bytes[source],sizes[source]);w->lengths[i]=sizes[source];
    }
    if(!result&&present[9]&&(!target||(sizes[9]&&(sizes[9]!=160||memcmp(bytes[9],ad_state+168,160)))))result=-VFS_EIO;
    if(!result)result=sv_db(e->fs,w);
    account_redo_wipe(w);kfree(w);return result;
}
static int ad_validate(struct ext4_engine *e){
    if(!ad_enabled)return 0;
    int r=sv_locators(e);if(r)return r;pe_origin o;r=ad_pair(e,false,&o);if(r)return r;
    if(ad_u64(ad_state+24)>UINT64_MAX-256)return -VFS_EFBIG;
    static uint8_t marker[512];uint8_t h[32];
    if(!e4_bytes(e->fs,ad_u64(ad_locator(0)+48)*e->fs->bs,marker,512))return -VFS_EIO;
    ad_hash(marker,512,h);if(memcmp(h,ad_en.wire+120,32))return -VFS_EIO;
    r=sv_semantics(e);if(r)return r;
    if(!o.active)return e->orphan_count?-VFS_EIO:0;
    if(sv_roles[o.role]||e->orphan_count!=1||e->orphans[0]!=o.inode)return -VFS_EIO;
    ext4_mount_t *fs=e->fs;e4_inode_t in;fs->orphan_access=o.inode;r=e4_inode(fs,o.inode,&in);fs->orphan_access=0;
    if(r||in.generation!=o.generation||in.size!=o.size||in.uid||in.gid||in.full_mode!=(VFS_S_IFREG|e4_ac_mode(o.role)))return -VFS_EIO;
    uint8_t raw[256];uint32_t g=(o.inode-1)/fs->ipg,b=(o.inode-1)%fs->ipg;
    if(!e4_bytes(fs,(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+b*256,raw,256)||e4_u16(raw+26))return -VFS_EIO;
    fs->rw_ino=0;if(e4_mapping(&in)||in.map->count>o.extents)return -VFS_EIO;
    uint64_t logical=0,total=0;
    for(unsigned i=0;i<in.map->count;i++){
        e4_extent_t x=in.map->entries[i];const uint8_t *p=ad_slot+160+16*i;
        if(x.unwritten||x.logical!=logical||x.logical!=e4_u32(p)||x.physical!=ad_u64(p+8)||x.len>e4_u32(p+4))return -VFS_EIO;
        logical+=x.len;
    }
    for(unsigned i=0;i<fs->physical_count;i++){
        e4_range_t x=fs->physical[i];unsigned found=0;
        for(unsigned j=0;j<o.ranges;j++){const uint8_t *p=ad_slot+416+16*j;if(x.lo>=ad_u64(p)&&(uint64_t)x.lo+x.len<=ad_u64(p)+e4_u32(p+8))found++;}
        if(found!=1)return -VFS_EIO;
        total+=x.len;
    }
    return total*(fs->bs/512)==e4_u32(raw+28)?0:-VFS_EIO;
}
static int e4_namespace_end(ext4_engine_t *e,int r){int result=sv_namespace_core_end(e,r);ad_pair_staged=false;return result;}
static int e4_semantic_commit(struct ext4_engine *e){
    if(sv_cancelled)return -VFS_EINTR;
    if(ad_enabled&&sv_pending_move){
        pe_origin o;int r=ad_pair(e,false,&o);if(r)return r;
        if(o.active||!ad_u64(ad_state+152)||e4_u16(ad_state+160))return -VFS_EIO;
        e4_p16(ad_state+160,1);r=ad_store(e);if(r)return r;
    }
    int r=sv_primitive_commit(e);
    if(!r&&sv_armed&&++sv_commits==sv_cut){sv_cancelled=true;sv_cancel_events=(unsigned)ad_disk->events;}
    return r;
}
static int sv_remove(void *p,unsigned role){
    if(sv_cancelled)return -VFS_EINTR;
    assert(sv_role==ACCOUNT_ROLE_COUNT);sv_role=role;
    int r=sv_backend.remove(p,role);sv_role=ACCOUNT_ROLE_COUNT;return r;
}
static int sv_write(void *p,unsigned role,const void *bytes,size_t n){return sv_cancelled?-VFS_EINTR:sv_backend.write(p,role,bytes,n);}
static int sv_rename(void *p,unsigned from,unsigned to){
    if(sv_cancelled)return -VFS_EINTR;
    sv_pending_move=from==ACCOUNT_INTENT_TEMP&&to==ACCOUNT_PENDING;
    int r=sv_backend.rename(p,from,to);sv_pending_move=false;return r;
}
static int sv_sync(void *p){return sv_cancelled?-VFS_EINTR:sv_backend.sync(p);}
static bool sv_pin_deny(struct vfs_node *node){
    if(!sv_readers||!node||!node->parent||node->parent->permission_actor!=e4_permission_actor||!node->fs_private||!node->parent->fs_private)return false;
    e4_node_t *n=(void *)node,*parent=(void *)node->parent;
    if(n->inode.fs!=sv_fs)return false;
    uint32_t ino=parent->inode.ino;
    for(unsigned i=0;i<11;i++){
        bool priv=(i>=3&&i<=5)||i>=9;const char *name=e4_ac_paths[i];for(const char *p=name;*p;p++)if(*p=='/')name=p+1;
        if(ino==e4_u32(ad_locator(priv?13:9)+4)&&!strcmp(name,node->name))return true;
    }
    return false;
}
static void sv_relax(void){sv_readers=!enabled||writer||sv_cancelled;}
static int sv_begin(void *p){
    ext4_mount_t *fs=p;uint64_t irq=spin_lock_irqsave(&e4_lock);sv_readers=true;
    int r=0;for(unsigned i=0;i<fs->nodes;i++)if(fs->cached[i]->opens&&sv_pin_deny(&fs->cached[i]->node)){r=-VFS_EAGAIN;break;}
    if(!r)r=ad_operation(fs->engine,true);
    spin_unlock_irqrestore(&e4_lock,irq);return r;
}
static int sv_issue_pre(void *p,uint64_t gen){
    ext4_mount_t *fs=p;pe_origin o;uint64_t irq=spin_lock_irqsave(&e4_lock);int r=ad_pair(fs->engine,false,&o);
    if(!r&&(gen>=UINT64_MAX-1||ad_u64(ad_state+144)!=gen||ad_u64(ad_state+24)>UINT64_MAX-128))r=-VFS_EFBIG;
    spin_unlock_irqrestore(&e4_lock,irq);return r;
}
static int sv_issue(void *p,account_workspace_t *w,uint64_t gen){
    ext4_mount_t *fs=p;pe_origin o;uint64_t irq=spin_lock_irqsave(&e4_lock);int r=ad_pair(fs->engine,false,&o);
    if(!r&&(o.active||!e4_u16(ad_state+22)||ad_u64(ad_state+152)||ad_u64(ad_state+144)+1!=gen))r=-VFS_EIO;
    if(!r)r=e4_namespace_begin(fs->engine);
    if(!r){
        sv_old_hashes(fs);ad_p64(ad_state+144,gen);ad_p64(ad_state+152,gen);
        uint8_t *intent=ad_state+168;memset(intent,0,160);memcpy(intent,"FOSACCT1",8);ad_p64(intent+8,gen);
        for(unsigned i=0;i<3;i++){e4_p32(intent+16+4*i,(uint32_t)w->lengths[i]);ad_hash(w->files[i],w->lengths[i],intent+32+32*i);}
        ad_hash(intent,128,intent+128);r=ad_store(fs->engine);r=e4_namespace_end(fs->engine,r);
    }
    spin_unlock_irqrestore(&e4_lock,irq);return !r&&sv_cancelled?-VFS_EINTR:r;
}
static int sv_complete(void *p){
    ext4_mount_t *fs=p;pe_origin o;uint64_t irq=spin_lock_irqsave(&e4_lock);int r=ad_pair(fs->engine,false,&o);
    if(!r&&(o.active||fs->engine->orphan_count))r=-VFS_EIO;
    static uint8_t b[8192];size_t n;
    for(unsigned i=3;!r&&i<11;i++)if(sv_raw_read(fs,i,b,&n)!=-VFS_ENOENT)r=-VFS_EIO;
    if(!r&&ad_u64(ad_state+152)){
        r=e4_namespace_begin(fs->engine);
        if(!r){sv_old_hashes(fs);memset(ad_state+152,0,176);r=ad_store(fs->engine);r=e4_namespace_end(fs->engine,r);}
    }
    spin_unlock_irqrestore(&e4_lock,irq);return !r&&sv_cancelled?-VFS_EINTR:r;
}
static int sv_end(void *p){
    int r=sv_complete(p);if(!r)r=ad_op(p,false);
    return !r&&sv_cancelled?-VFS_EINTR:r;
}
static int sv_boot_bind(void *p,account_io_t *io){
    ext4_mount_t *fs=p;pe_origin o;uint64_t irq=spin_lock_irqsave(&e4_lock);int r=ad_pair(fs->engine,false,&o);
    if(!r)generation=ad_u64(ad_state+144);
    spin_unlock_irqrestore(&e4_lock,irq);if(r)return r;
    sv_backend=*io;io->remove=sv_remove;io->write=sv_write;io->rename=sv_rename;io->sync=sv_sync;sv_readers=true;return 0;
}
static int sv_boot_end(void *p){
    int r=sv_complete(p);if(r)return r;ext4_mount_t *fs=p;
    if(e4_u16(ad_state+22))r=ad_op(fs,false);
    return !r&&sv_cancelled?-VFS_EINTR:r;
}
static void sv_slots(ext4_mount_t *fs,unsigned which){
    uint32_t ino,parent;assert(!ad_path(fs,which?"/mnt/var/lib/fortress/accounts":"/mnt/etc",&ino));
    assert(!ad_path(fs,which?"/mnt/var/lib/fortress":"/mnt",&parent));
    uint64_t irq=spin_lock_irqsave(&e4_lock);e4_inode_t in;assert(!e4_inode(fs,ino,&in));uint32_t block;assert(!e4_dir_block(&in,0,&block));
    assert(!e4_namespace_begin(fs->engine));uint8_t *b=e4_stage(fs->engine,block,E4_REFERENCE);assert(b);memset(b,0,fs->bs);
    e4_dir_entry(b,ino,12,".",2);e4_dir_entry(b+12,parent,12,"..",2);unsigned at=24;
    for(unsigned i=0;i<(which?5u:6u);i++,at+=24){
        uint32_t id=0;const char *name="";if(!which&&i<3){assert(!ad_path(fs,sv_path(i),&id));name=i==0?"passwd":i==1?"group":"shadow";}
        e4_dir_entry(b+at,id,24,name,id?1:0);
    }
    while(at<fs->bs-12){unsigned take=(fs->bs-12-at)%12?16:12;e4_dir_entry(b+at,0,take,"",0);at+=take;}
    assert(at==fs->bs-12);e4_dir_sum(&in,b);assert(!e4_namespace_end(fs->engine,0));spin_unlock_irqrestore(&e4_lock,irq);
}
static void sv_seed(ext4_mount_t *fs){
    assert(!vfs_mkdir("/mnt/root",0700));
    for(unsigned i=0;i<3;i++){
        file_t *f=vfs_open(sv_path(i),VFS_O_CREAT|VFS_O_RDWR|VFS_O_TRUNC);assert(f);size_t n=strlen(sv_old[i]);
        assert(vfs_write(f,sv_old[i],n)==(int64_t)n);assert(!vfs_close(f));creds_t root;creds_init_root(&root);
        assert(!vfs_chmod_creds(sv_path(i),e4_ac_mode(i),&root));
    }
    sv_slots(fs,0);sv_slots(fs,1);
}
static void sv_seed_state(ext4_mount_t *fs){ad_p64(ad_state+144,1);sv_old_hashes(fs);}
static void teardown(void){
    if(sv_fs){accounts_abandon();assert(!ext4_discard_prepared_root(sv_fs));sv_fs=NULL;}
    sv_previous_teardown();storage=(account_io_t){0};namespace=NULL;enabled=false;busy=0;generation=1;writer=token=next_token=0;
    writer_dead=latched=false;sv_readers=true;sv_cancelled=sv_armed=sv_pending_move=false;sv_role=ACCOUNT_ROLE_COUNT;ad_pair_staged=false;
}
static void sv_mount(block_dev_t *dev){
    const char *stage=NULL;int r=ext4_prepare_root_diagnose(dev,true,&sv_fs,&stage);
    if(r)fprintf(stderr,"SERVICE constructor stage=%s error=%d\n",stage,r);
    assert(!r&&sv_fs&&sv_fs->detached&&!e4_active);
    ext4_account_namespace_report_t report;assert(!ext4_root_account_namespace_admit(sv_fs,&report));
}
static void sv_report(const char *label){
    pe_origin o;uint64_t irq=spin_lock_irqsave(&e4_lock);assert(!ad_pair(sv_fs->engine,false,&o));spin_unlock_irqrestore(&e4_lock,irq);
    printf("%s commits=%u active=%u role=%u phase=%u target=%llu auth=%llu operation=%llu sequence=%llu\n",label,sv_commits,o.active,o.active?o.role:11,e4_u16(ad_state+160),
        (unsigned long long)ad_u64(ad_state+152),(unsigned long long)ad_u64(ad_state+144),
        (unsigned long long)ad_u64(ad_state+32),(unsigned long long)ad_u64(ad_state+24));
}
static unsigned sv_family(void){
    static uint8_t b[8192];size_t n;bool old=true,newer=true;
    for(unsigned i=0;i<3;i++){assert(!sv_backend.read(sv_fs,i,b,sizeof b,&n));old&=n==strlen(sv_old[i])&&!memcmp(b,sv_old[i],n);newer&=n==8192&&!memcmp(b,sv_new[i],n);}
    assert(old!=newer);return newer;
}
static void sv_noauth(void){
    assert(!enabled&&sv_cancelled&&ad_disk->events==sv_cancel_events);
    size_t events=ad_disk->events;accounts_process_exit(77);assert(!enabled&&!writer&&ad_disk->events==events);
    creds_t root;creds_init_root(&root);account_request_t q={.size=64,.version=1,.generation=generation};
    assert(accounts_control(ACCOUNT_CHECK,(uintptr_t)&q,sizeof q,&root,88)==SYSCALL_EAGAIN);
    q.generation=0;assert(accounts_control(ACCOUNT_BEGIN,(uintptr_t)&q,sizeof q,&root,88)==SYSCALL_EAGAIN&&ad_disk->events==events);
}
int main(int argc,char **argv){
    assert(argc==5||argc==7);setvbuf(stdout,NULL,_IONBF,0);sv_profile();
    if(!strcmp(argv[1],"setup"))return sv_primitive_previous_main(argc,argv);
    assert(argc==7);const char *prefix=argv[6];size_t size;uint8_t *initial=load(argv[2],&size);unsigned ss=(unsigned)strtoul(argv[3],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);ad_disk=&d;
    block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);ad_auth(argv[4],argv[5]);
    bool reject=!strcmp(argv[1],"reject");
    if(reject){
        const char *stage=NULL;int r=ext4_prepare_root_diagnose(&dev,true,&sv_fs,&stage);
        if(!r)fprintf(stderr,"UNEXPECTED acceptance of rejected fixture\n");
        assert(r&&!sv_fs&&!e4_engine_busy&&!d.events&&!memcmp(initial,d.stable,size));
        assert(!strcmp(stage,"account-provenance-preview"));puts("PASS reject writes=0 flushes=0");
    }else{
        sv_mount(&dev);const char *eq=NULL;for(const char *p=argv[1];*p;p++)if(*p=='=')eq=p;
        bool recover=!strncmp(argv[1],"recover",7),cancel=eq!=NULL;
        sv_cut=cancel?(unsigned)strtoul(eq+1,NULL,10):UINT32_MAX;sv_commits=0;
        sv_armed=recover;int r=accounts_prepare(sv_fs);sv_armed=false;
        if(r){assert(recover&&cancel&&r==-VFS_EINTR&&sv_commits==sv_cut);sv_noauth();sv_report("PASS interrupt-recovery");save(prefix,"interrupted",d.stable,size);}
        else if(!recover){
            assert(enabled&&!writer&&generation==ad_u64(ad_state+144));creds_t root;creds_init_root(&root);
            account_request_t req={.size=64,.version=1};
            size_t events=d.events;creds_t ordinary=root;ordinary.euid=1000;
            assert(accounts_control(ACCOUNT_BEGIN,(uintptr_t)&req,sizeof req,&ordinary,77)==SYSCALL_EPERM&&d.events==events);
            assert(!accounts_control(ACCOUNT_BEGIN,(uintptr_t)&req,sizeof req,&root,77));
            account_request_t competing={.size=64,.version=1};events=d.events;
            assert(accounts_control(ACCOUNT_BEGIN,(uintptr_t)&competing,sizeof competing,&root,88)==SYSCALL_EAGAIN&&d.events==events);
            int err;vfs_node_t *node=e4_ac_lookup(sv_fs,"/etc/passwd",&err);assert(node&&e4_open(node)==-VFS_EAGAIN);vfs_node_put(node);
            req.passwd=(uintptr_t)sv_new[0];req.group=(uintptr_t)sv_new[1];req.shadow=(uintptr_t)sv_new[2];req.passwd_bytes=req.group_bytes=req.shadow_bytes=8192;
            sv_commits=0;sv_armed=true;int64_t result=accounts_control(ACCOUNT_UPDATE,(uintptr_t)&req,sizeof req,&root,77);sv_armed=false;
            if(cancel){assert(result==SYSCALL_EINTR&&sv_commits==sv_cut);sv_noauth();sv_report("PASS interrupt-update");save(prefix,"interrupted",d.stable,size);}
            else{
                assert(!result&&enabled&&writer==77);sv_report("PASS update");
                memset(&req.passwd,0,40);req.flags=0;assert(!accounts_control(ACCOUNT_FINISH,(uintptr_t)&req,sizeof req,&root,77));
                assert(enabled&&!writer&&generation==2);printf("PASS family=%u auth=%llu\n",sv_family(),(unsigned long long)generation);
                assert(!ext4_freeze_and_sync(sv_fs));save(prefix,"final",d.stable,size);
            }
        }else{
            assert(enabled&&!writer&&generation==ad_u64(ad_state+144));sv_report("PASS recovery");
            creds_t root;creds_init_root(&root);account_request_t check={.size=64,.version=1,.generation=generation};
            assert(!accounts_control(ACCOUNT_CHECK,(uintptr_t)&check,sizeof check,&root,77));
            if(generation>1){check.generation=1;assert(accounts_control(ACCOUNT_CHECK,(uintptr_t)&check,sizeof check,&root,77)==SYSCALL_EAGAIN);}
            printf("PASS family=%u auth=%llu\n",sv_family(),(unsigned long long)generation);
            assert(!ext4_freeze_and_sync(sv_fs));save(prefix,"final",d.stable,size);
        }
    }
    ad_enabled=false;teardown();free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}
