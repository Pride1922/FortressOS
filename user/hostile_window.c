/* hostile-window: test-only misbehaving desktop app. Each start runs the
 * next attack (counter in /tmp/hostile-window.n):
 *   0 absurd sizes, then an unknown request (compositor must drop it)
 *   1 creates a window, then ignores EV_CLOSE forever (must be SIGKILLed)
 *   2 floods 20000 full-window damage requests, then waits
 *   3 exits at once without creating a window
 *   4 asks to open a relative path          (must be dropped)
 *   5 declares a 128-byte path              (must be dropped)
 *   6 commits an incomplete path            (must be dropped)
 *   7 commits 50 valid opens in a burst     (rate limit: one editor)
 * Installed as /bin/hostile-window; tests copy it over /bin/hello-window. */
#include "user/tools/common.h"
#include "src/include/syscall_abi.h"
#include "src/include/desktop_proto.h"

static void send(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d, const char *text) {
    desktop_msg_t m = {.kind = kind, .a = a, .b = b, .c = c, .d = d};
    for (size_t i = 0; text && text[i] && i < DESKTOP_TEXT_MAX; i++) m.text[i] = text[i], m.len++;
    tool_syscall(SYS_WRITE, DESKTOP_FD_REQUESTS, (uintptr_t)&m, sizeof(m));
}

static unsigned next_mode(void) {
    char c = '0';
    long fd = tool_syscall(SYS_OPEN, (uintptr_t)"/tmp/hostile-window.n", VFS_O_RDONLY, 0);
    if (fd >= 0) { tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)&c, 1); tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0); }
    char next = (char)(c + 1);
    fd = tool_syscall(SYS_OPEN, (uintptr_t)"/tmp/hostile-window.n", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0);
    if (fd >= 0) { tool_syscall(SYS_WRITE, (uintptr_t)fd, (uintptr_t)&next, 1); tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0); }
    return (unsigned)(c - '0');
}

static void sleep_forever(void) {
    timespec_t t = {.tv_sec = 3600, .tv_nsec = 0};
    for (;;) tool_syscall(SYS_NANOSLEEP, (uintptr_t)&t, 0, 0);
}

int hostile_window_main(void) {
    switch (next_mode()) {
    case 0:
        send(DESKTOP_REQ_CREATE, 0xffffffffu, 0xffffffffu, 0, 0, "Hostile: sizes");
        send(DESKTOP_REQ_DAMAGE, 0xfffffff0u, 0xfffffff0u, 0xffffffffu, 0xffffffffu, 0);
        send(DESKTOP_REQ_DAMAGE, 0, 0, 0xffffffffu, 0xffffffffu, 0);
        send(DESKTOP_REQ_TITLE, 0, 0, 0, 0, "bad\x01title");
        sleep_forever();
        break;
    case 1:
        send(DESKTOP_REQ_CREATE, 300, 200, 0, 0, "Hostile: ignores close");
        sleep_forever(); /* never reads events, never exits */
        break;
    case 2:
        send(DESKTOP_REQ_CREATE, 400, 300, 0, 0, "Hostile: flood");
        for (int i = 0; i < 20000; i++) send(DESKTOP_REQ_DAMAGE, 0, 0, 400, 300, 0);
        sleep_forever();
        break;
    case 4:
        send(DESKTOP_REQ_CREATE, 300, 200, 0, 0, "Hostile: relative open");
        send(DESKTOP_REQ_OPEN_BEGIN, 5, 0, 0, 0, 0);
        send(DESKTOP_REQ_OPEN_APPEND, 0, 0, 0, 0, "tmp/x");
        send(DESKTOP_REQ_OPEN_COMMIT, 0, 0, 0, 0, 0);
        sleep_forever();
        break;
    case 5:
        send(DESKTOP_REQ_CREATE, 300, 200, 0, 0, "Hostile: long open");
        send(DESKTOP_REQ_OPEN_BEGIN, 128, 0, 0, 0, 0);
        sleep_forever();
        break;
    case 6:
        send(DESKTOP_REQ_CREATE, 300, 200, 0, 0, "Hostile: short open");
        send(DESKTOP_REQ_OPEN_BEGIN, 10, 0, 0, 0, 0);
        send(DESKTOP_REQ_OPEN_APPEND, 0, 0, 0, 0, "/tmp");
        send(DESKTOP_REQ_OPEN_COMMIT, 0, 0, 0, 0, 0);
        sleep_forever();
        break;
    case 7:
        send(DESKTOP_REQ_CREATE, 300, 200, 0, 0, "Hostile: open flood");
        for (int i = 0; i < 50; i++) {
            send(DESKTOP_REQ_OPEN_BEGIN, 14, 0, 0, 0, 0);
            send(DESKTOP_REQ_OPEN_APPEND, 0, 0, 0, 0, "/tmp/flood.txt");
            send(DESKTOP_REQ_OPEN_COMMIT, 0, 0, 0, 0, 0);
        }
        sleep_forever();
        break;
    default:
        break; /* exit without a window */
    }
    return 0;
}
