/* Private boot-selected detached enrolled root. Retained for the complete boot;
 * no unmount, device registration or user-selected storage path exists. */
static uint8_t gk_bytes[32u*1024u*1024u];
static uint8_t gk_policy_bytes[1728];
static uint64_t gk_writes,gk_flushes;
static uint64_t gk_mount_identity=1;
static unsigned gk_admitted,gk_wait,gk_ready,gk_boundary_seen;
static block_dev_t gk_device;
static bool gk_nvme,gk_read_fault;
static bool gk_read(block_dev_t *d,uint64_t l,void *p){
 (void)smp_tlb_service_local();if(l>=d->sector_count)return false;
 if(gk_read_fault && l==2)return false;
 if(gk_nvme)return nvme_read_sector(l,p);
 memcpy(p,gk_bytes+l*d->sector_size,d->sector_size);return true;
}
static bool gk_write(block_dev_t *d,uint64_t l,const void *p){
 (void)smp_tlb_service_local();if(l>=d->sector_count)return false;
 gk_writes++;if(gk_nvme)return nvme_write_sector(l,p);
 memcpy(gk_bytes+l*d->sector_size,p,d->sector_size);return true;
}
static bool gk_flush(block_dev_t *d){(void)d;(void)smp_tlb_service_local();gk_flushes++;return gk_nvme?nvme_flush():true;}
static int e4_namespace_end(ext4_engine_t *e,int r){int result=sv_namespace_core_end(e,r);ad_pair_staged=false;return result;}
static int e4_semantic_commit(ext4_engine_t *e){return ad_enabled?xi_commit(e):-VFS_EROFS;}
static int ad_activate(ext4_engine_t *e,unsigned ino,unsigned parent){(void)e;return xr_typed?xi_activate(ino,parent):-VFS_EROFS;}
static int ad_retire(ext4_engine_t *e,unsigned ino){(void)e;return xr_typed?xi_retire(ino):-VFS_EROFS;}
static bool sv_pin_deny(struct vfs_node *n){
 /* This detached tree has no public name/open admission. Recovery uses only
  * internal retained lookups; all writes still pass the semantic fence. */
 (void)n;return false;
}
static int ad_preview(struct ext4_engine *e){return ad_enabled?ad_validate(e):-VFS_EROFS;}
static bool gk_released(void *unused){(void)unused;return !__atomic_load_n(&gk_wait,__ATOMIC_ACQUIRE);}
static bool gk_boundary(void){
 spin_debug_assert_unheld();
 unsigned wait=__atomic_load_n(&gk_wait,__ATOMIC_ACQUIRE);
 if(wait && (wait==1 || xi_stage>=3)){
  if(!gk_boundary_seen){gk_boundary_seen=1;__atomic_add_fetch(&gk_ready,1,__ATOMIC_RELEASE);}
  /* Enroll with IRQs still excluded. A signal wakes this exact continuation;
   * the scheduler's interruptible wait handles STOP/CONT and caught signals.
   * Yield-polling here can starve a sleeping controller's timer on SMP=1. */
  sched_wait_until(&gk_wait,gk_released,NULL);
 }
 return process_signal_interrupt();
}
static void gk_load(const char *path,void *bytes,size_t count){
 creds_t root;creds_init_root(&root);int error;file_t *f=vfs_open_creds(path,VFS_O_RDONLY,&root,&error);
 assert(f && f->node->size==count);
 for(size_t at=0;at<count;){size_t n=count-at;if(n>32768)n=32768;assert(vfs_read(f,(uint8_t *)bytes+at,n)==(int64_t)n);at+=n;}
 assert(!vfs_close(f));
}
void gk_fixture_boot(bool nvme_fixture,bool read_fault){
 assert(!sv_fs&&!ad_enabled);gk_load("/gate1/root.img",gk_bytes,sizeof gk_bytes);
 gk_load("/gate1/policy.bin",gk_policy_bytes,sizeof gk_policy_bytes);
 /* The fixture selector and its test authority come from the pinned immutable
  * boot module, separately from mutable filesystem records. No disk keys. */
 gk_load("/gate1/enrollment.bin",ad_descriptor,PE_BODY+192);
 ng_policy_load(gk_policy_bytes+384);xr_policy_load(gk_policy_bytes,sizeof gk_policy_bytes);
 assert(!pe_verify(ad_descriptor,PE_BODY+192,&xr_en_policy,&ad_en));ad_enabled=true;
 /* Only the private runner's QEMU fw_cfg selector admits raw NVMe. With no
  * selector (including a physical boot), this build uses RAM and never opens
  * a hardware data device. The runner also validates its final disk argv. */
 if(nvme_fixture){
  assert(nvme_init()&&nvme_has_unique_namespace()&&nvme_get_sector_size()==512&&nvme_get_sector_count()==sizeof gk_bytes/512);
  gk_nvme=true;
 }
 gk_read_fault=read_fault;
 gk_device=(block_dev_t){.sector_size=512,.sector_count=sizeof gk_bytes/512,.read_sector=gk_read,.write_sector=gk_write,.flush=gk_flush};
 const char *stage=NULL;int r=e4_mount_journal_admitted(&gk_device,"/",true,&sv_fs,&stage,true);
 if(r){serial_raw_puts("GATE1 MOUNT REFUSE ");serial_raw_puts(stage);serial_raw_print_dec((uint64_t)-r);assert(!gk_writes&&!gk_flushes);serial_puts("\nGATE1 ROOT REFUSED writes=0 flushes=0\n");return;}
 assert(!r&&sv_fs&&sv_fs->detached&&sv_fs->trusted_root);
 __atomic_store_n(&gk_admitted,1,__ATOMIC_RELEASE);serial_puts("GATE1 ENROLLED DETACHED ROOT ADMITTED\n");
}
static void gk_capture(pkg_transaction_status *s){
 *s=(pkg_transaction_status){.phase=e4_u16(xi_current.g+22),.sequence=ad_u64(xi_current.g+24),.account_sequence=ad_u64(xi_current.a+24),.credential_generation=ad_u64(xi_current.a+144)};
 for(unsigned i=0;i<2;i++){s->remaining[i]=ad_u64(xi_current.g+312+8*i);s->charged[i]=ad_u64(xi_current.g+328+8*i);}
}
static int gk_status(void *context,pkg_transaction_status *out){
 if(context!=&gk_mount_identity||!__atomic_load_n(&gk_admitted,__ATOMIC_ACQUIRE))return -VFS_EROFS;
 uint64_t irq=spin_lock_irqsave(&e4_lock),writes=gk_writes,flushes=gk_flushes;
 int r=sv_fs->engine->tainted?-VFS_EIO:ad_validate(sv_fs->engine);if(!r)gk_capture(out);assert(writes==gk_writes&&flushes==gk_flushes);
 spin_unlock_irqrestore(&e4_lock,irq);return r;
}
static int gk_recover(void *context,pkg_transaction_status *out){
 if(context!=&gk_mount_identity||!__atomic_load_n(&gk_admitted,__ATOMIC_ACQUIRE))return -VFS_EROFS;
 gk_boundary_seen=0;int r=xi_finish(sv_fs->engine);
 __atomic_store_n(&gk_wait,0,__ATOMIC_RELEASE);
 if(!r)r=gk_status(context,out);
 return r;
}
const pkg_transaction_backend *gk_backend(void){
 static const pkg_transaction_backend b={&gk_mount_identity,gk_status,gk_recover};return &b;
}
bool gk_root_admitted(void){return __atomic_load_n(&gk_admitted,__ATOMIC_ACQUIRE)!=0;}
int64_t gk_storage_test(uint64_t op){
 if(op==10||op==11){__atomic_store_n(&gk_wait,op==10?1:2,__ATOMIC_RELEASE);return 0;}
 if(op==12)return __atomic_load_n(&gk_ready,__ATOMIC_ACQUIRE);
 if(op==13)return (int64_t)gk_writes;
 if(op==14)return xi_commits;
 if(op==16){uint64_t irq=spin_lock_irqsave(&e4_lock);sv_fs->engine->tainted=true;spin_unlock_irqrestore(&e4_lock,irq);return 0;}
 if(op==15){
  assert(xi_stage==11);assert(!ext4_freeze_and_sync(sv_fs));
  serial_puts("GATE1 SNAPSHOT address=");serial_print_hex((uintptr_t)gk_bytes);serial_puts(" bytes=");serial_print_dec(sizeof gk_bytes);
  serial_puts(" commits=");serial_print_dec(xi_commits);serial_puts(" writes=");serial_print_dec(gk_writes);serial_puts(" flushes=");serial_print_dec(gk_flushes);serial_puts("\n");return 0;
 }
 return SYSCALL_EINVAL;
}
