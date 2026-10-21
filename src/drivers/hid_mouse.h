#ifndef FORTRESS_HID_MOUSE_H
#define FORTRESS_HID_MOUSE_H

#include "types.h"

/* USB HID Boot Mouse Report (at least 3 bytes, optional 4th byte for wheel) */
typedef struct {
    uint8_t buttons; /* Bit 0: Left, Bit 1: Right, Bit 2: Middle */
    int8_t  dx;      /* Relative X motion (-128..127) */
    int8_t  dy;      /* Relative Y motion (-128..127) */
    int8_t  dz;      /* Relative Z / wheel motion (-128..127) (optional) */
} __attribute__((packed)) hid_boot_mouse_report_t;

/* Mouse Button Masks */
#define HID_MOUSE_BTN_LEFT    (1u << 0)
#define HID_MOUSE_BTN_RIGHT   (1u << 1)
#define HID_MOUSE_BTN_MIDDLE  (1u << 2)
#define HID_MOUSE_BTN_SIDE    (1u << 3)
#define HID_MOUSE_BTN_EXTRA   (1u << 4)

/* Mouse State & Aggregated Position */
typedef struct {
    int32_t x;           /* Clamped or accumulated X coordinate */
    int32_t y;           /* Clamped or accumulated Y coordinate */
    int32_t max_x;       /* Virtual screen / bounding box max X (0 for unbounded) */
    int32_t max_y;       /* Virtual screen / bounding box max Y (0 for unbounded) */
    uint8_t buttons;     /* Currently held buttons */
    uint64_t report_count;
    uint64_t event_count;
} hid_mouse_state_t;

/* Decoded Mouse Event passed to sink */
typedef struct {
    int8_t  dx;
    int8_t  dy;
    int8_t  dz;          /* Wheel */
    uint8_t buttons;     /* Current button state */
    uint8_t pressed;     /* Buttons pressed in this transition (0 -> 1) */
    uint8_t released;    /* Buttons released in this transition (1 -> 0) */
    int32_t x;           /* Current accumulated X */
    int32_t y;           /* Current accumulated Y */
} hid_mouse_event_t;

typedef void (*hid_mouse_event_fn_t)(const hid_mouse_event_t *event, void *ctx);

/*
 * Process a raw HID mouse report:
 * Parses buttons, relative deltas (dx, dy, dz), updates state (with optional clamping),
 * detects button edge transitions (press, release), and invokes the sink callback if provided.
 */
static inline void hid_mouse_process_report(const uint8_t *raw_report,
                                            size_t len,
                                            hid_mouse_state_t *state,
                                            hid_mouse_event_fn_t sink,
                                            void *ctx) {
    if (!raw_report || len < 3 || !state) return;

    uint8_t buttons = raw_report[0];
    int8_t dx = (int8_t)raw_report[1];
    int8_t dy = (int8_t)raw_report[2];
    int8_t dz = (len >= 4) ? (int8_t)raw_report[3] : 0;

    uint8_t old_buttons = state->buttons;
    uint8_t pressed = buttons & ~old_buttons;
    uint8_t released = old_buttons & ~buttons;

    state->buttons = buttons;
    state->x += dx;
    state->y += dy;

    if (state->max_x > 0) {
        if (state->x < 0) state->x = 0;
        if (state->x > state->max_x) state->x = state->max_x;
    }
    if (state->max_y > 0) {
        if (state->y < 0) state->y = 0;
        if (state->y > state->max_y) state->y = state->max_y;
    }

    state->report_count++;

    if (dx != 0 || dy != 0 || dz != 0 || pressed != 0 || released != 0) {
        state->event_count++;
        if (sink) {
            hid_mouse_event_t ev = {
                .dx = dx,
                .dy = dy,
                .dz = dz,
                .buttons = buttons,
                .pressed = pressed,
                .released = released,
                .x = state->x,
                .y = state->y,
            };
            sink(&ev, ctx);
        }
    }
}

#endif /* FORTRESS_HID_MOUSE_H */
