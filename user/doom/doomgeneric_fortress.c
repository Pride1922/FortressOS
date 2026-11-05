/* FortressOS desktop and doomgeneric platform adapter
 * Implements Stage 1 & Stage 2: SHM desktop window client, presentation,
 * integer scaling, letterbox clipping, non-blocking event polling, and lifecycle.
 */

#include "doom_runtime.h"

#ifndef DOOM_HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#else
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#endif

#include "src/include/types.h"
#include "src/include/syscall_abi.h"
#include "src/include/desktop_proto.h"
#include "upstream/doomkeys.h"
#include "upstream/doomgeneric.h"

#define PIXELS_VA 0x7000000000ULL
#define KEY_QUEUE_CAP 64

#define DOOM_OUTER_W 642 /* 640 content + 2 window border */
#define DOOM_OUTER_H 426 /* 400 content + 25 title bar + 1 border */

typedef struct {
    int pressed;
    unsigned char key;
} doom_key_ev_t;

typedef struct {
    uint64_t frame_count;
    uint64_t damage_count;
    uint64_t damaged_pixels;
    uint32_t win_width;
    uint32_t win_height;
    uint32_t win_stride;
    uint32_t scale;
    bool is_desktop;
} fortress_desktop_stats_t;

/* State variables */
static bool s_is_desktop = false;
static uint32_t *s_shm_pixels = NULL;
static uint64_t s_shm_size = 0;
static uint32_t s_win_width = 0;
static uint32_t s_win_height = 0;
static uint32_t s_win_stride = 0;
static uint32_t s_current_scale = 1;
static volatile bool s_shutdown_requested = false;
static int s_target_frames = -1;

static uint64_t s_frame_count = 0;
static uint64_t s_damage_count = 0;
static uint64_t s_damaged_pixels = 0;
static uint32_t s_last_pixel = 0;
static uint64_t s_start_time_ms = 0;

static doom_key_ev_t s_key_queue[KEY_QUEUE_CAP];
static unsigned s_key_head = 0;
static unsigned s_key_tail = 0;

#ifdef DOOM_HOST_TEST
static int s_mock_req_fd = -1;
static int s_mock_ev_fd = -1;
#endif

#ifndef DOOM_HOST_TEST
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} timespec_internal_t;

static uint64_t fortress_get_time_ms(void) {
    timespec_internal_t ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    doom_syscall(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts, 0, 0, 0, 0);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
#else
static uint64_t fortress_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
#endif

/* Upstream shutdown hooks */
extern void I_Quit(void);
extern void I_AtExit(void (*func)(void), bool runfirst);

/* Desktop Protocol Message I/O */
static bool desktop_send_msg(const desktop_msg_t *msg) {
    int fd = DESKTOP_FD_REQUESTS;
#ifdef DOOM_HOST_TEST
    if (s_mock_req_fd >= 0) fd = s_mock_req_fd;
#endif
    const char *p = (const char *)msg;
    size_t done = 0;
    while (done < sizeof(*msg)) {
#ifndef DOOM_HOST_TEST
        long n = doom_syscall(SYS_WRITE, fd, (uintptr_t)(p + done), sizeof(*msg) - done, 0, 0, 0);
#else
        ssize_t n = write(fd, p + done, sizeof(*msg) - done);
#endif
        if (n == -22 /* EINTR */) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

static bool desktop_send(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d, const char *text) {
    desktop_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = kind;
    m.a = a;
    m.b = b;
    m.c = c;
    m.d = d;
    if (text) {
        size_t len = 0;
        while (text[len] && len < DESKTOP_TEXT_MAX) {
            m.text[len] = text[len];
            len++;
        }
        m.len = (uint32_t)len;
    }
    return desktop_send_msg(&m);
}

__attribute__((unused))
static bool desktop_receive_blocking(desktop_msg_t *out) {
    int fd = DESKTOP_FD_EVENTS;
#ifdef DOOM_HOST_TEST
    if (s_mock_ev_fd >= 0) fd = s_mock_ev_fd;
#endif
    char *p = (char *)out;
    size_t done = 0;
    while (done < sizeof(*out)) {
#ifndef DOOM_HOST_TEST
        long n = doom_syscall(SYS_READ, fd, (uintptr_t)(p + done), sizeof(*out) - done, 0, 0, 0);
#else
        ssize_t n = read(fd, p + done, sizeof(*out) - done);
#endif
        if (n == -22 /* EINTR */) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

/* Key translation helper */
static unsigned char translate_keycode(uint32_t keycode, uint32_t ascii) {
    switch (keycode) {
    case 103: return KEY_UPARROW;
    case 108: return KEY_DOWNARROW;
    case 105: return KEY_LEFTARROW;
    case 106: return KEY_RIGHTARROW;
    case 1:   return KEY_ESCAPE;
    case 28:  return KEY_ENTER;
    case 57:  return ' ';
    case 15:  return KEY_TAB;
    case 14:  return KEY_BACKSPACE;
    case 29:
    case 97:  return KEY_RCTRL;
    case 42:
    case 54:  return KEY_RSHIFT;
    case 56:
    case 100: return KEY_RALT;
    default:
        if (ascii >= 'a' && ascii <= 'z') return (unsigned char)ascii;
        if (ascii >= 'A' && ascii <= 'Z') return (unsigned char)(ascii + ('a' - 'A'));
        if (ascii >= '0' && ascii <= '9') return (unsigned char)ascii;
        if (ascii != 0 && ascii < 128) return (unsigned char)ascii;
        return 0;
    }
}

static void enqueue_key(int pressed, unsigned char key) {
    if (key == 0) return;
    unsigned next = (s_key_head + 1) % KEY_QUEUE_CAP;
    if (next != s_key_tail) {
        s_key_queue[s_key_head].pressed = pressed;
        s_key_queue[s_key_head].key = key;
        s_key_head = next;
    }
}

/* Event processing */
static void handle_desktop_event(const desktop_msg_t *msg) {
    switch (msg->kind) {
    case DESKTOP_EV_CONFIGURE: {
        uint32_t new_w = msg->a;
        uint32_t new_h = msg->b;
        uint32_t new_stride = msg->c;
        if ((uint64_t)new_stride * new_h * 4 > s_shm_size || new_w > new_stride) {
            return;
        }
        if (new_w != s_win_width || new_h != s_win_height || new_stride != s_win_stride) {
            s_win_width = new_w;
            s_win_height = new_h;
            s_win_stride = new_stride;

            /* Clear new window content region to opaque black */
            if (s_shm_pixels) {
                for (uint32_t y = 0; y < s_win_height; y++) {
                    uint32_t *row = &s_shm_pixels[y * s_win_stride];
                    for (uint32_t x = 0; x < s_win_width; x++) {
                        row[x] = 0xFF000000;
                    }
                }
                desktop_send(DESKTOP_REQ_DAMAGE, 0, 0, s_win_width, s_win_height, NULL);
            }
        }
        break;
    }
    case DESKTOP_EV_CLOSE:
        s_shutdown_requested = true;
        break;
    case DESKTOP_EV_KEY: {
        int pressed = (msg->d != DESKTOP_KEY_RELEASE) ? 1 : 0;
        unsigned char k = translate_keycode(msg->b, msg->a);
        enqueue_key(pressed, k);
        break;
    }
    default:
        break;
    }
}

bool fortress_desktop_poll_events(void) {
    if (!s_is_desktop) return true;

    int fd = DESKTOP_FD_EVENTS;
#ifdef DOOM_HOST_TEST
    if (s_mock_ev_fd >= 0) fd = s_mock_ev_fd;
#endif

    for (;;) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

#ifndef DOOM_HOST_TEST
        long r = doom_syscall(SYS_POLL, (uintptr_t)&pfd, 1, 0, 0, 0, 0);
#else
        int r = poll(&pfd, 1, 0);
#endif
        if (r < 0) {
            if (r == -22 /* EINTR */) continue;
            s_shutdown_requested = true;
            return false;
        }
        if (r == 0) {
            return true; /* no more pending events */
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            s_shutdown_requested = true;
            return false;
        }
        if (pfd.revents & POLLIN) {
            desktop_msg_t msg;
            char *p = (char *)&msg;
            size_t done = 0;
            while (done < sizeof(msg)) {
#ifndef DOOM_HOST_TEST
                long n = doom_syscall(SYS_READ, fd, (uintptr_t)(p + done), sizeof(msg) - done, 0, 0, 0);
#else
                ssize_t n = read(fd, p + done, sizeof(msg) - done);
#endif
                if (n == -22 /* EINTR */) continue;
                if (n <= 0) {
                    s_shutdown_requested = true;
                    return false;
                }
                done += (size_t)n;
            }
            handle_desktop_event(&msg);
        }
    }
}

void fortress_desktop_teardown(void) {
    if (s_shm_pixels) {
#ifndef DOOM_HOST_TEST
        doom_syscall(SYS_SHM_UNMAP, PIXELS_VA, 0, 0, 0, 0, 0);
#endif
        s_shm_pixels = NULL;
    }
    if (s_is_desktop) {
#ifndef DOOM_HOST_TEST
        doom_syscall(SYS_CLOSE, DESKTOP_FD_REQUESTS, 0, 0, 0, 0, 0);
        doom_syscall(SYS_CLOSE, DESKTOP_FD_EVENTS, 0, 0, 0, 0, 0);
        doom_syscall(SYS_CLOSE, DESKTOP_FD_PIXELS, 0, 0, 0, 0, 0);
#endif
        s_is_desktop = false;
    }
}

bool fortress_desktop_init(void) {
#ifdef DOOM_HOST_TEST
    if (s_mock_req_fd >= 0) {
        s_is_desktop = true;
        return true;
    }
#endif

#ifndef DOOM_HOST_TEST
    stat_ext_v1_t st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    st.version = 1;
    long r = doom_syscall(SYS_FSTAT, DESKTOP_FD_PIXELS, (uintptr_t)&st, 0, 0, 0, 0);
    if (r < 0 || st.file_size == 0) {
        printf("[FortressOS Doom] Desktop SHM not available (fd 5 stat=%ld). Running standalone/headless.\n", r);
        s_is_desktop = false;
        return true;
    }

    long va = doom_syscall(SYS_SHM_MAP, DESKTOP_FD_PIXELS, PIXELS_VA, 0, 0, 0, 0);
    if (va < 0) {
        printf("[FortressOS Doom] SYS_SHM_MAP failed: %ld\n", va);
        s_is_desktop = false;
        return false;
    }

    s_shm_pixels = (uint32_t *)(uintptr_t)PIXELS_VA;
    s_shm_size = st.file_size;
    s_is_desktop = true;

    /* Request window creation */
    if (!desktop_send(DESKTOP_REQ_CREATE, DOOM_OUTER_W, DOOM_OUTER_H, 0, 0, "Doom")) {
        printf("[FortressOS Doom] Failed to send REQ_CREATE\n");
        fortress_desktop_teardown();
        return false;
    }

    /* Read initial synchronous CONFIGURE event */
    desktop_msg_t conf;
    if (!desktop_receive_blocking(&conf) || conf.kind != DESKTOP_EV_CONFIGURE) {
        printf("[FortressOS Doom] Failed to receive initial CONFIGURE event\n");
        fortress_desktop_teardown();
        return false;
    }

    handle_desktop_event(&conf);
    printf("[FortressOS Doom] Desktop SHM mapped at 0x%llx, content=%ux%u stride=%u\n",
           (unsigned long long)PIXELS_VA, s_win_width, s_win_height, s_win_stride);
#endif
    return true;
}

/* doomgeneric platform hooks */

void DG_Init(void) {
    s_start_time_ms = fortress_get_time_ms();
    s_frame_count = 0;
    s_damage_count = 0;
    s_damaged_pixels = 0;
    I_AtExit(fortress_desktop_teardown, false);
    printf("[FortressOS Doom] DG_Init: ScreenBuffer=%p, Res=%dx%d\n",
           DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_DrawFrame(void) {
    s_frame_count++;
    if (!DG_ScreenBuffer) return;

    s_last_pixel = DG_ScreenBuffer[0];

    if (s_is_desktop && s_shm_pixels && s_win_width > 0 && s_win_height > 0) {
        uint32_t game_w = DOOMGENERIC_RESX;
        uint32_t game_h = DOOMGENERIC_RESY;

        /* Determine integer scaling factor */
        uint32_t scale = 1;
        if (s_win_width >= game_w * 2 && s_win_height >= game_h * 2) {
            scale = s_win_width / game_w;
            if (s_win_height / game_h < scale) {
                scale = s_win_height / game_h;
            }
            if (scale < 1) scale = 1;
            if (scale > 4) scale = 4;
        }
        s_current_scale = scale;

        uint32_t render_w = game_w * scale;
        uint32_t render_h = game_h * scale;

        if (render_w > s_win_width) render_w = s_win_width;
        if (render_h > s_win_height) render_h = s_win_height;

        /* Center within window content area (letterboxing) */
        uint32_t ox = (s_win_width > render_w) ? (s_win_width - render_w) / 2 : 0;
        uint32_t oy = (s_win_height > render_h) ? (s_win_height - render_h) / 2 : 0;

        /* Safety guard against buffer overrun */
        if ((uint64_t)(oy + render_h) * s_win_stride * 4 <= s_shm_size) {
            if (scale == 1) {
                uint32_t blit_w = (render_w < game_w) ? render_w : game_w;
                uint32_t blit_h = (render_h < game_h) ? render_h : game_h;
                for (uint32_t y = 0; y < blit_h; y++) {
                    uint32_t *dst = &s_shm_pixels[(oy + y) * s_win_stride + ox];
                    const uint32_t *src = &DG_ScreenBuffer[y * game_w];
                    for (uint32_t x = 0; x < blit_w; x++) {
                        dst[x] = src[x] | 0xFF000000;
                    }
                }
            } else {
                uint32_t eff_w = (render_w / scale < game_w) ? (render_w / scale) : game_w;
                uint32_t eff_h = (render_h / scale < game_h) ? (render_h / scale) : game_h;
                for (uint32_t y = 0; y < eff_h; y++) {
                    const uint32_t *src = &DG_ScreenBuffer[y * game_w];
                    for (uint32_t sy = 0; sy < scale; sy++) {
                        uint32_t dst_y = oy + y * scale + sy;
                        if (dst_y >= s_win_height) break;
                        uint32_t *dst = &s_shm_pixels[dst_y * s_win_stride + ox];
                        for (uint32_t x = 0; x < eff_w; x++) {
                            uint32_t p = src[x] | 0xFF000000;
                            for (uint32_t sx = 0; sx < scale; sx++) {
                                dst[x * scale + sx] = p;
                            }
                        }
                    }
                }
            }

            /* Submit bounded damage message */
            if (!desktop_send(DESKTOP_REQ_DAMAGE, ox, oy, render_w, render_h, NULL)) {
                s_shutdown_requested = true;
            } else {
                s_damage_count++;
                s_damaged_pixels += (uint64_t)render_w * render_h;
            }
        }
    }

    if (s_target_frames > 0 && (int)s_frame_count >= s_target_frames) {
        s_shutdown_requested = true;
    }
}

void DG_SleepMs(uint32_t ms) {
    if (ms == 0) return;
#ifndef DOOM_HOST_TEST
    timespec_internal_t req = {
        .tv_sec = (int64_t)(ms / 1000),
        .tv_nsec = (int64_t)((ms % 1000) * 1000000ULL),
    };
    doom_syscall(SYS_NANOSLEEP, (uintptr_t)&req, 0, 0, 0, 0, 0);
#else
    struct timespec req = {
        .tv_sec = (time_t)(ms / 1000),
        .tv_nsec = (long)((ms % 1000) * 1000000L),
    };
    nanosleep(&req, NULL);
#endif
}

uint32_t DG_GetTicksMs(void) {
    uint64_t now = fortress_get_time_ms();
    return (uint32_t)(now - s_start_time_ms);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    fortress_desktop_poll_events();
    if (s_key_head != s_key_tail) {
        if (pressed) *pressed = s_key_queue[s_key_tail].pressed;
        if (key) *key = s_key_queue[s_key_tail].key;
        s_key_tail = (s_key_tail + 1) % KEY_QUEUE_CAP;
        return 1;
    }
    return 0;
}

void DG_SetWindowTitle(const char *title) {
    if (s_is_desktop && title) {
        desktop_send(DESKTOP_REQ_TITLE, 0, 0, 0, 0, title);
    }
}

/* Inspection and stats APIs */
uint64_t fortress_doom_get_frame_count(void) {
    return s_frame_count;
}

uint32_t fortress_doom_get_last_pixel(void) {
    return s_last_pixel;
}

void fortress_desktop_get_stats(fortress_desktop_stats_t *out) {
    if (!out) return;
    out->frame_count = s_frame_count;
    out->damage_count = s_damage_count;
    out->damaged_pixels = s_damaged_pixels;
    out->win_width = s_win_width;
    out->win_height = s_win_height;
    out->win_stride = s_win_stride;
    out->scale = s_current_scale;
    out->is_desktop = s_is_desktop;
}

void fortress_desktop_set_target_frames(int frames) {
    s_target_frames = frames;
}

#ifdef DOOM_HOST_TEST
void fortress_desktop_set_mock(int req_fd, int ev_fd, uint32_t *pixels, uint64_t size, uint32_t stride) {
    s_mock_req_fd = req_fd;
    s_mock_ev_fd = ev_fd;
    s_shm_pixels = pixels;
    s_shm_size = size;
    s_win_stride = stride;
    s_is_desktop = (req_fd >= 0 && ev_fd >= 0 && pixels != NULL);
    s_shutdown_requested = false;
}

void fortress_desktop_reset(void) {
    s_is_desktop = false;
    s_shm_pixels = NULL;
    s_shm_size = 0;
    s_win_width = 0;
    s_win_height = 0;
    s_win_stride = 0;
    s_current_scale = 1;
    s_shutdown_requested = false;
    s_target_frames = -1;
    s_frame_count = 0;
    s_damage_count = 0;
    s_damaged_pixels = 0;
    s_key_head = 0;
    s_key_tail = 0;
    s_mock_req_fd = -1;
    s_mock_ev_fd = -1;
}
#endif

int doom_main(int argc, char **argv) {
    printf("Starting Doom on FortressOS...\n");
    doom_runtime_init();

    /* Check for explicit -frames limit (for headless/testing runs) */
    s_target_frames = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            s_target_frames = atoi(argv[i + 1]);
            printf("[FortressOS Doom] Target frames limit: %d\n", s_target_frames);
        }
    }

    fortress_desktop_init();

    doomgeneric_Create(argc, argv);

    /* Main execution loop */
    while (!s_shutdown_requested) {
        fortress_desktop_poll_events();
        if (s_shutdown_requested) break;
        doomgeneric_Tick();
    }

    printf("[FortressOS Doom] Main loop finished (frames=%llu, damage_msgs=%llu).\n",
           (unsigned long long)s_frame_count, (unsigned long long)s_damage_count);

    I_Quit();
    fortress_desktop_teardown();
    return 0;
}
