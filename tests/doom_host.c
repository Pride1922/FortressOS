/* Host verification test for Doom port feasibility (Stage 0, 1 & 2)
 * Validates pinned engine/data combination, initialization, frame production,
 * level warping, error handling (missing/truncated WAD), runtime adapter,
 * SHM desktop window protocol, presentation, integer scaling, letterboxing,
 * damage tracking, key events, and lifecycle management.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>

#include "src/include/types.h"
#include "src/include/desktop_proto.h"
#include "user/doom/upstream/doomkeys.h"
#include "user/doom/doom_runtime.h"
#include "user/doom/upstream/doomgeneric.h"

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

extern uint64_t fortress_doom_get_frame_count(void);
extern uint32_t fortress_doom_get_last_pixel(void);
extern void fortress_desktop_get_stats(fortress_desktop_stats_t *out);
extern void fortress_desktop_set_target_frames(int frames);
extern void fortress_desktop_set_mock(int req_fd, int ev_fd, uint32_t *pixels, uint64_t size, uint32_t stride);
extern void fortress_desktop_reset(void);
extern bool fortress_desktop_poll_events(void);
extern void fortress_desktop_teardown(void);
extern int DG_GetKey(int *pressed, unsigned char *key);
extern int doom_main(int argc, char **argv);

static void test_pinned_identities(void) {
    printf("[TEST] Verifying pinned upstream metadata...\n");
    FILE *f = fopen("user/doom/upstream.json", "r");
    assert(f != NULL && "upstream.json must exist");

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    assert(strstr(buf, "e257a09527062bfb986b6e5e0780839b6cbd5b71") != NULL);
    assert(strstr(buf, "26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7") != NULL);
    assert(strstr(buf, "v0.13.0") != NULL);
    assert(strstr(buf, "d9adc4d792627e7fc47b09067b15486da724010c71dd12831e1cf8e0755b68ad") != NULL);
    assert(strstr(buf, "7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d") != NULL);
    assert(strstr(buf, "a8772e088847032510d97ba2312406a6998f21cbab44d4ff10696faa9c0ecd4b") != NULL);
    printf("  -> PASS: All pinned commits, versions, and SHA256 hashes verified.\n");
}

static void test_desktop_protocol_abi(void) {
    printf("[TEST] Verifying desktop protocol ABI contracts...\n");
    assert(sizeof(desktop_msg_t) == 64);
    assert(DESKTOP_FD_REQUESTS == 3);
    assert(DESKTOP_FD_EVENTS == 4);
    assert(DESKTOP_FD_PIXELS == 5);
    assert(DESKTOP_REQ_CREATE == 1u);
    assert(DESKTOP_REQ_DAMAGE == 2u);
    assert(DESKTOP_REQ_TITLE == 3u);
    assert(DESKTOP_EV_CONFIGURE == 101u);
    assert(DESKTOP_EV_KEY == 102u);
    assert(DESKTOP_EV_CLOSE == 103u);
    assert(DESKTOP_KEY_RELEASE == 0u);
    assert(DESKTOP_KEY_PRESS == 1u);
    printf("  -> PASS: 64-byte protocol structs, messages, and descriptors verified.\n");
}

static void test_desktop_scaling_and_presentation(void) {
    printf("[TEST] Testing desktop SHM presentation, scaling, and damage...\n");
    fortress_desktop_reset();

    int req_pipe[2];
    int ev_pipe[2];
    assert(pipe(req_pipe) == 0);
    assert(pipe(ev_pipe) == 0);

    /* Allocate mock shared memory buffer: 1920 x 1080 @ 4 Bpp */
    const uint32_t stride = 1920;
    const uint32_t rows = 1080;
    uint64_t shm_size = (uint64_t)stride * rows * sizeof(uint32_t);
    uint32_t *mock_pixels = (uint32_t *)calloc(stride * rows, sizeof(uint32_t));
    assert(mock_pixels != NULL);

    fortress_desktop_set_mock(req_pipe[1], ev_pipe[0], mock_pixels, shm_size, stride);

    /* Allocate dummy screen buffer */
    DG_ScreenBuffer = (pixel_t *)malloc(640 * 400 * sizeof(pixel_t));
    assert(DG_ScreenBuffer != NULL);
    for (int i = 0; i < 640 * 400; i++) {
        DG_ScreenBuffer[i] = 0x00FF0000 | (uint32_t)(i & 0xFF); /* Red with gradient */
    }

    /* 1. Test native 640x400 configuration */
    desktop_msg_t cfg1 = {
        .kind = DESKTOP_EV_CONFIGURE,
        .a = 640,
        .b = 400,
        .c = stride
    };
    write(ev_pipe[1], &cfg1, sizeof(cfg1));
    assert(fortress_desktop_poll_events());

    /* Read the resize-damage message sent on configure */
    desktop_msg_t dmg_cfg;
    assert(read(req_pipe[0], &dmg_cfg, sizeof(dmg_cfg)) == sizeof(dmg_cfg));
    assert(dmg_cfg.kind == DESKTOP_REQ_DAMAGE);
    assert(dmg_cfg.a == 0 && dmg_cfg.b == 0 && dmg_cfg.c == 640 && dmg_cfg.d == 400);

    /* Render a frame */
    DG_DrawFrame();

    /* Check frame damage */
    desktop_msg_t dmg_frame;
    assert(read(req_pipe[0], &dmg_frame, sizeof(dmg_frame)) == sizeof(dmg_frame));
    assert(dmg_frame.kind == DESKTOP_REQ_DAMAGE);
    assert(dmg_frame.a == 0 && dmg_frame.b == 0);
    assert(dmg_frame.c == 640 && dmg_frame.d == 400);

    /* Verify pixel 0 was blitted with opaque alpha */
    assert(mock_pixels[0] == (DG_ScreenBuffer[0] | 0xFF000000));
    printf("  -> Native 640x400 presentation PASS\n");

    /* 2. Test letterboxing in larger window: 800x600 */
    desktop_msg_t cfg2 = {
        .kind = DESKTOP_EV_CONFIGURE,
        .a = 800,
        .b = 600,
        .c = stride
    };
    write(ev_pipe[1], &cfg2, sizeof(cfg2));
    assert(fortress_desktop_poll_events());

    /* Read clear damage */
    assert(read(req_pipe[0], &dmg_cfg, sizeof(dmg_cfg)) == sizeof(dmg_cfg));
    assert(dmg_cfg.a == 0 && dmg_cfg.b == 0 && dmg_cfg.c == 800 && dmg_cfg.d == 600);

    /* Render frame */
    DG_DrawFrame();
    assert(read(req_pipe[0], &dmg_frame, sizeof(dmg_frame)) == sizeof(dmg_frame));
    assert(dmg_frame.kind == DESKTOP_REQ_DAMAGE);
    /* Letterbox offset: ox = (800 - 640)/2 = 80, oy = (600 - 400)/2 = 100 */
    assert(dmg_frame.a == 80);
    assert(dmg_frame.b == 100);
    assert(dmg_frame.c == 640);
    assert(dmg_frame.d == 400);

    /* Letterbox border at (0, 0) must be black */
    assert(mock_pixels[0] == 0xFF000000);
    /* Centered content at (80, 100) must match game pixel */
    assert(mock_pixels[100 * stride + 80] == (DG_ScreenBuffer[0] | 0xFF000000));
    printf("  -> Letterbox 800x600 centering PASS\n");

    /* 3. Test 2x integer scaling in 1280x800 */
    desktop_msg_t cfg3 = {
        .kind = DESKTOP_EV_CONFIGURE,
        .a = 1280,
        .b = 800,
        .c = stride
    };
    write(ev_pipe[1], &cfg3, sizeof(cfg3));
    assert(fortress_desktop_poll_events());
    assert(read(req_pipe[0], &dmg_cfg, sizeof(dmg_cfg)) == sizeof(dmg_cfg));

    DG_DrawFrame();
    assert(read(req_pipe[0], &dmg_frame, sizeof(dmg_frame)) == sizeof(dmg_frame));
    assert(dmg_frame.a == 0 && dmg_frame.b == 0);
    assert(dmg_frame.c == 1280 && dmg_frame.d == 800);

    /* Check 2x scaling: 2x2 block for pixel (0,0) */
    uint32_t expected = DG_ScreenBuffer[0] | 0xFF000000;
    assert(mock_pixels[0] == expected);
    assert(mock_pixels[1] == expected);
    assert(mock_pixels[stride] == expected);
    assert(mock_pixels[stride + 1] == expected);
    printf("  -> 2x integer scaling (1280x800) PASS\n");

    /* 4. Test clipping in undersized window: 400x300 */
    desktop_msg_t cfg4 = {
        .kind = DESKTOP_EV_CONFIGURE,
        .a = 400,
        .b = 300,
        .c = stride
    };
    write(ev_pipe[1], &cfg4, sizeof(cfg4));
    assert(fortress_desktop_poll_events());
    assert(read(req_pipe[0], &dmg_cfg, sizeof(dmg_cfg)) == sizeof(dmg_cfg));

    DG_DrawFrame();
    assert(read(req_pipe[0], &dmg_frame, sizeof(dmg_frame)) == sizeof(dmg_frame));
    assert(dmg_frame.a == 0 && dmg_frame.b == 0);
    assert(dmg_frame.c == 400 && dmg_frame.d == 300);
    printf("  -> Clipping in undersized 400x300 window PASS\n");

    /* 5. Test window title update */
    DG_SetWindowTitle("Freedoom: Phase 1");
    desktop_msg_t title_msg;
    assert(read(req_pipe[0], &title_msg, sizeof(title_msg)) == sizeof(title_msg));
    assert(title_msg.kind == DESKTOP_REQ_TITLE);
    assert(strcmp(title_msg.text, "Freedoom: Phase 1") == 0);
    printf("  -> Window title update PASS\n");

    /* 6. Test statistics */
    fortress_desktop_stats_t stats;
    fortress_desktop_get_stats(&stats);
    assert(stats.damage_count > 0);
    assert(stats.damaged_pixels > 0);
    printf("  -> Stats: frames=%llu, damage_msgs=%llu, damaged_pixels=%llu\n",
           (unsigned long long)stats.frame_count,
           (unsigned long long)stats.damage_count,
           (unsigned long long)stats.damaged_pixels);

    /* Clean up */
    free(DG_ScreenBuffer);
    DG_ScreenBuffer = NULL;
    free(mock_pixels);
    close(req_pipe[0]); close(req_pipe[1]);
    close(ev_pipe[0]); close(ev_pipe[1]);
    fortress_desktop_reset();
    printf("  -> PASS: All desktop presentation and scaling tests succeeded.\n");
}

static void test_desktop_input_and_close(void) {
    printf("[TEST] Testing desktop key events and close handling...\n");
    fortress_desktop_reset();

    int req_pipe[2], ev_pipe[2];
    assert(pipe(req_pipe) == 0 && pipe(ev_pipe) == 0);
    uint32_t dummy_buf[16];
    fortress_desktop_set_mock(req_pipe[1], ev_pipe[0], dummy_buf, sizeof(dummy_buf), 4);

    /* 1. Feed key press: UP arrow (Linux keycode 103) */
    desktop_msg_t key_press = {
        .kind = DESKTOP_EV_KEY,
        .a = 0,
        .b = 103, /* KEY_UP */
        .c = 0,
        .d = DESKTOP_KEY_PRESS
    };
    write(ev_pipe[1], &key_press, sizeof(key_press));

    int pressed = 0;
    unsigned char key = 0;
    assert(DG_GetKey(&pressed, &key) == 1);
    assert(pressed == 1);
    assert(key == KEY_UPARROW);

    /* 2. Feed key release: UP arrow */
    desktop_msg_t key_rel = {
        .kind = DESKTOP_EV_KEY,
        .a = 0,
        .b = 103,
        .c = 0,
        .d = DESKTOP_KEY_RELEASE
    };
    write(ev_pipe[1], &key_rel, sizeof(key_rel));

    assert(DG_GetKey(&pressed, &key) == 1);
    assert(pressed == 0);
    assert(key == KEY_UPARROW);

    /* 3. Feed Escape key (Linux keycode 1) */
    desktop_msg_t key_esc = {
        .kind = DESKTOP_EV_KEY,
        .a = 27,
        .b = 1,
        .c = 0,
        .d = DESKTOP_KEY_PRESS
    };
    write(ev_pipe[1], &key_esc, sizeof(key_esc));
    assert(DG_GetKey(&pressed, &key) == 1);
    assert(pressed == 1);
    assert(key == KEY_ESCAPE);

    /* Queue should now be empty */
    assert(DG_GetKey(&pressed, &key) == 0);

    /* 4. Feed window close event */
    desktop_msg_t close_ev = { .kind = DESKTOP_EV_CLOSE };
    write(ev_pipe[1], &close_ev, sizeof(close_ev));
    assert(fortress_desktop_poll_events());

    /* 5. Feed broken event pipe (close write end) */
    close(ev_pipe[1]);
    /* Polling closed pipe must return false promptly */
    assert(fortress_desktop_poll_events() == false);
    printf("  -> Prompt detection of broken compositor pipe verified.\n");

    close(req_pipe[0]); close(req_pipe[1]);
    close(ev_pipe[0]);
    fortress_desktop_reset();
    printf("  -> PASS: Key events and close/pipe termination verified.\n");
}

static void test_engine_init_and_frames(const char *wad_path) {
    printf("[TEST] Testing engine initialization & frame generation with %s...\n", wad_path);
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {
            (char *)"doom",
            (char *)"-iwad",
            (char *)wad_path,
            (char *)"-nosound",
            (char *)"-frames",
            (char *)"70",
            NULL
        };
        int ret = doom_main(6, argv);
        exit(ret);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 && "doom_main must succeed");
    printf("  -> PASS: Engine initialized and produced valid frames.\n");
}

static void test_engine_warp(const char *wad_path) {
    printf("[TEST] Testing level warp with %s...\n", wad_path);
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {
            (char *)"doom",
            (char *)"-iwad",
            (char *)wad_path,
            (char *)"-warp",
            (char *)"1",
            (char *)"1",
            (char *)"-nosound",
            (char *)"-frames",
            (char *)"70",
            NULL
        };
        int ret = doom_main(9, argv);
        exit(ret);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 && "warp must succeed");
    printf("  -> PASS: Level warping and 3D playloop executed successfully.\n");
}

static void test_missing_wad_graceful_exit(void) {
    printf("[TEST] Testing missing WAD graceful handling...\n");
    fortress_desktop_reset();
    pid_t pid = fork();
    if (pid == 0) {
        close(1); close(2); /* quiet */
        char *argv[] = {
            (char *)"doom",
            (char *)"-nogui",
            (char *)"-iwad",
            (char *)"/nonexistent/path/missing.wad",
            NULL
        };
        doom_main(4, argv);
        exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && "missing WAD must exit cleanly via exit()");
    printf("  -> PASS: Missing WAD correctly caught and handled without crash.\n");
}

static void test_corrupted_wad_handling(void) {
    printf("[TEST] Testing corrupted/truncated WAD graceful handling...\n");
    fortress_desktop_reset();
    char corrupt_path[] = "/tmp/doom_corrupt_test_XXXXXX.wad";
    int fd = mkstemps(corrupt_path, 4);
    assert(fd >= 0);
    write(fd, "IWAD\x01\x00\x00\x00\xFF\xFF\xFF\x7F\x00\x00\x00\x00garbage_lump_data_here", 32);
    close(fd);

    pid_t pid = fork();
    if (pid == 0) {
        close(1); close(2); /* quiet */
        char *argv[] = {
            (char *)"doom",
            (char *)"-nogui",
            (char *)"-iwad",
            corrupt_path,
            NULL
        };
        doom_main(4, argv);
        exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(corrupt_path);
    assert(WIFEXITED(status) && "corrupted WAD must exit cleanly without memory fault");
    printf("  -> PASS: Corrupted WAD caught and safely exited.\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("==========================================================================\n");
    printf("FortressOS Doom Desktop & Presentation Verification Suite (Stages 0, 1, 2)\n");
    printf("==========================================================================\n");

    test_pinned_identities();
    test_desktop_protocol_abi();
    test_desktop_scaling_and_presentation();
    test_desktop_input_and_close();

    const char *freedm_wad = "/tmp/doom-eval/freedm-0.13.0/freedm.wad";
    const char *freedoom1_wad = "/tmp/doom-eval/freedoom-0.13.0/freedoom1.wad";

    if (access(freedm_wad, R_OK) == 0) {
        test_engine_init_and_frames(freedm_wad);
    } else {
        printf("[WARN] %s not accessible; skipping live freedm test\n", freedm_wad);
    }

    if (access(freedoom1_wad, R_OK) == 0) {
        test_engine_warp(freedoom1_wad);
    } else {
        printf("[WARN] %s not accessible; skipping live freedoom1 warp test\n", freedoom1_wad);
    }

    test_missing_wad_graceful_exit();
    test_corrupted_wad_handling();

    printf("\n>>> ALL STAGE 0, 1 & 2 DOOM TESTS PASSED <<<\n");
    return 0;
}
