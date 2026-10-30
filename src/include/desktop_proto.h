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
#define DESKTOP_EV_CONFIGURE 101u /* a,b = content width/height, c = stride */
#define DESKTOP_EV_KEY       102u /* a = typed byte */
#define DESKTOP_EV_CLOSE     103u /* window closed: save and exit */

typedef struct {
    uint32_t kind, a, b, c, d, len;
    char text[DESKTOP_TEXT_MAX];
} desktop_msg_t;
_Static_assert(sizeof(desktop_msg_t) == 64, "desktop protocol message is 64 bytes");

#endif
