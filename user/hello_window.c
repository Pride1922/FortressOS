/* hello-window: smallest out-of-process desktop app (protocol:
 * src/include/desktop_proto.h). A gradient with a box; h/j/k/l move the
 * box, any other key recolours it, q quits. The title counts keys. */
#include "user/tools/common.h"
#include "src/include/syscall_abi.h"
#include "src/include/desktop_proto.h"

#define PIXELS_VA 0x7000000000ULL
#define BOX 40

static uint32_t *pixels;
static uint32_t width, height, stride;
static int box_x = 60, box_y = 50;
static uint32_t box_color = 0xffe8e8e8;
static unsigned keys;

static bool send(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d, const char *text) {
    desktop_msg_t m = {.kind = kind, .a = a, .b = b, .c = c, .d = d};
    if (text) {
        size_t n = tool_length(text);
        if (n > DESKTOP_TEXT_MAX) n = DESKTOP_TEXT_MAX;
        for (size_t i = 0; i < n; i++) m.text[i] = text[i];
        m.len = (uint32_t)n;
    }
    const char *p = (const char *)&m;
    for (size_t done = 0; done < sizeof(m);) {
        long n = tool_syscall(SYS_WRITE, DESKTOP_FD_REQUESTS, (uintptr_t)(p + done), sizeof(m) - done);
        if (n == SYSCALL_EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

static bool receive(desktop_msg_t *m) {
    char *p = (char *)m;
    for (size_t done = 0; done < sizeof(*m);) {
        long n = tool_syscall(SYS_READ, DESKTOP_FD_EVENTS, (uintptr_t)(p + done), sizeof(*m) - done);
        if (n == SYSCALL_EINTR) continue;
        if (n <= 0) return false; /* compositor gone */
        done += (size_t)n;
    }
    return true;
}

static void draw(void) {
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            uint32_t r = x * 200 / (width ? width : 1), g = y * 160 / (height ? height : 1);
            uint32_t color = 0xff000000 | r << 16 | g << 8 | 0x70;
            if ((int)x >= box_x && (int)x < box_x + BOX && (int)y >= box_y && (int)y < box_y + BOX)
                color = box_color;
            pixels[y * stride + x] = color;
        }
    send(DESKTOP_REQ_DAMAGE, 0, 0, width, height, 0);
}

static void title(void) {
    char text[DESKTOP_TEXT_MAX] = "Hello - ";
    size_t at = tool_length(text);
    at += tool_format_u64(text + at, keys);
    const char *suffix = " keys";
    for (size_t i = 0; suffix[i] && at + 1 < sizeof(text); i++) text[at++] = suffix[i];
    text[at] = 0;
    send(DESKTOP_REQ_TITLE, 0, 0, 0, 0, text);
}

int hello_window_main(void) {
    stat_ext_v1_t st = {.size = sizeof(st), .version = 1};
    if (tool_syscall(SYS_FSTAT, DESKTOP_FD_PIXELS, (uintptr_t)&st, 0) < 0) return 2;
    long va = tool_syscall(SYS_SHM_MAP, DESKTOP_FD_PIXELS, PIXELS_VA, 0);
    if (va < 0) return 3;
    pixels = (uint32_t *)(uintptr_t)va;
    if (!send(DESKTOP_REQ_CREATE, 420, 260, 0, 0, "Hello, FortressOS")) return 4;
    desktop_msg_t m;
    while (receive(&m)) {
        switch (m.kind) {
        case DESKTOP_EV_CONFIGURE:
            /* Never trust sizes beyond the buffer we actually have. */
            if ((uint64_t)m.c * m.b * 4 > st.file_size || m.a > m.c) return 5;
            width = m.a; height = m.b; stride = m.c;
            if (box_x + BOX > (int)width) box_x = 0;
            if (box_y + BOX > (int)height) box_y = 0;
            draw();
            break;
        case DESKTOP_EV_KEY:
            keys++;
            if (m.a == 'q') return 0;
            if (m.a == 'h' && box_x >= 20) box_x -= 20;
            else if (m.a == 'l' && box_x + BOX + 20 <= (int)width) box_x += 20;
            else if (m.a == 'k' && box_y >= 20) box_y -= 20;
            else if (m.a == 'j' && box_y + BOX + 20 <= (int)height) box_y += 20;
            else box_color = 0xff000000 | (box_color * 2654435761u >> 8);
            draw();
            title();
            break;
        case DESKTOP_EV_CLOSE:
            return 0;
        default:
            break;
        }
    }
    return 0;
}
