#include "user/pthread.h"
#include "user/tools/common.h"

static volatile int g_mutex_counter = 0;
static pthread_mutex_t g_test_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile int g_cond_ready = 0;
static volatile int g_cond_finished = 0;
static pthread_mutex_t g_cond_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_test_cond = PTHREAD_COND_INITIALIZER;

static pthread_barrier_t g_test_barrier;
static volatile int g_barrier_phase = 0;

static pthread_once_t g_test_once = PTHREAD_ONCE_INIT;
static volatile int g_once_run_count = 0;

static void *worker_basic(void *arg) {
    uintptr_t id = (uintptr_t)arg;
    return (void *)(uintptr_t)(id * 100 + 42);
}

static void *worker_mutex(void *arg) {
    (void)arg;
    for (int i = 0; i < 500; i++) {
        pthread_mutex_lock(&g_test_mutex);
        g_mutex_counter++;
        pthread_mutex_unlock(&g_test_mutex);
    }
    return NULL;
}

static void *worker_cond(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_cond_mutex);
    while (!g_cond_ready) {
        pthread_cond_wait(&g_test_cond, &g_cond_mutex);
    }
    g_cond_finished = 1;
    pthread_mutex_unlock(&g_cond_mutex);
    return NULL;
}

static void *worker_barrier(void *arg) {
    (void)arg;
    __atomic_fetch_add(&g_barrier_phase, 1, __ATOMIC_SEQ_CST);
    pthread_barrier_wait(&g_test_barrier);
    return NULL;
}

static void once_init_routine(void) {
    __atomic_fetch_add(&g_once_run_count, 1, __ATOMIC_SEQ_CST);
}

static void *worker_once(void *arg) {
    (void)arg;
    pthread_once(&g_test_once, once_init_routine);
    return NULL;
}

int pthread_test_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    tool_write("pthread-test", "=== Testing FortressOS Userland Threads (pthreads) ===\n", 55);

    /* 1. Basic Create & Join */
    tool_write("pthread-test", "[1/5] Testing pthread_create & pthread_join... ", 47);
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) {
        int rc = pthread_create(&threads[i], NULL, worker_basic, (void *)(i + 1));
        if (rc != 0) {
            tool_write("pthread-test", "FAIL (create error)\n", 20);
            return 1;
        }
    }
    for (uintptr_t i = 0; i < 4; i++) {
        void *ret = NULL;
        int rc = pthread_join(threads[i], &ret);
        uintptr_t expected = (i + 1) * 100 + 42;
        if (rc != 0 || (uintptr_t)ret != expected) {
            tool_write("pthread-test", "FAIL (join error or mismatch)\n", 30);
            return 1;
        }
    }
    tool_write("pthread-test", "PASS\n", 5);

    /* 2. Mutex Serialization */
    tool_write("pthread-test", "[2/5] Testing pthread_mutex (concurrent counter)... ", 52);
    g_mutex_counter = 0;
    for (int i = 0; i < 4; i++) {
        if (pthread_create(&threads[i], NULL, worker_mutex, NULL) != 0) {
            tool_write("pthread-test", "FAIL (mutex create error)\n", 26);
            return 1;
        }
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    if (g_mutex_counter != 2000) {
        tool_write("pthread-test", "FAIL (counter mismatch)\n", 24);
        return 1;
    }
    tool_write("pthread-test", "PASS (counter=2000)\n", 20);

    /* 3. Condition Variables */
    tool_write("pthread-test", "[3/5] Testing pthread_cond_wait & pthread_cond_signal... ", 57);
    g_cond_ready = 0;
    g_cond_finished = 0;
    pthread_t cond_thread;
    if (pthread_create(&cond_thread, NULL, worker_cond, NULL) != 0) {
        tool_write("pthread-test", "FAIL (cond create error)\n", 25);
        return 1;
    }
    pthread_yield();
    pthread_mutex_lock(&g_cond_mutex);
    g_cond_ready = 1;
    pthread_cond_signal(&g_test_cond);
    pthread_mutex_unlock(&g_cond_mutex);

    pthread_join(cond_thread, NULL);
    if (!g_cond_finished) {
        tool_write("pthread-test", "FAIL (cond not finished)\n", 25);
        return 1;
    }
    tool_write("pthread-test", "PASS\n", 5);

    /* 4. Barrier */
    tool_write("pthread-test", "[4/5] Testing pthread_barrier... ", 33);
    g_barrier_phase = 0;
    pthread_barrier_init(&g_test_barrier, NULL, 4);
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, worker_barrier, NULL);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    if (g_barrier_phase != 4) {
        tool_write("pthread-test", "FAIL (barrier phase mismatch)\n", 30);
        return 1;
    }
    pthread_barrier_destroy(&g_test_barrier);
    tool_write("pthread-test", "PASS\n", 5);

    /* 5. pthread_once */
    tool_write("pthread-test", "[5/5] Testing pthread_once single-init guarantee... ", 52);
    g_once_run_count = 0;
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, worker_once, NULL);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    if (g_once_run_count != 1) {
        tool_write("pthread-test", "FAIL (once run count != 1)\n", 27);
        return 1;
    }
    tool_write("pthread-test", "PASS (run_count=1)\n", 19);

    tool_write("pthread-test", "=== All Userland Thread Tests PASSED ===\n", 41);
    return 0;
}
