#ifndef FORTRESS_USER_WGET_CODEC_H
#define FORTRESS_USER_WGET_CODEC_H

#include "types.h"

#define WGET_MAX_URL_LEN       1024
#define WGET_MAX_LOCATION_LEN  1024
#define WGET_MAX_HEADER_BYTES  8192
#define WGET_MAX_FILENAME_LEN  256
#define WGET_MAX_HOST_LEN      256
#define WGET_MAX_REDIRECTS     5

enum {
    WGET_OK = 0,
    WGET_ERR_INVALID_URL = -1,
    WGET_ERR_HTTPS = -2,
    WGET_ERR_UNSUPPORTED_SCHEME = -3,
    WGET_ERR_URL_TOO_LONG = -4,
    WGET_ERR_HOST_TOO_LONG = -5,
    WGET_ERR_PATH_TOO_LONG = -6,
    WGET_ERR_INVALID_PORT = -7,
    WGET_ERR_HEADER_OVERFLOW = -8,
    WGET_NEED_MORE_DATA = -9,
    WGET_ERR_MALFORMED_HEADER = -10,
    WGET_ERR_REDIRECT_OVERFLOW = -11,
};

typedef struct {
    char raw_url[WGET_MAX_URL_LEN];
    char host[WGET_MAX_HOST_LEN];
    uint16_t port;
    char path[WGET_MAX_URL_LEN];
    char filename[WGET_MAX_FILENAME_LEN];
} wget_url_t;

typedef struct {
    int status_code;
    char status_text[64];
    bool has_content_length;
    uint64_t content_length;
    char location[WGET_MAX_LOCATION_LEN];
    size_t header_len; /* Byte offset where body starts */
} wget_response_t;

/* Parse raw URL into host, port, path, and default filename.
 * Returns WGET_OK on success, or WGET_ERR_* on failure. */
int wget_parse_url(const char *raw_url, wget_url_t *out_url);

/* Find end of HTTP headers (\r\n\r\n or \n\n) within bounded 8192 bytes.
 * Returns WGET_OK with *header_len set to body start offset,
 * WGET_NEED_MORE_DATA if delimiter not yet reached and len < 8192,
 * or WGET_ERR_HEADER_OVERFLOW if len >= 8192 without finding delimiter. */
int wget_find_header_end(const uint8_t *buf, size_t buf_len, size_t *header_len);

/* Parse HTTP response status line and headers up to header_len.
 * Extracts status_code, status_text, Content-Length, and Location. */
int wget_parse_response_headers(const uint8_t *buf, size_t header_len, wget_response_t *out_resp);

/* Resolve a redirect Location header relative to current_url.
 * Returns WGET_OK on success, WGET_ERR_HTTPS if redirecting to HTTPS, or other error. */
int wget_resolve_redirect(const wget_url_t *current_url, const char *location, wget_url_t *out_url);

/* Return human-readable error description for a WGET_ERR_* code. */
const char *wget_strerror(int err);

#endif /* FORTRESS_USER_WGET_CODEC_H */
