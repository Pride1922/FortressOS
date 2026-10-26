#ifndef FB_CAMPAIGN_H
#define FB_CAMPAIGN_H
/* Private snapshot-kernel observer. Never allocated in the production ABI. */
#define FB_TEST_CALL 0x7ff5
#define FB_TEST_CPU 0
#define FB_TEST_BITMAP 1
#define FB_TEST_ROOT 2
#define FB_TEST_ROOT_LIVE 3
#define FB_TEST_LEAK 4
#define FB_TEST_THREAD_LIVE 5
#define FB_TEST_PREEMPTS 6
#define FB_TEST_REMOTE_MASK 7
#define FB_TEST_REMOTE_PREEMPTS 8
#define FB_TEST_WARM_HEAP 9
#define FB_TEST_FAULT_PREEMPTS 10
typedef struct {
    uint64_t kind, id, cpu, root, wins, busy, attempts, ns;
} fb_report_t;
#define FB_READY 1
#define FB_RELEASED 2
#define FB_LEADER_GONE 3
#define FB_PROGRESS 4
#define FB_DONE 5
#define FB_STARTED 6
#endif
