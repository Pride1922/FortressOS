#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "../src/kernel/random.c"

static unsigned features=3, calls, fail_at;
static unsigned stuck;
static uint64_t word_counter=1;
static unsigned seed_wipes, boot_seed_wipes, user_wipes;
void random_test_wiped(const void *p,size_t n) {
    for(size_t i=0;i<n;i++) assert(((const uint8_t *)p)[i]==0);
    if(n==32) seed_wipes++;
    if(n==80) boot_seed_wipes++;
    if(n==MAX_GETRANDOM_LEN) user_wipes++;
}
unsigned random_hw_features(void) { return features; }
bool random_hw_word(bool seed, uint64_t *out) {
    (void)seed; calls++;
    if (fail_at && calls>=fail_at) return false;
    if (stuck) { *out=stuck==1 ? 0 : stuck==2 ? UINT64_MAX : 42; return true; }
    *out=++word_counter; return true;
}
uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!pthread_mutex_lock(&l->mutex)); return 0;
}
void spin_unlock_irqrestore(spinlock_t *l,uint64_t irq) {
    (void)irq; assert(!pthread_mutex_unlock(&l->mutex));
}
uint64_t *vmm_get_active_pml4_virt(void) { return NULL; }
static uintptr_t valid_address;
static bool valid=true;
bool vmm_validate_user_range(uint64_t *p,uintptr_t a,size_t n,bool write) {
    (void)p; assert(write); return valid && a==valid_address && n<=256;
}
static void reset(void) {
    initialized=false; stats=(random_stats_t){0};
    random_wipe(&generator,sizeof(generator));
    features=3; calls=0; fail_at=0; stuck=0; word_counter=1;
}
static uint8_t outputs[8][128][32];
static void *worker(void *arg) {
    size_t id=(size_t)arg;
    for (unsigned i=0;i<128;i++) assert(random_get(outputs[id][i],32)==32);
    return NULL;
}
static void hex(const uint8_t *p,size_t n) {
    for(size_t i=0;i<n;i++) printf("%02x",p[i]);
    puts("");
}
static void unhex(const char *s,uint8_t *out,size_t n) {
    for(size_t i=0;i<n;i++) {
        unsigned x; assert(sscanf(s+i*2,"%2x",&x)==1); out[i]=(uint8_t)x;
    }
}
int main(int argc,char **argv) {
    if (argc>1) {
        /* Compared with separate Python hashlib/HMAC implementation. */
        (void)argv; hmac_drbg_t d; uint8_t seed[48],out[64];
        for(unsigned i=0;i<48;i++) seed[i]=(uint8_t)i;
        hmac_drbg_init(&d,seed,48);
        assert(hmac_drbg_generate(&d,out,64)); hex(out,64);
        assert(hmac_drbg_generate(&d,out,64)); hex(out,64);
        hmac_drbg_reseed(&d,seed,32);
        assert(hmac_drbg_generate(&d,out,64)); hex(out,64);
        return 0;
    }
    _Static_assert(SYS_GETRANDOM==77,"ABI");
    /* NIST CAVP no-reseed SHA-256 COUNT=0, two generate calls, second output.
     * Also published in OpenSSL openssl-3.0.15 evprand.txt HMAC-DRBG section. */
    hmac_drbg_t kat; uint8_t katseed[48], katout[128], expected[128];
    unhex("ca851911349384bffe89de1cbdc46e6831e44d34a4fb935ee285dd14b71a7488"
          "659ba96c601dc69fc902940805ec0ca8",katseed,48);
    unhex("e528e9abf2dece54d47c7e75e5fe302149f817ea9fb4bee6f4199697d04d5b89"
          "d54fbb978a15b5c443c9ec21036d2460b6f73ebad0dc2aba6e624abf07745bc1"
          "07694bb7547bb0995f70de25d6b29e2d3011bb19d27676c07162c8b5ccde0668"
          "961df86803482cb37ed6d5c0bb8d50cf1f50d476aa0458bdaba806f48be9dcb8",expected,128);
    hmac_drbg_init(&kat,katseed,48);
    assert(hmac_drbg_generate(&kat,katout,128));
    assert(hmac_drbg_generate(&kat,katout,128));
    assert(!memcmp(katout,expected,128));
    puts("PASS NIST CAVP HMAC-DRBG SHA-256 no-reseed COUNT=0");
    uint8_t b[257],copy[257]; memset(b,0xa5,sizeof(b)); memcpy(copy,b,sizeof(b));
    valid_address=(uintptr_t)b;
    assert(random_user(0,0,0)==SYSCALL_EINVAL);
    assert(random_user(0,257,0)==SYSCALL_EINVAL);
    assert(random_user(0,32,1)==SYSCALL_EINVAL);
    assert(random_user(0,32,1ULL<<32)==SYSCALL_EINVAL);
    assert(random_user(0,32,0)==SYSCALL_EFAULT);
    assert(random_user(UINT64_MAX-8,32,0)==SYSCALL_EFAULT);
    assert(random_user(0xffffffff80000000ULL,32,0)==SYSCALL_EFAULT);
    valid=false; assert(random_user(valid_address,32,0)==SYSCALL_EFAULT); valid=true;
    assert(random_user(valid_address,32,0)==SYSCALL_EIO);
    for(unsigned f=0;f<4;f++) {
        reset(); features=f; random_init(NULL,0);
        assert(random_ready()==(f!=0));
        if (f) {
            assert(random_user(valid_address,256,0)==256);
            assert(b[256]==0xa5);
        } else assert(random_user(valid_address,32,0)==SYSCALL_EIO);
    }
    reset(); fail_at=1; random_init(NULL,0);
    assert(!random_ready()); assert(calls==64); /* finite stalled source */
    for(unsigned s=1;s<=3;s++) {
        reset(); stuck=s; random_init(NULL,0); assert(!random_ready());
    }
    reset(); random_init(NULL,0);
    for(size_t n=1;n<=256;n++) {
        memset(b,0xa5,sizeof(b));
        assert(random_user(valid_address,n,0)==(int64_t)n);
        for(size_t i=n;i<sizeof(b);i++) assert(b[i]==0xa5);
    }
    reset(); random_init(NULL,0); assert(random_ready());
    memset(b,0xa5,sizeof(b)); fail_at=calls+2;
    assert(random_user(valid_address,256,0)==SYSCALL_EIO);
    assert(!memcmp(b,copy,sizeof(b))); assert(!random_ready());
    for(size_t i=0;i<sizeof(generator);i++) assert(((uint8_t *)&generator)[i]==0);
    fail_at=0; assert(random_get(b,32)==SYSCALL_EIO); /* latched */
    reset(); random_init(NULL,0);
    pthread_t threads[8];
    for(size_t i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
    for(size_t i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    for(size_t i=0;i<1024;i++) for(size_t j=0;j<i;j++)
        assert(memcmp((uint8_t *)outputs+i*32,(uint8_t *)outputs+j*32,32));
    random_stats_t snapshot; random_stats(&snapshot);
    assert(snapshot.bytes==32768 && snapshot.reseeds==1024 && !snapshot.hardware_failures);
    hmac_drbg_t d; uint8_t seed[48]={1}; hmac_drbg_init(&d,seed,48);
    d.requests=HMAC_DRBG_INTERVAL; assert(hmac_drbg_generate(&d,b,1));
    memset(b,0xa5,sizeof(b)); assert(!hmac_drbg_generate(&d,b,32));
    assert(!memcmp(b,copy,sizeof(b)));
    hmac_drbg_reseed(&d,seed,32); assert(hmac_drbg_generate(&d,b,32));
    random_wipe(b,sizeof(b)); for(size_t i=0;i<sizeof(b);i++) assert(!b[i]);
    assert(seed_wipes && boot_seed_wipes && user_wipes);
    puts("PASS random actual-code ABI/failure/exhaustion/wipe + 8-thread 1024 unique requests");
    return 0;
}
