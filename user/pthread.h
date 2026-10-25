#ifndef FORTRESS_USER_PTHREAD_H
#define FORTRESS_USER_PTHREAD_H

#include "types.h"
#include "syscall_abi.h"

#define PTHREAD_STACK_MIN   8192
#define PTHREAD_STACK_SIZE  16384
#define PTHREAD_MAX_THREADS 16

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2

#define PTHREAD_MUTEX_INITIALIZER {0, 0, 0, 0}
#define PTHREAD_COND_INITIALIZER  {0}
#define PTHREAD_SPINLOCK_INITIALIZER {0}
#define PTHREAD_ONCE_INIT         0
#define PTHREAD_BARRIER_SERIAL_THREAD 1

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EPERM
#define EPERM 1
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef EDEADLK
#define EDEADLK 35
#endif
#ifndef ESRCH
#define ESRCH 3
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif

typedef uint64_t pthread_t;

typedef struct {
    void   *stack_addr;
    size_t  stack_size;
    int     detachstate;
} pthread_attr_t;

typedef struct {
    volatile uint32_t lock;
    uint32_t          owner;
    uint32_t          recursion;
    int               type;
} pthread_mutex_t;

typedef struct {
    int type;
} pthread_mutexattr_t;

typedef struct {
    volatile uint32_t seq;
} pthread_cond_t;

typedef struct {
    int dummy;
} pthread_condattr_t;

typedef struct {
    volatile uint32_t lock;
} pthread_spinlock_t;

typedef struct {
    volatile uint32_t in_count;
    volatile uint32_t out_count;
    uint32_t          threshold;
    volatile uint32_t cycle;
} pthread_barrier_t;

typedef struct {
    int dummy;
} pthread_barrierattr_t;

typedef volatile int pthread_once_t;

/* Inline raw syscall helpers for freestanding user mode */
#ifndef PTHREAD_SYSCALL_DEFINED
#define PTHREAD_SYSCALL_DEFINED
static inline long pthread_sys_0(long nr) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr) : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long pthread_sys_1(long nr, uintptr_t a) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a) : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long pthread_sys_2(long nr, uintptr_t a, uintptr_t b) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b) : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long pthread_sys_4(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    long ret;
    register uintptr_t r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}
#endif

/* Thread bootstrapping */
typedef struct {
    void *(*start_routine)(void *);
    void *arg;
    uint8_t used;
} pthread_starter_t;

static uint8_t g_pthread_stack_pool[PTHREAD_MAX_THREADS][PTHREAD_STACK_SIZE] __attribute__((aligned(16)));
static pthread_starter_t g_pthread_starters[PTHREAD_MAX_THREADS];

static void pthread_trampoline(void *arg) {
    pthread_starter_t *s = (pthread_starter_t *)arg;
    void *(*fn)(void *) = s->start_routine;
    void *param = s->arg;
    void *res = fn(param);
    pthread_sys_1(SYS_THREAD_EXIT, (uintptr_t)res);
}

/* Attributes */
static inline int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    attr->stack_addr = NULL;
    attr->stack_size = PTHREAD_STACK_SIZE;
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}

static inline int pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr;
    return 0;
}

static inline int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, size_t stacksize) {
    if (!attr || !stackaddr || stacksize < PTHREAD_STACK_MIN) return EINVAL;
    attr->stack_addr = stackaddr;
    attr->stack_size = stacksize;
    return 0;
}

static inline int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr, size_t *stacksize) {
    if (!attr || !stackaddr || !stacksize) return EINVAL;
    *stackaddr = attr->stack_addr;
    *stacksize = attr->stack_size;
    return 0;
}

static inline int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
    if (!attr || (detachstate != PTHREAD_CREATE_JOINABLE && detachstate != PTHREAD_CREATE_DETACHED))
        return EINVAL;
    attr->detachstate = detachstate;
    return 0;
}

/* Thread lifecycle */
static inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void *(*start_routine)(void *), void *arg) {
    if (!thread || !start_routine) return EINVAL;

    int slot = -1;
    for (int i = 0; i < PTHREAD_MAX_THREADS; i++) {
        uint8_t expected = 0;
        if (__atomic_compare_exchange_n(&g_pthread_starters[i].used, &expected, 1,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return EAGAIN;

    g_pthread_starters[slot].start_routine = start_routine;
    g_pthread_starters[slot].arg = arg;

    uintptr_t stack_top;
    if (attr && attr->stack_addr) {
        stack_top = (uintptr_t)attr->stack_addr + attr->stack_size;
    } else {
        stack_top = (uintptr_t)&g_pthread_stack_pool[slot][PTHREAD_STACK_SIZE];
    }
    stack_top &= ~0xFULL; /* 16-byte alignment */

    uint64_t tid = 0;
    long rc = pthread_sys_4(SYS_THREAD_CREATE, (uintptr_t)pthread_trampoline,
                            stack_top, (uintptr_t)&g_pthread_starters[slot], (uintptr_t)&tid);
    if (rc < 0) {
        __atomic_store_n(&g_pthread_starters[slot].used, 0, __ATOMIC_RELEASE);
        return rc == -SYSCALL_ENOMEM ? EAGAIN : EINVAL;
    }

    *thread = (pthread_t)tid;
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval) {
    uint64_t val = 0;
    long rc = pthread_sys_2(SYS_THREAD_JOIN, (uintptr_t)thread, (uintptr_t)(retval ? &val : NULL));
    if (rc < 0) {
        if (rc == -SYSCALL_EDEADLK) return EDEADLK;
        if (rc == -SYSCALL_ESRCH) return ESRCH;
        if (rc == -SYSCALL_EINVAL) return EINVAL;
        return EINVAL;
    }
    if (retval) *retval = (void *)val;
    return 0;
}

static inline _Noreturn void pthread_exit(void *retval) {
    pthread_sys_1(SYS_THREAD_EXIT, (uintptr_t)retval);
    for (;;) { __asm__ volatile("pause"); }
}

static inline pthread_t pthread_self(void) {
    return (pthread_t)pthread_sys_0(SYS_THREAD_SELF);
}

static inline int pthread_equal(pthread_t t1, pthread_t t2) {
    return t1 == t2;
}

static inline int pthread_yield(void) {
    return (int)pthread_sys_0(SYS_THREAD_YIELD);
}

static inline int sched_yield(void) {
    return pthread_yield();
}

static inline int pthread_detach(pthread_t thread) {
    (void)thread;
    return 0;
}

/* Mutexes (Futex-backed fast-path atomic CAS) */
static inline int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    attr->type = PTHREAD_MUTEX_NORMAL;
    return 0;
}

static inline int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr) return EINVAL;
    attr->type = type;
    return 0;
}

static inline int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    (void)attr;
    return 0;
}

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    if (!mutex) return EINVAL;
    mutex->lock = 0;
    mutex->owner = 0;
    mutex->recursion = 0;
    mutex->type = attr ? attr->type : PTHREAD_MUTEX_NORMAL;
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    uint32_t self = (uint32_t)pthread_self();
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->owner == self && self != 0) {
        mutex->recursion++;
        return 0;
    }

    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&mutex->lock, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        mutex->owner = self;
        mutex->recursion = 1;
        return 0;
    }

    if (c != 2) {
        c = __atomic_exchange_n(&mutex->lock, 2, __ATOMIC_ACQ_REL);
    }
    while (c != 0) {
        pthread_sys_4(SYS_FUTEX, (uintptr_t)&mutex->lock, FUTEX_WAIT, 2, 0);
        c = __atomic_exchange_n(&mutex->lock, 2, __ATOMIC_ACQ_REL);
    }
    mutex->owner = self;
    mutex->recursion = 1;
    return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    uint32_t self = (uint32_t)pthread_self();
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->owner == self && self != 0) {
        mutex->recursion++;
        return 0;
    }

    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&mutex->lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        mutex->owner = self;
        mutex->recursion = 1;
        return 0;
    }
    return EBUSY;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    uint32_t self = (uint32_t)pthread_self();
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        if (mutex->owner != self) return EPERM;
        if (--mutex->recursion > 0) return 0;
    }
    mutex->owner = 0;
    if (__atomic_exchange_n(&mutex->lock, 0, __ATOMIC_RELEASE) == 2) {
        pthread_sys_4(SYS_FUTEX, (uintptr_t)&mutex->lock, FUTEX_WAKE, 1, 0);
    }
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    (void)mutex;
    return 0;
}

/* Condition Variables */
static inline int pthread_condattr_init(pthread_condattr_t *attr) { (void)attr; return 0; }
static inline int pthread_condattr_destroy(pthread_condattr_t *attr) { (void)attr; return 0; }

static inline int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    if (!cond) return EINVAL;
    cond->seq = 0;
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (!cond || !mutex) return EINVAL;
    uint32_t seq = cond->seq;
    pthread_mutex_unlock(mutex);
    pthread_sys_4(SYS_FUTEX, (uintptr_t)&cond->seq, FUTEX_WAIT, seq, 0);
    pthread_mutex_lock(mutex);
    return 0;
}

static inline int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                        const timespec_t *abstime) {
    if (!cond || !mutex || !abstime) return EINVAL;
    uint32_t seq = cond->seq;
    pthread_mutex_unlock(mutex);
    long rc = pthread_sys_4(SYS_FUTEX, (uintptr_t)&cond->seq, FUTEX_WAIT, seq, (uintptr_t)abstime);
    pthread_mutex_lock(mutex);
    if (rc == -SYSCALL_ETIMEDOUT) return ETIMEDOUT;
    return 0;
}

static inline int pthread_cond_signal(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    __atomic_fetch_add(&cond->seq, 1, __ATOMIC_SEQ_CST);
    pthread_sys_4(SYS_FUTEX, (uintptr_t)&cond->seq, FUTEX_WAKE, 1, 0);
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    __atomic_fetch_add(&cond->seq, 1, __ATOMIC_SEQ_CST);
    pthread_sys_4(SYS_FUTEX, (uintptr_t)&cond->seq, FUTEX_WAKE, 0x7FFFFFFF, 0);
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

/* Spinlocks */
static inline int pthread_spin_init(pthread_spinlock_t *lock, int pshared) {
    (void)pshared;
    if (!lock) return EINVAL;
    lock->lock = 0;
    return 0;
}

static inline int pthread_spin_lock(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;
    while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause" ::: "memory");
    }
    return 0;
}

static inline int pthread_spin_trylock(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;
    if (!__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) return 0;
    return EBUSY;
}

static inline int pthread_spin_unlock(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;
    __atomic_clear(&lock->lock, __ATOMIC_RELEASE);
    return 0;
}

static inline int pthread_spin_destroy(pthread_spinlock_t *lock) {
    (void)lock;
    return 0;
}

/* Barriers */
static inline int pthread_barrierattr_init(pthread_barrierattr_t *attr) { (void)attr; return 0; }
static inline int pthread_barrierattr_destroy(pthread_barrierattr_t *attr) { (void)attr; return 0; }

static inline int pthread_barrier_init(pthread_barrier_t *barrier,
                                      const pthread_barrierattr_t *attr, unsigned count) {
    (void)attr;
    if (!barrier || count == 0) return EINVAL;
    barrier->in_count = 0;
    barrier->out_count = 0;
    barrier->threshold = count;
    barrier->cycle = 0;
    return 0;
}

static inline int pthread_barrier_wait(pthread_barrier_t *barrier) {
    if (!barrier) return EINVAL;
    uint32_t cycle = barrier->cycle;
    uint32_t step = __atomic_add_fetch(&barrier->in_count, 1, __ATOMIC_SEQ_CST);
    if (step == barrier->threshold) {
        barrier->in_count = 0;
        __atomic_fetch_add(&barrier->cycle, 1, __ATOMIC_SEQ_CST);
        pthread_sys_4(SYS_FUTEX, (uintptr_t)&barrier->cycle, FUTEX_WAKE, 0x7FFFFFFF, 0);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    } else {
        while (barrier->cycle == cycle) {
            pthread_sys_4(SYS_FUTEX, (uintptr_t)&barrier->cycle, FUTEX_WAIT, cycle, 0);
        }
        return 0;
    }
}

static inline int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    (void)barrier;
    return 0;
}

/* pthread_once */
static inline int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) return EINVAL;
    if (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == 2) return 0;

    int expected = 0;
    if (__atomic_compare_exchange_n(once_control, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init_routine();
        __atomic_store_n(once_control, 2, __ATOMIC_RELEASE);
        pthread_sys_4(SYS_FUTEX, (uintptr_t)once_control, FUTEX_WAKE, 0x7FFFFFFF, 0);
        return 0;
    }
    while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == 1) {
        pthread_sys_4(SYS_FUTEX, (uintptr_t)once_control, FUTEX_WAIT, 1, 0);
    }
    return 0;
}

#endif /* FORTRESS_USER_PTHREAD_H */
