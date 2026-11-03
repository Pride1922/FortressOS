/* Actual SYS_DMESG with mock counter formatters and user-page adapters.
 * Exercises permission ordering and destination bounds, not hardware/SMP. */
#define TEST_PERMISSIONS_SYSCALLS
#define main pipe_fixture_main
#include "pipe_host.c"
#undef main
cpu_local_t cpu_locals[MAX_DETECTED_CPUS];
volatile bool g_cpu_installed[MAX_DETECTED_CPUS];
static unsigned snapshots,reads;
size_t net_poll_profile_format(char *p,size_t n) {(void)p;(void)n;return 0;}
void dmesg_append_str(const char *p,size_t n) {(void)p;assert(!n);}
size_t dmesg_read(char *p,size_t n) {reads++;memset(p,'L',n);return n;}
size_t iwlwifi_rx_profile_format(char *p,size_t n) {size_t k=n<3?n:3;memcpy(p,"RX\n",k);return k;}
size_t wlan_rx_profile_format(char *p,size_t n) {(void)p;(void)n;return 0;}
size_t net_arp_profile_format(char *p,size_t n) {(void)p;(void)n;return 0;}
size_t ext4_io_profile_format(char *p,size_t n) {snapshots++;memset(p,'P',n);return n;}
int main(void) {
 current.tid=41;current.is_user=true;creds_init_root(&current.creds);
 assert(!process_record_begin(41,0,false,0,0));assert(process_record_bind_creds(41,&current.creds));process_record_commit(41);
 unsigned char *allocation=malloc(DMESG_SIZE+18);assert(allocation);
 for(size_t cap=0;cap<=4096;cap++) {
  memset(allocation,0xa5,DMESG_SIZE+18);unsigned before=snapshots;
  int64_t n=sys_dmesg((uintptr_t)(allocation+1),cap);
  assert(n>=0&&(uint64_t)n<=cap);
  assert(allocation[0]==0xa5);
  for(size_t i=cap+1;i<cap+17;i++)assert(allocation[i]==0xa5);
  assert(snapshots-before==(cap>2052));assert(!heap_live);
  if(cap>2052) assert(allocation[n-3]=='\n'&&!memcmp(allocation+1+n-3,"RX\n",3));
 }
 memset(allocation,0xa5,DMESG_SIZE+18);
 assert(sys_dmesg((uintptr_t)(allocation+1),UINT64_MAX)==DMESG_SIZE);
 assert(allocation[0]==0xa5&&allocation[DMESG_SIZE+1]==0xa5);
 unsigned before=snapshots;
 fail_after=0;
 assert(sys_dmesg((uintptr_t)(allocation+1),4096)==4096);
 assert(snapshots==before&&!heap_live);fail_after=-1;
 unsigned prior_reads=reads;valid_range=false;
 assert(sys_dmesg((uintptr_t)(allocation+1),4096)==SYSCALL_EFAULT);
 assert(snapshots==before&&reads==prior_reads);valid_range=true;
 creds_t old=current.creds,next=old;next.uid=next.euid=next.suid=1000;next.cap_effective=0;
 assert(process_record_publish_creds(41,&old,&next));
 assert(sys_dmesg(0,0)==SYSCALL_EPERM);
 assert(sys_dmesg((uintptr_t)(allocation+1),4096)==SYSCALL_EPERM);
 assert(snapshots==before&&reads==prior_reads);
 free(allocation);puts("PASS actual SYS_DMESG: 4097 sizes, cap clamp, canaries, invalid pages and root-only snapshots");return 0;
}
