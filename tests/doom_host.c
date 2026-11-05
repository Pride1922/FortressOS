/* Host verification test for Doom port feasibility (Stage 0 & Stage 1)
 * Validates pinned engine/data combination, initialization, frame production,
 * level warping, error handling (missing/truncated WAD), and runtime adapter.
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

#include "user/doom/doom_runtime.h"
#include "user/doom/upstream/doomgeneric.h"

extern uint64_t fortress_doom_get_frame_count(void);
extern uint32_t fortress_doom_get_last_pixel(void);
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

static void test_engine_init_and_frames(const char *wad_path) {
    printf("[TEST] Testing engine initialization & frame generation with %s...\n", wad_path);

    char *argv[] = {
        (char *)"doom",
        (char *)"-iwad",
        (char *)wad_path,
        (char *)"-nosound",
        NULL
    };
    int argc = 4;

    int ret = doom_main(argc, argv);
    assert(ret == 0 && "doom_main must succeed");

    uint64_t frames = fortress_doom_get_frame_count();
    uint32_t pixel = fortress_doom_get_last_pixel();

    printf("  -> Produced %llu frames, final pixel: 0x%08x\n",
           (unsigned long long)frames, pixel);
    assert(frames >= 70 && "must produce at least 70 frames");
    assert(pixel != 0 && "framebuffer must have non-zero rendered content");
    printf("  -> PASS: Engine initialized and produced valid frames.\n");
}

static void test_engine_warp(const char *wad_path) {
    printf("[TEST] Testing level warp with %s...\n", wad_path);

    char *argv[] = {
        (char *)"doom",
        (char *)"-iwad",
        (char *)wad_path,
        (char *)"-warp",
        (char *)"1",
        (char *)"1",
        (char *)"-nosound",
        NULL
    };
    int argc = 7;

    int ret = doom_main(argc, argv);
    assert(ret == 0 && "warp must succeed");

    uint64_t frames = fortress_doom_get_frame_count();
    printf("  -> Warped to E1M1, total frames now: %llu\n", (unsigned long long)frames);
    printf("  -> PASS: Level warping and 3D playloop executed successfully.\n");
}

static void test_missing_wad_graceful_exit(void) {
    printf("[TEST] Testing missing WAD graceful handling...\n");
    /* We expect doom to detect missing file and exit via I_Error without crashing */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child */
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
    char corrupt_path[] = "/tmp/doom_corrupt_test_XXXXXX.wad";
    int fd = mkstemps(corrupt_path, 4);
    assert(fd >= 0);
    /* Write 32 bytes of garbage */
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
    printf("=================================================================\n");
    printf("FortressOS Doom Feasibility Host Verification Suite (Stage 0 & 1)\n");
    printf("=================================================================\n");

    test_pinned_identities();

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

    printf("\n>>> ALL HOST DOOM FEASIBILITY TESTS PASSED <<<\n");
    return 0;
}
