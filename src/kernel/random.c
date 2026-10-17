#include "random.h"
#include "../lib/hmac_drbg.h"
#include "../../user/tools/digest.h"
#include "spinlock.h"
#include "syscall_abi.h"
#include "vmm.h"
#include "string.h"

static spinlock_t random_lock=SPINLOCK_RANKED(1,"random");
static hmac_drbg_t generator;
static random_stats_t stats;
static bool initialized;

void random_wipe(void *p, size_t n) {
#ifdef RANDOM_HOST_TEST
    size_t original_length=n;
#endif
    volatile uint8_t *v=p;
    while (n--) *v++=0;
    __asm__ volatile("" ::: "memory");
#ifdef RANDOM_HOST_TEST
    extern void random_test_wiped(const void *,size_t);
    random_test_wiped(p,original_length);
#endif
}
#ifdef RANDOM_HOST_TEST
/* Host-only instruction adapters; never compiled into a release kernel. */
extern unsigned random_hw_features(void);
extern bool random_hw_word(bool seed, uint64_t *out);
#else
static unsigned random_hw_features(void) {
    uint32_t a,b,c,d,max; unsigned features=0;
    __asm__ volatile("cpuid":"=a"(max),"=b"(b),"=c"(c),"=d"(d):"a"(0),"c"(0));
    if (max>=1) {
        __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
        if (c & (1u<<30)) features|=1;
    }
    if (max>=7) {
        __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(7),"c"(0));
        if (b & (1u<<18)) features|=2;
    }
    return features;
}
static bool random_hw_word(bool seed, uint64_t *out) {
    unsigned char ok;
    if (seed) __asm__ volatile("rdseed %0; setc %1":"=r"(*out),"=qm"(ok)::"cc");
    else __asm__ volatile("rdrand %0; setc %1":"=r"(*out),"=qm"(ok)::"cc");
    return ok!=0;
}
#endif
/* Recheck CPUID on the executing CPU (including APs). Prefer RDSEED.
 * If advertised RDSEED stalls, fail rather than downgrade that call to RDRAND.
 * RDRAND-only machines are explicitly trusted as a hardware DRBG source.
 * Reject obvious stuck outputs, but this is not an entropy/health certification. */
static bool entropy(uint8_t *out, size_t n) {
    unsigned features=random_hw_features();
    if (!features) return false;
    uint64_t value=0, previous=0;
    bool good=true;
    for (size_t offset=0;offset<n;offset+=8) {
        bool ok=false;
        for (unsigned retry=0;retry<64 && !ok;retry++)
            ok=random_hw_word((features&2)!=0,&value);
        if (!ok || !value || value==UINT64_MAX || (offset && value==previous)) {
            good=false; break;
        }
        memcpy(out+offset,&value,8); previous=value;
    }
    random_wipe(&value,sizeof(value)); random_wipe(&previous,sizeof(previous));
    return good;
}
void random_init(const boot_info_t *boot, uint64_t epoch) {
    /* Boot only; not reachable from user input or process lifecycle. */
    if (initialized) return;
    initialized=true;
    uint8_t seed[80]={0}; digest_t ctx;
    uint32_t lo,hi;
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    uint64_t tsc=((uint64_t)hi<<32)|lo;
    digest_init(&ctx,DIGEST_SHA256);
    digest_update(&ctx,"FortressOS random v1",19);
    digest_update(&ctx,&tsc,sizeof(tsc)); digest_update(&ctx,&epoch,sizeof(epoch));
    if (boot) {
        size_t count=boot->memmap_entry_count;
        if (count>MAX_BOOT_MEMMAP_ENTRIES) count=MAX_BOOT_MEMMAP_ENTRIES;
        digest_update(&ctx,boot->memmap_entries,count*sizeof(boot->memmap_entries[0]));
    }
    digest_final(&ctx,seed+48);
    if (entropy(seed,48)) {
        hmac_drbg_init(&generator,seed,sizeof(seed)); stats.ready=true;
    } else stats.hardware_failures++;
    random_wipe(seed,sizeof(seed)); random_wipe(&ctx,sizeof(ctx));
    random_wipe(&tsc,sizeof(tsc));
}
bool random_ready(void) {
    uint64_t irq=spin_lock_irqsave(&random_lock);
    bool ready=stats.ready;
    spin_unlock_irqrestore(&random_lock,irq); return ready;
}
int64_t random_get(void *out, size_t length) {
    if (!out || !length || length>MAX_GETRANDOM_LEN) return SYSCALL_EINVAL;
    uint8_t seed[32]={0};
    uint64_t irq=spin_lock_irqsave(&random_lock);
    int64_t result=SYSCALL_EIO;
    if (stats.ready) {
        /* Prediction resistance: fresh hardware reseed before EVERY request.
         * A failed source latches unavailable until reboot, erasing old state. */
        if (!entropy(seed,sizeof(seed))) {
            stats.hardware_failures++; stats.ready=false;
            random_wipe(&generator,sizeof(generator));
        } else {
            hmac_drbg_reseed(&generator,seed,sizeof(seed)); stats.reseeds++;
            if (hmac_drbg_generate(&generator,out,length)) {
                stats.bytes+=length; result=(int64_t)length;
            } else result=SYSCALL_EAGAIN;
        }
    }
    random_wipe(seed,sizeof(seed));
    spin_unlock_irqrestore(&random_lock,irq); return result;
}
void random_stats(random_stats_t *out) {
    uint64_t irq=spin_lock_irqsave(&random_lock);
    *out=stats;
    spin_unlock_irqrestore(&random_lock,irq);
}
int64_t random_user(uintptr_t address, size_t length, uint64_t flags) {
    if (!length || length>MAX_GETRANDOM_LEN || flags) return SYSCALL_EINVAL;
    if (!vmm_validate_user_range(vmm_get_active_pml4_virt(),address,length,true))
        return SYSCALL_EFAULT;
    uint8_t bytes[MAX_GETRANDOM_LEN]={0};
    int64_t result=random_get(bytes,length);
    /* No user access under random_lock. Current task owns its address space;
     * no scheduling/blocking between validation and copy. */
    if (result>0) memcpy((void *)address,bytes,length);
    random_wipe(bytes,sizeof(bytes)); return result;
}
