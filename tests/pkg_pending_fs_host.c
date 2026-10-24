/* Generated native readers are appended by the copied-source harness. */
#define PKG_GEOMETRY_MAIN retained_geometry_main
#include "pkg_enroll_geometry_host.c"
#include "crypto.h"
#include "pkg_pending_candidate.h"
static uint8_t jb_bundle[32768],jb_witness[32+20*128],jb_selector[1536];
static size_t jb_bundle_n,jb_selector_n;
static ps_policy jb_policy;
#include "pkg_pending_export_generated.inc"
#include "pkg_current_fs.inc"
#include "pkg_pending_fs.inc"
long tool_syscall(long n,uintptr_t a,uintptr_t b,uintptr_t c){(void)n;(void)a;(void)b;(void)c;assert(!"unexpected codec I/O");return -1;}
static int native_validate(ext4_engine_t *e){
    if(e->tainted||e->orphan_count||e->orphan_recovery_required)return -VFS_EINVAL;
    int r=identities(e->fs,jb_witness,sizeof jb_witness);
    if(!r)r=pn_export_bundle(e->fs,"",jb_witness);
    if(!r)r=pn_load(e->fs);
    return r;
}
static int native_open(block_dev_t *disk,ext4_engine_t **out){
    *out=NULL;gpt_policy_result_t gp;if(!gpt_parse_ex(disk,&gp)||gp!=GPT_POLICY_PRIMARY_CONSISTENT)return -VFS_EINVAL;
    block_dev_t *dev=&gpt_get_partition(0)->block_dev;
    ext4_mount_t *fs=kcalloc(1,sizeof *fs);e4_journal_guard_t *guard=kcalloc(1,sizeof *guard);assert(fs&&guard);
    guard->lifecycle=true;fs->dev=dev;fs->bs=512;fs->blocks=65536;fs->journal_bootstrap=true;
    uint32_t *map=NULL;jbd2_source_t source;jbd2_plan_t *plan=NULL;ext4_engine_t *preview=NULL;
    int r=e4_journal_source(fs,guard,&map,&source);if(!r)r=jbd2_analyze(dev,&source,&plan,NULL);
    kfree(map);e4_discard(fs);
    block_dev_t projected={.sector_size=512,.sector_count=65536,.read_sector=e4_preview_sector,.write_sector=e4_preview_write,.flush=e4_preview_flush,.priv=plan};
    if(!r)r=e4_open_journal_orphans_owned(&projected,true,&preview,false,true);
    if(!r)r=native_validate(preview);
    if(preview)ext4_engine_close(preview);
    if(!r)r=jbd2_replay(plan,true);
    jbd2_release(plan);kfree(guard);
    if(!r)r=e4_open_journal_orphans_owned(dev,true,out,false,true);
    if(!r)r=native_validate(*out);
    if(r){if(*out)ext4_engine_close(*out);*out=NULL;return r;}
    ext4_engine_t *e=*out;e->fs->rw_map=kmalloc(sizeof(e4_map_t)+E4_MAP_MAX*sizeof(e4_extent_t));assert(e->fs->rw_map);
    e->fs->engine=e;e->fs->journal_mounted=true;e->fs->detached=true;pn_bound=e;return 0;
}
static void native_close(ext4_engine_t *e){pn_bound=NULL;if(e){e->fs->journal_mounted=false;ext4_engine_close(e);}assert(!live&&!e4_engine_busy);}
static int native_recover(ext4_engine_t *e){
    int r=0;unsigned steps=0;
    for(;;){uint64_t f=spin_lock_irqsave(&e4_lock);r=native_validate(e);
        if(!r&&pn_view.action!=PP_DONE){assert(++steps<=8);r=pn_step(e);spin_unlock_irqrestore(&e4_lock,f);if(r)break;continue;}
        spin_unlock_irqrestore(&e4_lock,f);break;
    }
    return r;
}
static size_t exact(const char *path,void *out,size_t capacity){size_t n;uint8_t *p=load(path,&n);assert(n<=capacity);memcpy(out,p,n);free(p);return n;}
int main(int argc,char **argv){
    assert(argc==8);size_t n;uint8_t *initial=load(argv[1],&n);assert(n==64*1024*1024);
    uint8_t *stable=malloc(n),*volatile_bytes=malloc(n);assert(stable&&volatile_bytes);memcpy(stable,initial,n);memcpy(volatile_bytes,initial,n);
    ext4_fault_disk_t d={.stable=stable,.volatile_bytes=volatile_bytes,.bytes=n,.cut=-1};
    block_dev_t disk={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    uint8_t *capsule=malloc(33920);assert(capsule);size_t cn=exact(argv[2],capsule,33920);assert(cn>1152);
    memcpy(&jb_policy,capsule,1136);memcpy(&pn_grant_floor,capsule+1136,8);memcpy(&pn_state_floor,capsule+1144,8);pn_baseline_n=cn-1152;memcpy(pn_baseline,capsule+1152,pn_baseline_n);free(capsule);
    jb_selector_n=exact(argv[3],jb_selector,sizeof jb_selector);assert(exact(argv[4],jb_witness,sizeof jb_witness)==sizeof jb_witness);
    ext4_engine_t *e=NULL;int r=native_open(&disk,&e);bool accepted=atoi(argv[7]);
    if(!accepted){assert(r&&!d.writes&&!d.flushes);native_close(e);puts("PASS native refusal before writes");goto done;}
    assert(!r);assert(!native_recover(e));size_t events=d.events;assert(!native_recover(e)&&events==d.events);
    assert(pn_view.phase==PG_IDLE);native_close(e);
    FILE *f=fopen(argv[5],"wbx");assert(f&&fwrite(stable,1,n,f)==n&&!fclose(f));
    printf("PASS native recovery writes=%zu flushes=%zu events=%zu\n",d.writes,d.flushes,d.events);
    /* Optional selected power cut, then authenticate projected state afresh. */
    long cut=strtol(argv[6],NULL,10);
    if(cut>=0){
        memcpy(stable,initial,n);ext4_fault_restart(&d);d.cut=cut;assert(!native_open(&disk,&e));r=native_recover(e);assert(r||d.events<=(size_t)cut);
        if(r&&e->tainted){size_t stopped=d.events;assert(native_validate(e)&&native_recover(e)&&d.events==stopped);}
        native_close(e);
        ext4_fault_restart(&d);assert(!native_open(&disk,&e));assert(!native_recover(e));events=d.events;assert(!native_recover(e)&&events==d.events);native_close(e);
        puts("PASS native interrupted recovery and fresh-owner repeat");
    }
done:
    free(stable);free(volatile_bytes);free(initial);assert(!live);return 0;
}
