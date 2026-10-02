#include "wget_codec.h"

/* Freestanding helper functions */
static size_t wget_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void wget_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void wget_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static char wget_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static int wget_strncasecmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c1 = wget_tolower(s1[i]);
        char c2 = wget_tolower(s2[i]);
        if (c1 != c2) return (unsigned char)c1 - (unsigned char)c2;
        if (!c1) return 0;
    }
    return 0;
}

static void derive_filename(const char *path, char *out_filename, size_t max_len) {
    size_t path_len = wget_strlen(path);
    const char *last_slash = NULL;
    for (size_t i = 0; i < path_len; i++) {
        if (path[i] == '/') last_slash = &path[i];
    }

    const char *start = last_slash ? (last_slash + 1) : path;
    /* Stop at '?' or '#' or end of string */
    size_t comp_len = 0;
    while (start[comp_len] && start[comp_len] != '?' && start[comp_len] != '#') {
        comp_len++;
    }

    if (comp_len == 0) {
        /* Empty filename or ends in slash: default to index.html */
        const char *def = "index.html";
        size_t def_len = wget_strlen(def);
        if (def_len >= max_len) def_len = max_len - 1;
        wget_memcpy(out_filename, def, def_len);
        out_filename[def_len] = '\0';
        return;
    }

    if (comp_len >= max_len) comp_len = max_len - 1;
    wget_memcpy(out_filename, start, comp_len);
    out_filename[comp_len] = '\0';
}

int wget_parse_url(const char *raw_url, wget_url_t *out_url) {
    if (!raw_url || !out_url) return WGET_ERR_INVALID_URL;
    wget_memset(out_url, 0, sizeof(*out_url));

    size_t raw_len = wget_strlen(raw_url);
    if (raw_len >= WGET_MAX_URL_LEN) return WGET_ERR_URL_TOO_LONG;
    wget_memcpy(out_url->raw_url, raw_url, raw_len);
    out_url->raw_url[raw_len] = '\0';

    const char *p = raw_url;

    /* Check scheme */
    if (wget_strncasecmp(p, "https://", 8) == 0) {
        return WGET_ERR_HTTPS;
    }
    if (wget_strncasecmp(p, "http://", 7) == 0) {
        p += 7;
    } else {
        /* Check if there's any other scheme "xyz://" */
        const char *colon_slash = NULL;
        for (size_t i = 0; p[i] && p[i] != '/'; i++) {
            if (p[i] == ':' && p[i+1] == '/' && p[i+2] == '/') {
                colon_slash = &p[i];
                break;
            }
        }
        if (colon_slash) {
            return WGET_ERR_UNSUPPORTED_SCHEME;
        }
        /* No scheme prefix: assume http:// */
    }

    /* Skip optional userinfo: user:pass@ */
    const char *at_sign = NULL;
    for (const char *scan = p; *scan && *scan != '/'; scan++) {
        if (*scan == '@') {
            at_sign = scan;
            break;
        }
    }
    if (at_sign) {
        p = at_sign + 1;
    }

    /* Extract host and optional port */
    const char *host_start = p;
    size_t host_len = 0;
    while (*p && *p != ':' && *p != '/') {
        host_len++;
        p++;
    }

    if (host_len == 0) return WGET_ERR_INVALID_URL;
    if (host_len >= WGET_MAX_HOST_LEN) return WGET_ERR_HOST_TOO_LONG;

    wget_memcpy(out_url->host, host_start, host_len);
    out_url->host[host_len] = '\0';

    /* Parse port if present */
    uint16_t port = 80;
    if (*p == ':') {
        p++;
        unsigned num = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            num = num * 10 + (unsigned)(*p - '0');
            if (num > 65535) return WGET_ERR_INVALID_PORT;
            digits++;
            p++;
        }
        if (digits == 0 || num == 0) return WGET_ERR_INVALID_PORT;
        port = (uint16_t)num;
    }
    out_url->port = port;

    /* Extract path */
    if (*p == '\0') {
        out_url->path[0] = '/';
        out_url->path[1] = '\0';
    } else {
        size_t path_len = wget_strlen(p);
        if (path_len >= WGET_MAX_URL_LEN) return WGET_ERR_PATH_TOO_LONG;
        wget_memcpy(out_url->path, p, path_len);
        out_url->path[path_len] = '\0';
    }

    /* Derive default filename */
    derive_filename(out_url->path, out_url->filename, sizeof(out_url->filename));

    return WGET_OK;
}

int wget_find_header_end(const uint8_t *buf, size_t buf_len, size_t *header_len) {
    if (!buf || !header_len) return WGET_ERR_MALFORMED_HEADER;

    /* Search for \r\n\r\n or \n\n */
    for (size_t i = 0; i < buf_len; i++) {
        if (i + 3 < buf_len && buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            *header_len = i + 4;
            return WGET_OK;
        }
        if (i + 1 < buf_len && buf[i] == '\n' && buf[i+1] == '\n') {
            *header_len = i + 2;
            return WGET_OK;
        }
    }

    if (buf_len >= WGET_MAX_HEADER_BYTES) {
        return WGET_ERR_HEADER_OVERFLOW;
    }

    return WGET_NEED_MORE_DATA;
}

int wget_parse_response_headers(const uint8_t *buf, size_t header_len, wget_response_t *out_resp) {
    if (!buf || !out_resp || header_len == 0) return WGET_ERR_MALFORMED_HEADER;
    wget_memset(out_resp, 0, sizeof(*out_resp));
    out_resp->header_len = header_len;

    const char *p = (const char *)buf;
    const char *end = p + header_len;

    /* 1. Parse Status Line */
    /* Find end of first line */
    const char *line_end = p;
    while (line_end < end && *line_end != '\r' && *line_end != '\n') {
        line_end++;
    }

    /* Verify "HTTP/" prefix */
    if ((size_t)(line_end - p) < 12 || wget_strncasecmp(p, "HTTP/", 5) != 0) {
        return WGET_ERR_MALFORMED_HEADER;
    }

    /* Skip to status code: find space */
    const char *sp1 = p;
    while (sp1 < line_end && *sp1 != ' ') sp1++;
    if (sp1 >= line_end) return WGET_ERR_MALFORMED_HEADER;
    sp1++; /* skip space */

    int code = 0;
    int digits = 0;
    while (sp1 < line_end && *sp1 >= '0' && *sp1 <= '9') {
        code = code * 10 + (*sp1 - '0');
        digits++;
        sp1++;
    }
    if (digits != 3) return WGET_ERR_MALFORMED_HEADER;
    out_resp->status_code = code;

    /* Copy status text (reason phrase) */
    while (sp1 < line_end && *sp1 == ' ') sp1++;
    size_t reason_len = (size_t)(line_end - sp1);
    if (reason_len >= sizeof(out_resp->status_text)) {
        reason_len = sizeof(out_resp->status_text) - 1;
    }
    wget_memcpy(out_resp->status_text, sp1, reason_len);
    out_resp->status_text[reason_len] = '\0';

    /* 2. Parse Remaining Header Lines */
    p = line_end;
    while (p < end) {
        /* Skip \r and \n */
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        if (p >= end) break;

        line_end = p;
        while (line_end < end && *line_end != '\r' && *line_end != '\n') {
            line_end++;
        }
        size_t cur_len = (size_t)(line_end - p);

        /* Check for Content-Length: */
        if (cur_len >= 15 && wget_strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *val = p + 15;
            while (val < line_end && (*val == ' ' || *val == '\t')) val++;
            uint64_t cl = 0;
            bool valid = false;
            while (val < line_end && *val >= '0' && *val <= '9') {
                cl = cl * 10 + (uint64_t)(*val - '0');
                valid = true;
                val++;
            }
            if (valid) {
                out_resp->has_content_length = true;
                out_resp->content_length = cl;
            }
        }
        /* Check for Location: */
        else if (cur_len >= 9 && wget_strncasecmp(p, "Location:", 9) == 0) {
            const char *val = p + 9;
            while (val < line_end && (*val == ' ' || *val == '\t')) val++;
            const char *val_end = line_end;
            while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t')) val_end--;
            size_t loc_len = (size_t)(val_end - val);
            if (loc_len >= sizeof(out_resp->location)) {
                loc_len = sizeof(out_resp->location) - 1;
            }
            wget_memcpy(out_resp->location, val, loc_len);
            out_resp->location[loc_len] = '\0';
        }

        p = line_end;
    }

    return WGET_OK;
}

int wget_resolve_redirect(const wget_url_t *current_url, const char *location, wget_url_t *out_url) {
    if (!current_url || !location || !out_url) return WGET_ERR_INVALID_URL;

    /* Scheme check */
    if (wget_strncasecmp(location, "https://", 8) == 0) {
        return WGET_ERR_HTTPS;
    }
    if (wget_strncasecmp(location, "http://", 7) == 0) {
        return wget_parse_url(location, out_url);
    }
    if (wget_strncasecmp(location, "//", 2) == 0) {
        /* Scheme-relative URL: default to http: */
        static char s_redirect_full[WGET_MAX_URL_LEN];
        size_t loc_len = wget_strlen(location);
        if (5 + loc_len >= sizeof(s_redirect_full)) return WGET_ERR_URL_TOO_LONG;
        wget_memcpy(s_redirect_full, "http:", 5);
        wget_memcpy(s_redirect_full + 5, location, loc_len);
        s_redirect_full[5 + loc_len] = '\0';
        return wget_parse_url(s_redirect_full, out_url);
    }

    /* Relative URL */
    wget_memset(out_url, 0, sizeof(*out_url));
    size_t host_len = wget_strlen(current_url->host);
    wget_memcpy(out_url->host, current_url->host, host_len + 1);
    out_url->port = current_url->port;

    if (location[0] == '/') {
        /* Absolute path on same server */
        size_t loc_len = wget_strlen(location);
        if (loc_len >= sizeof(out_url->path)) return WGET_ERR_PATH_TOO_LONG;
        wget_memcpy(out_url->path, location, loc_len + 1);
    } else {
        /* Relative to directory of current path */
        const char *last_slash = NULL;
        for (const char *s = current_url->path; *s; s++) {
            if (*s == '/') last_slash = s;
        }
        size_t dir_len = last_slash ? (size_t)(last_slash - current_url->path + 1) : 1;
        size_t loc_len = wget_strlen(location);
        if (dir_len + loc_len >= sizeof(out_url->path)) return WGET_ERR_PATH_TOO_LONG;

        if (last_slash) {
            wget_memcpy(out_url->path, current_url->path, dir_len);
        } else {
            out_url->path[0] = '/';
            dir_len = 1;
        }
        wget_memcpy(out_url->path + dir_len, location, loc_len);
        out_url->path[dir_len + loc_len] = '\0';
    }

    derive_filename(out_url->path, out_url->filename, sizeof(out_url->filename));
    return WGET_OK;
}

const char *wget_strerror(int err) {
    switch (err) {
        case WGET_OK: return "success";
        case WGET_ERR_INVALID_URL: return "invalid URL";
        case WGET_ERR_HTTPS: return "HTTPS (TLS) is not supported yet; please use plain HTTP (http://...)";
        case WGET_ERR_UNSUPPORTED_SCHEME: return "unsupported URL scheme";
        case WGET_ERR_URL_TOO_LONG: return "URL exceeds 1024 bytes limit";
        case WGET_ERR_HOST_TOO_LONG: return "hostname exceeds 255 bytes limit";
        case WGET_ERR_PATH_TOO_LONG: return "path exceeds 1024 bytes limit";
        case WGET_ERR_INVALID_PORT: return "invalid port number";
        case WGET_ERR_HEADER_OVERFLOW: return "response headers exceed 8192 bytes limit";
        case WGET_NEED_MORE_DATA: return "need more data";
        case WGET_ERR_MALFORMED_HEADER: return "malformed HTTP response header";
        case WGET_ERR_REDIRECT_OVERFLOW: return "too many redirects (exceeded 5 hops)";
        default: return "unknown error";
    }
}
