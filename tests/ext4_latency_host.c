/* Measurement adapter only: pthread wait/hold is not hardware IRQ latency. */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdint.h>
#include <stdatomic.h>
static uint64_t ext4_latency_now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
typedef struct {uint64_t waits[32768],holds[32768],start;unsigned n;} samples_t;
static _Thread_local samples_t sample;
static void ext4_latency_acquired(uint64_t before) {
    sample.start=ext4_latency_now();
    if(sample.n<32768)sample.waits[sample.n]=sample.start-before;
}
static void ext4_latency_releasing(void) {
    if(sample.n<32768)sample.holds[sample.n++]=ext4_latency_now()-sample.start;
}
#define EXT4_HOST_LATENCY
#define TEST_PERMISSIONS_ENFORCEMENT
#define TEST_EXT4_ATOMIC_COUNTERS
#define main previous_mount_main
#include "ext4_mount_host.c"
#undef main
static unsigned delay_ms;
static _Atomic bool in_flush;
static jbd2_writer_t *observed_writer;
static uint64_t flush_ns[6],flush_count[6];
static bool measured_flush(block_dev_t *dev) {
    unsigned state=observed_writer?(unsigned)jbd2_writer_state(observed_writer):0;
    uint64_t start=ext4_latency_now();
    atomic_store(&in_flush,true);
    if(delay_ms) {
        struct timespec t={.tv_sec=delay_ms/1000,.tv_nsec=(delay_ms%1000)*1000000L};
        while(nanosleep(&t,&t)) {}
    }
    bool ok=ext4_fault_flush(dev);atomic_store(&in_flush,false);
    assert(state<6);flush_count[state]++;flush_ns[state]+=ext4_latency_now()-start;return ok;
}
static int compare(const void *a,const void *b) {
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);
}
static void distribution(const char *label,uint64_t *values,unsigned n) {
    assert(n);qsort(values,n,sizeof(*values),compare);
    printf("{\"metric\":\"%s\",\"samples\":%u,\"p50_ns\":%llu,\"p95_ns\":%llu,\"max_ns\":%llu}\n",
        label,n,(unsigned long long)values[n/2],(unsigned long long)values[(n-1)*95/100],(unsigned long long)values[n-1]);
}
static void locks(const char *label) {
    char name[96];snprintf(name,sizeof(name),"%s_lock_wait",label);distribution(name,sample.waits,sample.n);
    snprintf(name,sizeof(name),"%s_lock_hold",label);distribution(name,sample.holds,sample.n);sample.n=0;
}
static file_t *open_new(const char *path) {file_t *f=vfs_open(path,VFS_O_CREAT|VFS_O_RDWR);assert(f);return f;}
static uint64_t peer_ns;
static void *peer(void *arg) {
    file_t *f=arg;while(!atomic_load(&in_flush)) {}
    uint64_t start=ext4_latency_now();assert(vfs_write(f,"LOG\n",4)==4);peer_ns=ext4_latency_now()-start;
    locks("concurrent_log");return NULL;
}
int main(int argc,char **argv) {
    assert(argc==4);size_t n;uint8_t *initial=load(argv[1],&n);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=measured_flush,.priv=&d};
    ext4_mount_t *fs;reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    file_t *f=open_new("/mnt/staging");static uint8_t bytes[16384];memset(bytes,'L',sizeof(bytes));
    observed_writer=fs->engine->writer;memset(flush_ns,0,sizeof(flush_ns));memset(flush_count,0,sizeof(flush_count));
    uint64_t times[640],start=ext4_latency_now();size_t w=d.writes,b=d.flushes;sample.n=0;
    for(unsigned i=0;i<640;i++) {uint64_t t=ext4_latency_now();assert(vfs_write(f,bytes,sizeof(bytes))==sizeof(bytes));times[i]=ext4_latency_now()-t;}
    assert(d.flushes-b==3840 && flush_count[JBD2_WRITE_COMMITTING]==2560 && flush_count[JBD2_WRITE_CHECKPOINTING]==1280);
    printf("{\"phase\":\"staging\",\"bytes\":10485760,\"calls\":640,\"writes\":%zu,\"flushes\":%zu,\"elapsed_ns\":%llu}\n",d.writes-w,d.flushes-b,(unsigned long long)(ext4_latency_now()-start));
    distribution("write",times,640);locks("staging");assert(!vfs_close(f));
    printf("{\"phase\":\"staging_barriers\",\"commit_flushes\":%llu,\"commit_flush_ns\":%llu,\"checkpoint_flushes\":%llu,\"checkpoint_flush_ns\":%llu}\n",
        (unsigned long long)flush_count[JBD2_WRITE_COMMITTING],(unsigned long long)flush_ns[JBD2_WRITE_COMMITTING],
        (unsigned long long)flush_count[JBD2_WRITE_CHECKPOINTING],(unsigned long long)flush_ns[JBD2_WRITE_CHECKPOINTING]);
    w=d.writes;b=d.flushes;sample.n=0;start=ext4_latency_now();assert(!ext4_sync(fs));
    printf("{\"phase\":\"final_sync\",\"writes\":%zu,\"flushes\":%zu,\"elapsed_ns\":%llu}\n",d.writes-w,d.flushes-b,(unsigned long long)(ext4_latency_now()-start));locks("final_sync");
    /* The peer starts its append while a deliberately slow sync owns exclusion.
     * Sleeping models device delay ONLY in this host callback, never in kernel. */
    f=open_new("/mnt/log");pthread_t thread;delay_ms=100;atomic_store(&in_flush,false);sample.n=0;
    assert(!pthread_create(&thread,NULL,peer,f));start=ext4_latency_now();assert(!ext4_sync(fs));assert(!pthread_join(thread,NULL));delay_ms=0;
    printf("{\"phase\":\"slow_sync\",\"injected_flush_ms\":100,\"concurrent_append_ns\":%llu,\"elapsed_ns\":%llu}\n",(unsigned long long)peer_ns,(unsigned long long)(ext4_latency_now()-start));locks("slow_sync");assert(!vfs_close(f));
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"clean",d.stable,n);
    /* A real failed flush is fenced, not retried by this measurement. */
    observed_writer=NULL;teardown();ext4_fault_restart(&d);vfs_init();assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    d.cut=(long)d.events;delay_ms=100;start=ext4_latency_now();assert(ext4_sync(fs)==-VFS_EIO);delay_ms=0;
    mount_info_t m={0};assert(ext4_get_mount_info(&fs->cached[0]->node,&m));assert(m.flags&MOUNT_FLAGS_RECOVERY_REQUIRED);
    size_t stop=d.events;assert(ext4_sync(fs)==-VFS_EIO&&d.events==stop);
    printf("{\"phase\":\"failed_sync\",\"elapsed_ns\":%llu,\"flags\":%u,\"later_io_events\":0}\n",(unsigned long long)(ext4_latency_now()-start),m.flags);
    teardown();ext4_fault_restart(&d);vfs_init();assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));assert(!ext4_freeze_and_sync(fs));save(argv[3],"recovered",d.stable,n);
    teardown();free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}
