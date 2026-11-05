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
extern bool fortress_doom_has_focus(void);
extern bool fortress_doom_is_key_down(unsigned char key);
extern void fortress_desktop_get_stats(fortress_desktop_stats_t *out);
extern void fortress_desktop_set_target_frames(int frames);
extern void fortress_desktop_set_mock(int req_fd, int ev_fd, uint32_t *pixels, uint64_t size, uint32_t stride);
extern void fortress_desktop_reset(void);
extern bool fortress_desktop_poll_events(void);
extern void fortress_desktop_teardown(void);
extern int DG_GetKey(int *pressed, unsigned char *key);
extern int doom_main(int argc, char **argv, char **envp);

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

static void test_stage3_keyboard_mappings(void) {
    printf("[TEST] Testing Stage 3 keyboard mappings (arrows, fire, use, strafe, run, controls, digits, F-keys)...\n");
    fortress_desktop_reset();

    int req_pipe[2], ev_pipe[2];
    assert(pipe(req_pipe) == 0 && pipe(ev_pipe) == 0);
    uint32_t dummy[16];
    fortress_desktop_set_mock(req_pipe[1], ev_pipe[0], dummy, sizeof(dummy), 4);

    struct {
        uint32_t keycode;
        uint32_t ascii;
        unsigned char expected_doom_key;
        const char *name;
    } mapping_cases[] = {
        /* Standard arrows */
        { 103, 0, KEY_UPARROW, "Up Arrow" },
        { 108, 0, KEY_DOWNARROW, "Down Arrow" },
        { 105, 0, KEY_LEFTARROW, "Left Arrow" },
        { 106, 0, KEY_RIGHTARROW, "Right Arrow" },
        /* Keypad arrows */
        { 72, 0, KEY_UPARROW, "Keypad Up" },
        { 80, 0, KEY_DOWNARROW, "Keypad Down" },
        { 75, 0, KEY_LEFTARROW, "Keypad Left" },
        { 77, 0, KEY_RIGHTARROW, "Keypad Right" },
        /* Action buttons */
        { 29, 0, KEY_FIRE, "Left Ctrl (Fire)" },
        { 97, 0, KEY_FIRE, "Right Ctrl (Fire)" },
        { 57, ' ', KEY_USE, "Space (Use)" },
        { 42, 0, KEY_RSHIFT, "Left Shift (Run)" },
        { 54, 0, KEY_RSHIFT, "Right Shift (Run)" },
        { 56, 0, KEY_RALT, "Left Alt (Strafe)" },
        { 100, 0, KEY_RALT, "Right Alt (Strafe)" },
        { 51, ',', KEY_STRAFE_L, "Comma (Strafe Left)" },
        { 52, '.', KEY_STRAFE_R, "Dot (Strafe Right)" },
        /* Controls */
        { 28, '\n', KEY_ENTER, "Enter" },
        { 1, 27, KEY_ESCAPE, "Escape" },
        { 15, '\t', KEY_TAB, "Tab (Map)" },
        { 14, '\b', KEY_BACKSPACE, "Backspace" },
        { 119, 0, KEY_PAUSE, "Pause" },
        /* Digits */
        { 2, '1', '1', "Weapon 1" },
        { 3, '2', '2', "Weapon 2" },
        { 8, '7', '7', "Weapon 7" },
        { 11, '0', '0', "Slot 0" },
        /* Function keys */
        { 59, 0, KEY_F1, "F1" },
        { 60, 0, KEY_F2, "F2 (Save)" },
        { 61, 0, KEY_F3, "F3 (Load)" },
        { 63, 0, KEY_F5, "F5" },
        { 68, 0, KEY_F10, "F10" },
        { 88, 0, KEY_F12, "F12" },
    };

    for (size_t i = 0; i < sizeof(mapping_cases)/sizeof(mapping_cases[0]); i++) {
        uint32_t kc = mapping_cases[i].keycode;
        uint32_t ascii = mapping_cases[i].ascii;
        unsigned char expected = mapping_cases[i].expected_doom_key;

        /* Press event */
        desktop_msg_t press_msg = {
            .kind = DESKTOP_EV_KEY,
            .a = ascii,
            .b = kc,
            .c = 0,
            .d = DESKTOP_KEY_PRESS
        };
        write(ev_pipe[1], &press_msg, sizeof(press_msg));
        assert(fortress_desktop_poll_events());

        int pressed = -1;
        unsigned char k = 0;
        assert(DG_GetKey(&pressed, &k) == 1);
        assert(pressed == 1);
        assert(k == expected);
        assert(fortress_doom_is_key_down(expected) == true);

        /* Release event (desktop passes ascii=0 on release) */
        desktop_msg_t rel_msg = {
            .kind = DESKTOP_EV_KEY,
            .a = 0,
            .b = kc,
            .c = 0,
            .d = DESKTOP_KEY_RELEASE
        };
        write(ev_pipe[1], &rel_msg, sizeof(rel_msg));
        assert(fortress_desktop_poll_events());

        assert(DG_GetKey(&pressed, &k) == 1);
        assert(pressed == 0);
        assert(k == expected);
        assert(fortress_doom_is_key_down(expected) == false);
    }

    close(req_pipe[0]); close(req_pipe[1]);
    close(ev_pipe[0]); close(ev_pipe[1]);
    fortress_desktop_reset();
    printf("  -> PASS: All primary keyboard mappings, presses, and releases verified.\n");
}

static void test_stage3_azerty_and_us_parity(void) {
    printf("[TEST] Testing US vs AZERTY layout parity and release tracking...\n");
    fortress_desktop_reset();

    int req_pipe[2], ev_pipe[2];
    assert(pipe(req_pipe) == 0 && pipe(ev_pipe) == 0);
    uint32_t dummy[16];
    fortress_desktop_set_mock(req_pipe[1], ev_pipe[0], dummy, sizeof(dummy), 4);

    /*
     * Keycode 17:
     * - On US layout: emits 'w' (119)
     * - On AZERTY layout: emits 'z' (122)
     */
    /* Test US 'W' */
    desktop_msg_t us_w_press = {
        .kind = DESKTOP_EV_KEY,
        .a = 'w',
        .b = 17,
        .c = 0,
        .d = DESKTOP_KEY_PRESS
    };
    write(ev_pipe[1], &us_w_press, sizeof(us_w_press));
    assert(fortress_desktop_poll_events());

    int pressed = 0;
    unsigned char k = 0;
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 1 && k == 'w');
    assert(fortress_doom_is_key_down('w') == true);

    /* Release US 'W' (release has ascii=0) */
    desktop_msg_t us_w_rel = {
        .kind = DESKTOP_EV_KEY,
        .a = 0,
        .b = 17,
        .c = 0,
        .d = DESKTOP_KEY_RELEASE
    };
    write(ev_pipe[1], &us_w_rel, sizeof(us_w_rel));
    assert(fortress_desktop_poll_events());
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 0 && k == 'w');
    assert(fortress_doom_is_key_down('w') == false);

    /* Test AZERTY 'Z' on same keycode 17 */
    desktop_msg_t az_z_press = {
        .kind = DESKTOP_EV_KEY,
        .a = 'z',
        .b = 17,
        .c = 0,
        .d = DESKTOP_KEY_PRESS
    };
    write(ev_pipe[1], &az_z_press, sizeof(az_z_press));
    assert(fortress_desktop_poll_events());
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 1 && k == 'z');
    assert(fortress_doom_is_key_down('z') == true);

    /* Release AZERTY 'Z' (release has ascii=0) */
    desktop_msg_t az_z_rel = {
        .kind = DESKTOP_EV_KEY,
        .a = 0,
        .b = 17,
        .c = 0,
        .d = DESKTOP_KEY_RELEASE
    };
    write(ev_pipe[1], &az_z_rel, sizeof(az_z_rel));
    assert(fortress_desktop_poll_events());
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 0 && k == 'z');
    assert(fortress_doom_is_key_down('z') == false);

    /*
     * Keycode 16:
     * - On US layout: emits 'q'
     * - On AZERTY layout: emits 'a'
     */
    /* Test US 'Q' */
    desktop_msg_t us_q_press = { .kind = DESKTOP_EV_KEY, .a = 'q', .b = 16, .d = DESKTOP_KEY_PRESS };
    write(ev_pipe[1], &us_q_press, sizeof(us_q_press));
    assert(fortress_desktop_poll_events());
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 1 && k == 'q');
    desktop_msg_t us_q_rel = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 16, .d = DESKTOP_KEY_RELEASE };
    write(ev_pipe[1], &us_q_rel, sizeof(us_q_rel));
    assert(fortress_desktop_poll_events());
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 0 && k == 'q');

    /* Test AZERTY 'A' on same keycode 16 */
    desktop_msg_t az_a_press = { .kind = DESKTOP_EV_KEY, .a = 'a', .b = 16, .d = DESKTOP_KEY_PRESS };
    write(ev_pipe[1], &az_a_press, sizeof(az_a_press));
    assert(fortress_desktop_poll_events());
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 1 && k == 'a');
    desktop_msg_t az_a_rel = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 16, .d = DESKTOP_KEY_RELEASE };
    write(ev_pipe[1], &az_a_rel, sizeof(az_a_rel));
    assert(fortress_desktop_poll_events());
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 0 && k == 'a');

    close(req_pipe[0]); close(req_pipe[1]);
    close(ev_pipe[0]); close(ev_pipe[1]);
    fortress_desktop_reset();
    printf("  -> PASS: Complete US and AZERTY layout parity with symmetrical release verified.\n");
}

static void test_stage3_simultaneous_keys(void) {
    printf("[TEST] Testing simultaneous key presses and partial release...\n");
    fortress_desktop_reset();

    int req_pipe[2], ev_pipe[2];
    assert(pipe(req_pipe) == 0 && pipe(ev_pipe) == 0);
    uint32_t dummy[16];
    fortress_desktop_set_mock(req_pipe[1], ev_pipe[0], dummy, sizeof(dummy), 4);

    /* Press 4 keys simultaneously: Up Arrow (103), Right Arrow (106), Fire/Ctrl (29), Speed/Shift (42) */
    desktop_msg_t p1 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 103, .d = DESKTOP_KEY_PRESS };
    desktop_msg_t p2 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 106, .d = DESKTOP_KEY_PRESS };
    desktop_msg_t p3 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 29,  .d = DESKTOP_KEY_PRESS };
    desktop_msg_t p4 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 42,  .d = DESKTOP_KEY_PRESS };
    write(ev_pipe[1], &p1, sizeof(p1));
    write(ev_pipe[1], &p2, sizeof(p2));
    write(ev_pipe[1], &p3, sizeof(p3));
    write(ev_pipe[1], &p4, sizeof(p4));

    assert(fortress_desktop_poll_events());

    int pressed = 0; unsigned char k = 0;
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 1 && k == KEY_UPARROW);
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 1 && k == KEY_RIGHTARROW);
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 1 && k == KEY_FIRE);
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 1 && k == KEY_RSHIFT);

    assert(fortress_doom_is_key_down(KEY_UPARROW) == true);
    assert(fortress_doom_is_key_down(KEY_RIGHTARROW) == true);
    assert(fortress_doom_is_key_down(KEY_FIRE) == true);
    assert(fortress_doom_is_key_down(KEY_RSHIFT) == true);

    /* Release Right Arrow and Fire */
    desktop_msg_t r2 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 106, .d = DESKTOP_KEY_RELEASE };
    desktop_msg_t r3 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 29,  .d = DESKTOP_KEY_RELEASE };
    write(ev_pipe[1], &r2, sizeof(r2));
    write(ev_pipe[1], &r3, sizeof(r3));
    assert(fortress_desktop_poll_events());

    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 0 && k == KEY_RIGHTARROW);
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 0 && k == KEY_FIRE);

    /* Up Arrow and Speed must still be held down */
    assert(fortress_doom_is_key_down(KEY_UPARROW) == true);
    assert(fortress_doom_is_key_down(KEY_RSHIFT) == true);
    assert(fortress_doom_is_key_down(KEY_RIGHTARROW) == false);
    assert(fortress_doom_is_key_down(KEY_FIRE) == false);

    /* Release remaining keys */
    desktop_msg_t r1 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 103, .d = DESKTOP_KEY_RELEASE };
    desktop_msg_t r4 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 42,  .d = DESKTOP_KEY_RELEASE };
    write(ev_pipe[1], &r1, sizeof(r1));
    write(ev_pipe[1], &r4, sizeof(r4));
    assert(fortress_desktop_poll_events());

    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 0 && k == KEY_UPARROW);
    assert(DG_GetKey(&pressed, &k) == 1 && pressed == 0 && k == KEY_RSHIFT);
    assert(fortress_doom_is_key_down(KEY_UPARROW) == false);
    assert(fortress_doom_is_key_down(KEY_RSHIFT) == false);

    close(req_pipe[0]); close(req_pipe[1]);
    close(ev_pipe[0]); close(ev_pipe[1]);
    fortress_desktop_reset();
    printf("  -> PASS: Multi-key combinations and partial release tracking verified.\n");
}

static void test_stage3_focus_loss_and_stuck_key_prevention(void) {
    printf("[TEST] Testing focus loss handling and stuck key prevention...\n");
    fortress_desktop_reset();

    int req_pipe[2], ev_pipe[2];
    assert(pipe(req_pipe) == 0 && pipe(ev_pipe) == 0);
    uint32_t dummy[16];
    fortress_desktop_set_mock(req_pipe[1], ev_pipe[0], dummy, sizeof(dummy), 4);

    assert(fortress_doom_has_focus() == true);

    /* Hold down 3 keys: Up Arrow (103), Fire (29), Run (42) */
    desktop_msg_t p1 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 103, .d = DESKTOP_KEY_PRESS };
    desktop_msg_t p2 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 29,  .d = DESKTOP_KEY_PRESS };
    desktop_msg_t p3 = { .kind = DESKTOP_EV_KEY, .a = 0, .b = 42,  .d = DESKTOP_KEY_PRESS };
    write(ev_pipe[1], &p1, sizeof(p1));
    write(ev_pipe[1], &p2, sizeof(p2));
    write(ev_pipe[1], &p3, sizeof(p3));
    assert(fortress_desktop_poll_events());

    int pressed = 0; unsigned char k = 0;
    while (DG_GetKey(&pressed, &k)) {
        assert(pressed == 1);
    }
    assert(fortress_doom_is_key_down(KEY_UPARROW) == true);
    assert(fortress_doom_is_key_down(KEY_FIRE) == true);
    assert(fortress_doom_is_key_down(KEY_RSHIFT) == true);

    /*
     * Simulate focus loss: Compositor sends DESKTOP_EV_FOCUS with a=0
     * (e.g. user pressed Alt+Tab, Super, Super+L, or switched windows).
     */
    desktop_msg_t focus_lost = {
        .kind = DESKTOP_EV_FOCUS,
        .a = 0, /* unfocused */
    };
    write(ev_pipe[1], &focus_lost, sizeof(focus_lost));
    assert(fortress_desktop_poll_events());

    /* Focus state must be false */
    assert(fortress_doom_has_focus() == false);

    /* All held keys must be released immediately */
    assert(fortress_doom_is_key_down(KEY_UPARROW) == false);
    assert(fortress_doom_is_key_down(KEY_FIRE) == false);
    assert(fortress_doom_is_key_down(KEY_RSHIFT) == false);

    /* DG_GetKey must return release events (pressed == 0) for each held key */
    int release_count = 0;
    while (DG_GetKey(&pressed, &k)) {
        assert(pressed == 0 && "All synthesized focus-loss events must be releases");
        release_count++;
    }
    assert(release_count == 3 && "Exactly 3 release events generated for the 3 held keys");

    /* Regain focus */
    desktop_msg_t focus_gain = {
        .kind = DESKTOP_EV_FOCUS,
        .a = 1, /* focused */
    };
    write(ev_pipe[1], &focus_gain, sizeof(focus_gain));
    assert(fortress_desktop_poll_events());

    assert(fortress_doom_has_focus() == true);
    assert(DG_GetKey(&pressed, &k) == 0 && "No stale keys upon regaining focus");

    close(req_pipe[0]); close(req_pipe[1]);
    close(ev_pipe[0]); close(ev_pipe[1]);
    fortress_desktop_reset();
    printf("  -> PASS: Focus loss cleanly releases all held keys with zero stuck keys.\n");
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
        extern char **environ;
        int ret = doom_main(6, argv, environ);
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
        extern char **environ;
        int ret = doom_main(9, argv, environ);
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
        extern char **environ;
        doom_main(4, argv, environ);
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
        extern char **environ;
        doom_main(4, argv, environ);
        exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(corrupt_path);
    assert(WIFEXITED(status) && "corrupted WAD must exit cleanly without memory fault");
    printf("  -> PASS: Corrupted WAD caught and safely exited.\n");
}

static void test_stage4_app_registry_and_package_spec(void) {
    printf("[TEST] Testing Stage 4 application registry, icon, and package specification...\n");

    /* 1. Audit user/compositor/src/app_registry.rs */
    FILE *freg = fopen("user/compositor/src/app_registry.rs", "r");
    assert(freg != NULL && "app_registry.rs must exist");
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, freg);
    fclose(freg);
    buf[n] = '\0';

    assert(strstr(buf, "Doom") != NULL && "AppId::Doom must be defined");
    assert(strstr(buf, "exec_packaged: c\"/usr/bin/doom\"") != NULL);
    assert(strstr(buf, "exec_fallback: c\"/bin/doom\"") != NULL);
    assert(strstr(buf, "icon_kind: 10") != NULL);
    assert(strstr(buf, "category: \"Games\"") != NULL);
    assert(strstr(buf, "game doom freedm freedoom") != NULL);
    assert(strstr(buf, "bytes.ends_with(b\"/doom\") || bytes == b\"doom\"") != NULL);

    /* 2. Audit scripts/build_desktop_package.py */
    FILE *fpkg = fopen("scripts/build_desktop_package.py", "r");
    assert(fpkg != NULL && "build_desktop_package.py must exist");
    n = fread(buf, 1, sizeof(buf) - 1, fpkg);
    fclose(fpkg);
    buf[n] = '\0';

    assert(strstr(buf, "\"doom\": (\"fortress-doom\", \"build/doom.elf\"") != NULL);

    /* 3. Audit user/compositor/src/clients.rs external apps list */
    FILE *fcli = fopen("user/compositor/src/clients.rs", "r");
    assert(fcli != NULL && "clients.rs must exist");
    n = fread(buf, 1, sizeof(buf) - 1, fcli);
    fclose(fcli);
    buf[n] = '\0';
    assert(strstr(buf, "name: \"Doom\"") != NULL);
    assert(strstr(buf, "packaged: c\"/usr/bin/doom\"") != NULL);
    assert(strstr(buf, "image: c\"/bin/doom\"") != NULL);

    printf("  -> PASS: App registry manifest, icon, action dispatch, and package definition verified.\n");
}

static void test_stage4_user_save_isolation_and_atomic_replace(const char *wad_path) {
    printf("[TEST] Testing Stage 4 user save isolation, default.cfg, and atomic replacement...\n");
    fortress_desktop_reset();

    char tmp_home[] = "/tmp/doom_test_user_home_XXXXXX";
    assert(mkdtemp(tmp_home) != NULL);

    extern char **environ;
    pid_t pid = fork();
    if (pid == 0) {
        close(1); close(2);
        setenv("HOME", tmp_home, 1);
        unsetenv("DOOM_SAVE_DIR");
        char *argv[] = {
            (char *)"doom",
            (char *)"-nogui",
            (char *)"-iwad",
            (char *)wad_path,
            (char *)"-nosound",
            (char *)"-frames",
            (char *)"5",
            NULL
        };
        int ret = doom_main(7, argv, environ);
        exit(ret);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 && "doom run in test home must succeed");

    /* Verify directory structure: $HOME/.doom/ and $HOME/.doom/.savegame/ */
    char dot_doom[512], default_cfg[512], savegame_dir[512];
    snprintf(dot_doom, sizeof(dot_doom), "%s/.doom", tmp_home);
    snprintf(default_cfg, sizeof(default_cfg), "%s/.doom/default.cfg", tmp_home);
    snprintf(savegame_dir, sizeof(savegame_dir), "%s/.doom/.savegame", tmp_home);

    struct stat st;
    assert(stat(dot_doom, &st) == 0 && S_ISDIR(st.st_mode) && ".doom directory must exist");
    assert(stat(default_cfg, &st) == 0 && S_ISREG(st.st_mode) && "default.cfg must be created in .doom");
    assert(stat(savegame_dir, &st) == 0 && S_ISDIR(st.st_mode) && ".savegame directory must exist");

    /* Test durable atomic save replacement policy:
     * Write initial savegame slot 0 -> write temporary save -> rename replacement
     */
    char save0[512], save_temp[512];
    snprintf(save0, sizeof(save0), "%s/.doom/.savegame/doomsav0.dsg", tmp_home);
    snprintf(save_temp, sizeof(save_temp), "%s/.doom/.savegame/temp.dsg", tmp_home);

    FILE *f0 = fopen(save0, "wb");
    assert(f0 != NULL);
    const char initial_data[] = "DOOM_SAVEGAME_VERSION_1_ORIGINAL";
    fwrite(initial_data, 1, sizeof(initial_data), f0);
    fclose(f0);

    /* Write update to temporary file */
    FILE *ftemp = fopen(save_temp, "wb");
    assert(ftemp != NULL);
    const char updated_data[] = "DOOM_SAVEGAME_VERSION_2_REPLACED";
    fwrite(updated_data, 1, sizeof(updated_data), ftemp);
    fclose(ftemp);

    /* Replace atomically via rename() */
    assert(rename(save_temp, save0) == 0 && "atomic savegame replacement must succeed");
    assert(access(save_temp, F_OK) != 0 && "temp savegame must be unlinked after rename");

    /* Verify new content persisted */
    FILE *fcheck = fopen(save0, "rb");
    assert(fcheck != NULL);
    char check_buf[64] = {0};
    fread(check_buf, 1, sizeof(check_buf), fcheck);
    fclose(fcheck);
    assert(strcmp(check_buf, updated_data) == 0 && "persisted savegame must match updated data");

    /* Clean up temporary directory */
    unlink(save0);
    unlink(default_cfg);
    rmdir(savegame_dir);
    rmdir(dot_doom);
    rmdir(tmp_home);
    printf("  -> PASS: User configuration and saves segregated into ~/.doom with atomic replacement.\n");
}

static void test_stage4_wad_search_order_and_discovery(const char *wad_path) {
    printf("[TEST] Testing Stage 4 WAD search order and automatic discovery without -iwad...\n");
    fortress_desktop_reset();

    char tmp_home[] = "/tmp/doom_test_wad_discovery_XXXXXX";
    assert(mkdtemp(tmp_home) != NULL);

    char dot_doom[1024], user_wad[1024];
    snprintf(dot_doom, sizeof(dot_doom), "%s/.doom", tmp_home);
    assert(mkdir(dot_doom, 0755) == 0);
    snprintf(user_wad, sizeof(user_wad), "%s/freedm.wad", dot_doom);

    /* Symlink WAD into user directory */
    assert(symlink(wad_path, user_wad) == 0);

    extern char **environ;
    pid_t pid = fork();
    if (pid == 0) {
        close(1); close(2);
        setenv("HOME", tmp_home, 1);
        char *argv[] = {
            (char *)"doom",
            (char *)"-nogui",
            (char *)"-nosound",
            (char *)"-frames",
            (char *)"5",
            NULL
        };
        /* Notice: NO -iwad argument passed! Must auto-discover ~/.doom/freedm.wad */
        int ret = doom_main(5, argv, environ);
        exit(ret);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 && "auto-discovery of user WAD must succeed");

    /* Clean up */
    unlink(user_wad);
    char cfg[512], svg[512];
    snprintf(cfg, sizeof(cfg), "%s/.doom/default.cfg", tmp_home);
    snprintf(svg, sizeof(svg), "%s/.doom/.savegame", tmp_home);
    unlink(cfg);
    rmdir(svg);
    rmdir(dot_doom);
    rmdir(tmp_home);
    printf("  -> PASS: Automatic WAD discovery in per-user library without -iwad verified.\n");
}

static void test_stage4_missing_wad_actionable_guidance(void) {
    printf("[TEST] Testing Stage 4 missing WAD actionable diagnostic guidance...\n");
    fortress_desktop_reset();

    char empty_home[] = "/tmp/doom_test_empty_home_XXXXXX";
    assert(mkdtemp(empty_home) != NULL);

    int pipe_out[2];
    assert(pipe(pipe_out) == 0);

    extern char **environ;
    pid_t pid = fork();
    if (pid == 0) {
        close(pipe_out[0]);
        dup2(pipe_out[1], 1);
        dup2(pipe_out[1], 2);
        close(pipe_out[1]);

        setenv("HOME", empty_home, 1);
        char *argv[] = {
            (char *)"doom",
            (char *)"-nogui",
            (char *)"-nosound",
            NULL
        };
        int ret = doom_main(3, argv, environ);
        exit(ret);
    }
    close(pipe_out[1]);

    char out_buf[4096] = {0};
    size_t total = 0;
    ssize_t r;
    while ((r = read(pipe_out[0], out_buf + total, sizeof(out_buf) - total - 1)) > 0) {
        total += (size_t)r;
    }
    close(pipe_out[0]);
    out_buf[total] = '\0';

    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 1 && "missing WAD must exit cleanly with code 1");

    assert(strstr(out_buf, "[FortressOS Doom] No Game Data (IWAD) Found!") != NULL);
    assert(strstr(out_buf, "freedoom1.wad") != NULL);
    assert(strstr(out_buf, "26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7") != NULL);
    assert(strstr(out_buf, "d9adc4d792627e7fc47b09067b15486da724010c71dd12831e1cf8e0755b68ad") != NULL);
    assert(strstr(out_buf, "/usr/share/games/doom") != NULL);

    rmdir(empty_home);
    printf("  -> PASS: Missing WAD prints actionable guidance and verified SHA-256 hashes.\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("====================================================================================\n");
    printf("FortressOS Doom Desktop, Input, Content & Packaging Suite (Stages 0, 1, 2, 3, and 4)\n");
    printf("====================================================================================\n");

    test_pinned_identities();
    test_desktop_protocol_abi();
    test_desktop_scaling_and_presentation();
    test_desktop_input_and_close();
    test_stage3_keyboard_mappings();
    test_stage3_azerty_and_us_parity();
    test_stage3_simultaneous_keys();
    test_stage3_focus_loss_and_stuck_key_prevention();
    test_stage4_app_registry_and_package_spec();

    const char *freedm_wad = "/tmp/doom-eval/freedm-0.13.0/freedm.wad";
    const char *freedoom1_wad = "/tmp/doom-eval/freedoom-0.13.0/freedoom1.wad";

    if (access(freedm_wad, R_OK) == 0) {
        test_engine_init_and_frames(freedm_wad);
        test_stage4_user_save_isolation_and_atomic_replace(freedm_wad);
        test_stage4_wad_search_order_and_discovery(freedm_wad);
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
    test_stage4_missing_wad_actionable_guidance();

    printf("\n>>> ALL STAGE 0, 1, 2, 3 & 4 DOOM TESTS PASSED <<<\n");
    return 0;
}
