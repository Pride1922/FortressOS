#include "user/tools/common.h"
#include "signal_abi.h"

static volatile int g_handler_called = 0;

static void segv_handler(int sig) {
    (void)sig;
    g_handler_called = 1;
    tool_write("crash-test", "[CRASH-TEST] Caught SIGSEGV in user handler successfully!\n", 58);
    tool_syscall(SYS_EXIT, 0, 0, 0);
}

static void recfault_handler(int sig) {
    (void)sig;
    tool_write("crash-test", "[CRASH-TEST] In handler; triggering recursive segfault...\n", 57);
    volatile int *bad = (volatile int *)0;
    *bad = 999;
}

int crash_test_main(int argc, char **argv) {
    if (argc < 2 || tool_equal(argv[1], "--help") || tool_equal(argv[1], "-h")) {
        const char usage[] =
            "Usage: crash-test <mode>\n"
            "Modes:\n"
            "  null      Trigger NULL pointer dereference (#PF / SIGSEGV)\n"
            "  divzero   Trigger integer division by zero (#DE / SIGFPE)\n"
            "  illegal   Trigger invalid opcode (#UD / SIGILL)\n"
            "  handler   Install custom SIGSEGV handler and test recovery\n"
            "  recfault  Install handler that causes another fault (recursive fault)\n";
        tool_write("crash-test", usage, sizeof(usage) - 1);
        return 0;
    }

    if (tool_equal(argv[1], "null")) {
        tool_write("crash-test", "[CRASH-TEST] Triggering NULL dereference...\n", 44);
        volatile int *ptr = (volatile int *)0;
        *ptr = 0x1337;
        return 0;
    }

    if (tool_equal(argv[1], "divzero")) {
        tool_write("crash-test", "[CRASH-TEST] Triggering division by zero...\n", 44);
        volatile int denom = 0;
        volatile int numer = 100;
        volatile int res = numer / denom;
        (void)res;
        return 0;
    }

    if (tool_equal(argv[1], "illegal")) {
        tool_write("crash-test", "[CRASH-TEST] Triggering invalid opcode (ud2)...\n", 48);
        __asm__ volatile("ud2");
        return 0;
    }

    if (tool_equal(argv[1], "handler")) {
        tool_write("crash-test", "[CRASH-TEST] Installing SIGSEGV handler...\n", 43);
        signal_action_t sa;
        sa.handler = (uintptr_t)segv_handler;
        sa.mask = 0;
        sa.flags = 0;
        sa.reserved = 0;
        if (tool_syscall(SYS_SIGACTION, SIGSEGV, (uintptr_t)&sa, 0) != 0) {
            tool_write("crash-test", "[CRASH-TEST] Failed to install handler\n", 39);
            return 1;
        }
        tool_write("crash-test", "[CRASH-TEST] Faulting now...\n", 29);
        volatile int *ptr = (volatile int *)0;
        *ptr = 0x42;
        return 0;
    }

    if (tool_equal(argv[1], "recfault")) {
        tool_write("crash-test", "[CRASH-TEST] Installing recursive fault handler...\n", 51);
        signal_action_t sa;
        sa.handler = (uintptr_t)recfault_handler;
        sa.mask = 0;
        sa.flags = 0;
        sa.reserved = 0;
        if (tool_syscall(SYS_SIGACTION, SIGSEGV, (uintptr_t)&sa, 0) != 0) {
            tool_write("crash-test", "[CRASH-TEST] Failed to install handler\n", 39);
            return 1;
        }
        tool_write("crash-test", "[CRASH-TEST] Initial fault...\n", 30);
        volatile int *ptr = (volatile int *)0;
        *ptr = 0x42;
        return 0;
    }

    tool_write("crash-test", "Unknown mode. Run 'crash-test --help'.\n", 39);
    return 1;
}
