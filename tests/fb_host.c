#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "fb.h"
#include "boot_info.h"
#include "spinlock.h"
#include "vmm.h"

static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool held;
uint64_t fb_host_lock(spinlock_t *lock) {
    assert(lock->rank==1 && !held); pthread_mutex_lock(&mutex); held=true; return 0;
}
void fb_host_unlock(spinlock_t *lock) {
    (void)lock; assert(held); held=false; pthread_mutex_unlock(&mutex);
}
static boot_info_t boot;
const boot_info_t *boot_info_get(void) { return &boot; }
static bool graphics;
static unsigned restorations, maps, unmaps;
static int map_error, unmap_error;
static bool block_unmap;
static pthread_barrier_t unmap_entered, unmap_continue;
void console_graphics_mode(bool owned) { assert(!held); graphics=owned; if (!owned) restorations++; }
void serial_raw_puts(const char *s) { assert(!held); assert(strstr(s,"Display owner exited")); }
int vmm_map_framebuffer_range(uint64_t *root, uintptr_t va, uintptr_t pa, size_t bytes) {
    assert(!held && root && va%4096==0 && pa==0x10000000 && bytes==1280*800*4);
    maps++; return map_error;
}
int vmm_unmap_device_range(uint64_t *root, uintptr_t va, uintptr_t pa, size_t bytes) {
    assert(!held && root && va%4096==0 && pa==0x10000000 && bytes==1280*800*4);
    unmaps++;
    if (block_unmap) { pthread_barrier_wait(&unmap_entered); pthread_barrier_wait(&unmap_continue); }
    return unmap_error;
}
static void *release_owner(void *arg) { return (void *)(uintptr_t)fb_release((uintptr_t)arg); }
static pthread_barrier_t barrier;
static int results[8];
static void *contender(void *arg) {
    size_t i=(size_t)arg; pthread_barrier_wait(&barrier);
    results[i]=fb_acquire(0x1000+i*4096); return NULL;
}
int main(void) {
    boot=(boot_info_t){.has_framebuffer=true,.fb_width=1280,.fb_height=800,
        .fb_pitch=5120,.fb_bpp=32,.fb_address=0x10000000};
    fb_info_v1_t info;
    uint64_t root[1];
    assert(fb_map(0x1000,root,0,&info)==SYSCALL_EPERM);
    assert(fb_release(0x1000)==SYSCALL_EPERM);
    assert(fb_acquire(0x1000)==0 && graphics);
    assert(fb_acquire(0x1000)==SYSCALL_EBUSY);
    assert(fb_acquire(0x2000)==SYSCALL_EBUSY);
    assert(fb_map(0x1000,root,1,&info)==SYSCALL_EINVAL);
    assert(fb_map(0x1000,root,0x7ffff0000000ULL,&info)==SYSCALL_EINVAL);
    assert(fb_map(0x1000,root,0x800000000000ULL,&info)==SYSCALL_EINVAL);
    assert(fb_release(0x2000)==SYSCALL_EPERM);
    assert(fb_map(0x2000,root,0,&info)==SYSCALL_EPERM);
    map_error=VMM_ERR_NOMEM;
    assert(fb_map(0x1000,root,0,&info)==SYSCALL_ENOMEM);
    map_error=VMM_ERR_ALREADY_MAPPED;
    assert(fb_map(0x1000,root,0,&info)==SYSCALL_EEXIST);
    map_error=0;
    assert(fb_map(0x1000,root,0,&info)==0);
    assert(info.version==1 && info.fb_user_vaddr==0x6000000000ULL);
    unsigned before=maps;
    assert(fb_map(0x1000,root,0,&info)==0 && maps==before);
    assert(fb_map(0x1000,root,0x7000000000ULL,&info)==SYSCALL_EBUSY);
    unmap_error=VMM_ERR_INVALID_ADDR;
    assert(fb_release(0x1000)==SYSCALL_EIO && graphics);
    assert(fb_acquire(0x2000)==SYSCALL_EBUSY);
    unmap_error=0;
    assert(fb_release(0x1000)==0 && !graphics && unmaps==2);
    assert(fb_acquire(0x2000)==0);
    fb_space_destroyed(0x1000); assert(graphics);
    fb_space_destroyed(0x2000); assert(!graphics);
    assert(fb_acquire(0x3000)==0 && fb_map(0x3000,root,0,&info)==0);
    pthread_barrier_init(&unmap_entered,NULL,2); pthread_barrier_init(&unmap_continue,NULL,2);
    block_unmap=true;
    pthread_t releasing; pthread_create(&releasing,NULL,release_owner,(void *)0x3000);
    pthread_barrier_wait(&unmap_entered);
    assert(graphics && fb_acquire(0x4000)==SYSCALL_EBUSY);
    assert(fb_map(0x3000,root,0,&info)==SYSCALL_EBUSY);
    assert(fb_release(0x3000)==SYSCALL_EBUSY);
    pthread_barrier_wait(&unmap_continue);
    void *release_result; pthread_join(releasing,&release_result);
    assert(!release_result && !graphics); block_unmap=false;
    pthread_t threads[8]; pthread_barrier_init(&barrier,NULL,8);
    for (size_t i=0;i<8;i++) pthread_create(&threads[i],NULL,contender,(void *)i);
    for (size_t i=0;i<8;i++) pthread_join(threads[i],NULL);
    unsigned winners=0; uintptr_t winner=0;
    for (size_t i=0;i<8;i++) {
        assert(results[i]==0 || results[i]==SYSCALL_EBUSY);
        if (!results[i]) { winners++; winner=0x1000+i*4096; }
    }
    assert(winners==1 && graphics);
    assert(fb_release(winner)==0 && !graphics);
    boot.fb_pitch=1;
    assert(fb_acquire(0x1000)==SYSCALL_ENODEV);
    boot.fb_width=8192; boot.fb_height=1024; boot.fb_pitch=32768;
    assert(fb_acquire(0x1000)==0); assert(fb_release(0x1000)==0);
    assert(restorations==5);
    puts("PASS framebuffer ownership: actual state machine, failures, aliases, death, 8-way contention");
}
