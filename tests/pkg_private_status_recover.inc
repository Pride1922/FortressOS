/* Private actual EXT4 adapter. Configuration and transport remain in harness. */
static void qt_capture(void){
 qt_snapshot=(pkg_transaction_status){.phase=e4_u16(xi_current.g+22),.sequence=ad_u64(xi_current.g+24),
  .account_sequence=ad_u64(xi_current.a+24),.credential_generation=ad_u64(xi_current.a+144)};
 for(unsigned i=0;i<2;i++){qt_snapshot.remaining[i]=ad_u64(xi_current.g+312+8*i);qt_snapshot.charged[i]=ad_u64(xi_current.g+328+8*i);}
}
static int qt_status(void *context,pkg_transaction_status *out){
 assert(context==qt_device);qt_status_calls++;size_t events=ad_disk->events;int r;
 if(sv_fs){uint64_t irq=spin_lock_irqsave(&e4_lock);r=ad_validate(sv_fs->engine);if(!r)qt_capture();spin_unlock_irqrestore(&e4_lock,irq);}
 else{
  qt_observe=true;qt_observed=false;const char *where=NULL;r=ext4_prepare_root_diagnose(qt_device,true,&sv_fs,&where);qt_observe=false;
  if(r==-VFS_EAGAIN&&qt_observed)r=0;
  assert(!sv_fs&&!e4_engine_busy);
 }
 assert(ad_disk->events==events);
 if(!r)*out=qt_snapshot;
 return r;
}
static int qt_recover(void *context,pkg_transaction_status *out){
 assert(context==qt_device);qt_recover_calls++;int r=0;
 if(!sv_fs){const char *where=NULL;r=ext4_prepare_root_diagnose(qt_device,true,&sv_fs,&where);}
 if(!r)r=xi_finish(sv_fs->engine);
 if(!r)r=accounts_prepare(sv_fs);
 if(!r){uint64_t irq=spin_lock_irqsave(&e4_lock);r=ad_validate(sv_fs->engine);if(!r)qt_capture();spin_unlock_irqrestore(&e4_lock,irq);}
 qt_backend_error=r;if(!r)*out=qt_snapshot;return r;
}
