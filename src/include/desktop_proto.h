#ifndef FORTRESS_DESKTOP_PROTO_H
#define FORTRESS_DESKTOP_PROTO_H
/* Desktop client protocol v1. Authoritative description and Rust twin:
 * user/compositor/src/proto.rs. Apps start with fd 3 = requests (write),
 * fd 4 = events (read), fd 5 = shared pixel buffer (stride x screen height,
 * 0xAARRGGBB), and /dev/null on 0..2. Every message is 64 bytes,
 * little-endian. */
#include "types.h"

#define DESKTOP_FD_REQUESTS 3
#define DESKTOP_FD_EVENTS   4
#define DESKTOP_FD_PIXELS   5

#define DESKTOP_TEXT_MAX 40

#define DESKTOP_REQ_CREATE    1u  /* a,b = outer width/height, text = title */
#define DESKTOP_REQ_DAMAGE    2u  /* a,b,c,d = x,y,w,h in content pixels */
#define DESKTOP_REQ_TITLE     3u  /* text = title (printable ASCII) */
#define DESKTOP_REQ_OPEN_BEGIN  4u /* a = path length 1..127: open in the editor */
#define DESKTOP_REQ_OPEN_APPEND 5u /* text = next 1..40 path bytes (printable) */
#define DESKTOP_REQ_OPEN_COMMIT 6u /* absolute path complete; at most 1/s */
#define DESKTOP_EV_CONFIGURE 101u /* a,b = content width/height, c = stride */
/* a = ascii byte (0 when non-printable or release; preserved for legacy apps),
 * b = Linux keycode, c = modifier bitmask (DESKTOP_MOD_*),
 * d = state (0 = release, 1 = press, 2 = repeat). */
#define DESKTOP_EV_KEY       102u
#define DESKTOP_EV_CLOSE     103u /* window closed: save and exit */
#define DESKTOP_EV_CLICK     104u /* a,b = x,y in content pixels (left press) */
#define DESKTOP_EV_FOCUS     105u /* a = 1 (focused), 0 (unfocused) */

#define DESKTOP_KEY_RELEASE  0u
#define DESKTOP_KEY_PRESS    1u
#define DESKTOP_KEY_REPEAT   2u

#define DESKTOP_MOD_SHIFT    (1u << 0)
#define DESKTOP_MOD_CTRL     (1u << 1)
#define DESKTOP_MOD_ALT      (1u << 2)
#define DESKTOP_MOD_SUPER    (1u << 3)
#define DESKTOP_MOD_CAPS     (1u << 4)
#define DESKTOP_MOD_NUMLOCK  (1u << 5)
#define DESKTOP_MOD_ALTGR    (1u << 6)

typedef struct {
    uint32_t kind, a, b, c, d, len;
    char text[DESKTOP_TEXT_MAX];
} desktop_msg_t;
_Static_assert(sizeof(desktop_msg_t) == 64, "desktop protocol message is 64 bytes");

#endif
