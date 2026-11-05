/* doomgeneric platform adapter for FortressOS */

#include "doom_runtime.h"

#ifndef DOOM_HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#else
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#endif

#include "upstream/doomgeneric.h"

static uint64_t s_start_time_ms = 0;
static uint64_t s_frame_count = 0;
static uint32_t s_last_first_pixel = 0;

#ifndef DOOM_HOST_TEST
#define SYS_CLOCK_GETTIME 74
#define CLOCK_MONOTONIC   1

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

void DG_Init(void) {
    s_start_time_ms = fortress_get_time_ms();
    s_frame_count = 0;
    printf("[FortressOS Doom] DG_Init: ScreenBuffer=%p, Res=%dx%d\n",
           DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_DrawFrame(void) {
    s_frame_count++;
    if (DG_ScreenBuffer) {
        s_last_first_pixel = DG_ScreenBuffer[0];
    }
    if (s_frame_count == 1 || s_frame_count % 35 == 0) {
        printf("[FortressOS Doom] Frame %u produced. Pixel[0]=0x%08x\n",
               (unsigned)s_frame_count, s_last_first_pixel);
    }
}

void DG_SleepMs(uint32_t ms) {
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void) {
    uint64_t now = fortress_get_time_ms();
    return (uint32_t)(now - s_start_time_ms);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    (void)pressed;
    (void)key;
    /* Stage 1 feasibility: no keys queued */
    return 0;
}

void DG_SetWindowTitle(const char *title) {
    printf("[FortressOS Doom] Title: %s\n", title ? title : "(null)");
}

uint64_t fortress_doom_get_frame_count(void) {
    return s_frame_count;
}

uint32_t fortress_doom_get_last_pixel(void) {
    return s_last_first_pixel;
}

int doom_main(int argc, char **argv) {
    printf("Starting Doom on FortressOS...\n");
    doom_runtime_init();

    doomgeneric_Create(argc, argv);

    /* Run 70 tics for Stage 1 feasibility validation */
    for (int i = 0; i < 70; i++) {
        doomgeneric_Tick();
    }

    printf("Feasibility check complete: successfully produced %u frames.\n",
           (unsigned)s_frame_count);

    doom_runtime_stats_t stats;
    doom_runtime_get_stats(&stats);
    printf("Runtime Stats:\n"
           "  Heap allocated peak: %u bytes\n"
           "  Heap allocations:    %u\n"
           "  Heap frees:          %u\n"
           "  File open count:     %u\n"
           "  File read bytes:     %u\n"
           "  File seeks:          %u\n",
           (unsigned)stats.heap_allocated_peak,
           (unsigned)stats.heap_alloc_count,
           (unsigned)stats.heap_free_count,
           (unsigned)stats.file_open_count,
           (unsigned)stats.file_read_bytes,
           (unsigned)stats.file_seek_count);

    return 0;
}
