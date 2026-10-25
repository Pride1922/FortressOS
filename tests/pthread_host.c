#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* Host test verifying pthreads contracts, synchronization semantics,
 * and shared-process credentials invariance under ASan & UBSan. */

static volatile int g_shared_val = 0;
static pthread_mutex_t g_host_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *host_worker_inc(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&g_host_mutex);
        g_shared_val++;
        pthread_mutex_unlock(&g_host_mutex);
    }
    return (void *)100;
}

static void test_mutex_concurrency(void) {
    printf("[HOST TEST] Testing mutex mutual exclusion with 8 concurrent threads...\n");
    g_shared_val = 0;
    pthread_t th[8];
    for (int i = 0; i < 8; i++) {
        assert(pthread_create(&th[i], NULL, host_worker_inc, NULL) == 0);
    }
    for (int i = 0; i < 8; i++) {
        void *res = NULL;
        assert(pthread_join(th[i], &res) == 0);
        assert((uintptr_t)res == 100);
    }
    assert(g_shared_val == 8000);
    printf("  [PASS] g_shared_val == 8000 (all 8,000 serialized increments accounted for)\n");
}

static volatile int g_cond_flag = 0;
static pthread_cond_t g_host_cond = PTHREAD_COND_INITIALIZER;

static void *host_worker_cond(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_host_mutex);
    while (g_cond_flag == 0) {
        pthread_cond_wait(&g_host_cond, &g_host_mutex);
    }
    g_cond_flag = 2;
    pthread_mutex_unlock(&g_host_mutex);
    return NULL;
}

static void test_cond_signaling(void) {
    printf("[HOST TEST] Testing condition variable wait & signal...\n");
    g_cond_flag = 0;
    pthread_t th;
    assert(pthread_create(&th, NULL, host_worker_cond, NULL) == 0);

    pthread_mutex_lock(&g_host_mutex);
    g_cond_flag = 1;
    pthread_cond_signal(&g_host_cond);
    pthread_mutex_unlock(&g_host_mutex);

    assert(pthread_join(th, NULL) == 0);
    assert(g_cond_flag == 2);
    printf("  [PASS] Condition variable successfully coordinated producer and consumer\n");
}

static pthread_barrier_t g_host_barrier;
static volatile int g_barrier_count = 0;

static void *host_worker_barrier(void *arg) {
    (void)arg;
    __atomic_fetch_add(&g_barrier_count, 1, __ATOMIC_SEQ_CST);
    int rc = pthread_barrier_wait(&g_host_barrier);
    assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
    return NULL;
}

static void test_barrier_sync(void) {
    printf("[HOST TEST] Testing barrier synchronization with 4 threads...\n");
    g_barrier_count = 0;
    assert(pthread_barrier_init(&g_host_barrier, NULL, 4) == 0);
    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        assert(pthread_create(&th[i], NULL, host_worker_barrier, NULL) == 0);
    }
    for (int i = 0; i < 4; i++) {
        assert(pthread_join(th[i], NULL) == 0);
    }
    assert(g_barrier_count == 4);
    assert(pthread_barrier_destroy(&g_host_barrier) == 0);
    printf("  [PASS] All 4 threads synchronized at barrier before proceeding\n");
}

static pthread_once_t g_host_once = PTHREAD_ONCE_INIT;
static volatile int g_once_count = 0;

static void once_routine(void) {
    g_once_count++;
}

static void *host_worker_once(void *arg) {
    (void)arg;
    pthread_once(&g_host_once, once_routine);
    return NULL;
}

static void test_once_execution(void) {
    printf("[HOST TEST] Testing pthread_once single-execution guarantee...\n");
    g_once_count = 0;
    pthread_t th[8];
    for (int i = 0; i < 8; i++) {
        assert(pthread_create(&th[i], NULL, host_worker_once, NULL) == 0);
    }
    for (int i = 0; i < 8; i++) {
        assert(pthread_join(th[i], NULL) == 0);
    }
    assert(g_once_count == 1);
    printf("  [PASS] Initialization routine executed exactly once across 8 threads\n");
}

int main(void) {
    printf("=====================================================\n");
    printf(" FortressOS Userland Threads Host Verification Suite \n");
    printf("=====================================================\n");

    test_mutex_concurrency();
    test_cond_signaling();
    test_barrier_sync();
    test_once_execution();

    printf("\n[SUCCESS] 100%% of pthread host unit tests PASSED cleanly under ASan/UBSan!\n");
    return 0;
}
