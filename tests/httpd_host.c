/* Host unit test suite for FortressOS Tiny HTTP Server under ASan/UBSan */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE 1

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_HOST 1

#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "vfs.h"
#include "poll.h"
#include "udp_common.h"
#include "string.h"

static void copy_str(char *dst, const char *src) {
    size_t len = strlen(src);
    memcpy(dst, src, len + 1);
}

/* Forward declare mock dispatcher */
static long host_mock_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e, uintptr_t f);

#define HTTPD_SYSCALL host_mock_syscall

/* Include user/httpd.c directly */
#include "../user/httpd.c"

/* Mock state */
typedef struct {
    bool is_open;
    const uint8_t *in_data;
    size_t in_len;
    size_t in_pos;

    uint8_t out_data[16384];
    size_t out_len;
} mock_sock_t;

static mock_sock_t s_mock_socks[32];
static int s_mock_client_fd = 4;
static int s_mock_file_fd = 5;

static const char *s_mock_file_content = "Hello, FortressOS HTTP World!\n";
static size_t s_mock_file_size = 30;

static long host_mock_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e, uintptr_t f)
{
    (void)d; (void)e; (void)f;
    if (nr == SYS_SYSINFO) {
        sysinfo_t *info = (sysinfo_t *)a;
        if (info) {
            info->uptime_ticks = 1000;
            info->tick_hz = 1000;
        }
        return 0;
    }
    if (nr == SYS_STAT) {
        const char *path = (const char *)a;
        vfs_stat_t *st = (vfs_stat_t *)b;
        if (strstr(path, "missing")) return -VFS_ENOENT;
        if (strstr(path, "forbidden")) return -VFS_EACCES;
        if (st) {
            st->type = VFS_FILE;
            st->size = s_mock_file_size;
            st->mode = 0644;
        }
        return 0;
    }
    if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        if (strstr(path, "missing")) return -VFS_ENOENT;
        if (strstr(path, "forbidden")) return -VFS_EACCES;
        return s_mock_file_fd;
    }
    if (nr == SYS_CLOSE) {
        int fd = (int)a;
        if (fd >= 0 && fd < 32) {
            s_mock_socks[fd].is_open = false;
        }
        return 0;
    }
    if (nr == SYS_SHUTDOWN) {
        return 0;
    }
    if (nr == SYS_READ) {
        int fd = (int)a;
        void *buf = (void *)b;
        size_t cap = (size_t)c;
        if (fd == s_mock_file_fd) {
            size_t left = s_mock_file_size;
            if (left > cap) left = cap;
            memcpy(buf, s_mock_file_content, left);
            return (long)left;
        }
        if (fd >= 0 && fd < 32 && s_mock_socks[fd].is_open) {
            mock_sock_t *ms = &s_mock_socks[fd];
            size_t left = ms->in_len - ms->in_pos;
            if (left == 0) return -11; /* EAGAIN */
            if (left > cap) left = cap;
            memcpy(buf, ms->in_data + ms->in_pos, left);
            ms->in_pos += left;
            return (long)left;
        }
        return -VFS_EBADF;
    }
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const void *buf = (const void *)b;
        size_t len = (size_t)c;
        if (fd == 1 || fd == 2) {
            /* Debug / stderr write */
            return (long)len;
        }
        if (fd >= 0 && fd < 32 && s_mock_socks[fd].is_open) {
            mock_sock_t *ms = &s_mock_socks[fd];
            size_t space = sizeof(ms->out_data) - ms->out_len;
            if (space == 0) return -11; /* EAGAIN */
            if (len > space) len = space;
            memcpy(ms->out_data + ms->out_len, buf, len);
            ms->out_len += len;
            return (long)len;
        }
        return -VFS_EBADF;
    }
    return 0;
}

static void test_mime_resolution(void) {
    printf("[1/5] Testing MIME type detection...\n");
    assert(strcmp(get_mime_type("index.html"), "text/html; charset=utf-8") == 0);
    assert(strcmp(get_mime_type("INDEX.HTM"), "text/html; charset=utf-8") == 0);
    assert(strcmp(get_mime_type("style.css"), "text/css") == 0);
    assert(strcmp(get_mime_type("bundle.js"), "application/javascript") == 0);
    assert(strcmp(get_mime_type("api.json"), "application/json") == 0);
    assert(strcmp(get_mime_type("logo.png"), "image/png") == 0);
    assert(strcmp(get_mime_type("photo.jpg"), "image/jpeg") == 0);
    assert(strcmp(get_mime_type("photo.jpeg"), "image/jpeg") == 0);
    assert(strcmp(get_mime_type("anim.gif"), "image/gif") == 0);
    assert(strcmp(get_mime_type("vector.svg"), "image/svg+xml") == 0);
    assert(strcmp(get_mime_type("favicon.ico"), "image/x-icon") == 0);
    assert(strcmp(get_mime_type("notes.txt"), "text/plain; charset=utf-8") == 0);
    assert(strcmp(get_mime_type("archive.tar.gz"), "application/octet-stream") == 0);
    assert(strcmp(get_mime_type("binary.bin"), "application/octet-stream") == 0);
    printf("  -> MIME types PASS\n");
}

static void test_path_sanitization(void) {
    printf("[2/5] Testing path sanitization & traversal mitigation...\n");
    char path[HTTPD_PATH_MAX];
    copy_str(s_docroot, "/srv/http");

    /* 1. Normal file */
    assert(sanitize_path("/index.html", path, sizeof(path)) == true);
    assert(strcmp(path, "/srv/http/index.html") == 0);

    /* 2. Root maps to index.html */
    assert(sanitize_path("/", path, sizeof(path)) == true);
    assert(strcmp(path, "/srv/http/index.html") == 0);

    /* 3. Query string and fragment stripped */
    assert(sanitize_path("/test.txt?version=1#sec", path, sizeof(path)) == true);
    assert(strcmp(path, "/srv/http/test.txt") == 0);

    /* 4. Subdirectory path */
    assert(sanitize_path("/css/style.css", path, sizeof(path)) == true);
    assert(strcmp(path, "/srv/http/css/style.css") == 0);

    /* 5. Subdirectory trailing slash maps to index.html */
    assert(sanitize_path("/docs/", path, sizeof(path)) == true);
    assert(strcmp(path, "/srv/http/docs/index.html") == 0);

    /* 6. Directory traversal attempts MUST be rejected */
    assert(sanitize_path("/../etc/shadow", path, sizeof(path)) == false);
    assert(sanitize_path("/../../etc/passwd", path, sizeof(path)) == false);
    assert(sanitize_path("/subdir/../../secret", path, sizeof(path)) == false);
    assert(sanitize_path("/..", path, sizeof(path)) == false);

    /* 7. Missing leading slash */
    assert(sanitize_path("relative/path", path, sizeof(path)) == false);

    printf("  -> Path sanitization PASS\n");
}

static void test_request_handling_get(void) {
    printf("[3/5] Testing HTTP GET request parsing & 200 OK response...\n");
    copy_str(s_docroot, "/srv/http");

    http_client_t client;
    memset(&client, 0, sizeof(client));
    client.sock = s_mock_client_fd;
    client.file_fd = -1;
    client.state = CLIENT_STATE_READING_REQ;

    const char *raw_req = "GET /test.txt HTTP/1.1\r\nHost: localhost\r\nUser-Agent: test\r\n\r\n";
    copy_str(client.req_buf, raw_req);
    client.req_len = strlen(raw_req);

    handle_request(&client);

    assert(client.state == CLIENT_STATE_SENDING_HEADER);
    assert(strstr(client.resp_header, "HTTP/1.0 200 OK\r\n") != NULL);
    assert(strstr(client.resp_header, "Server: FortressOS-httpd/1.0\r\n") != NULL);
    assert(strstr(client.resp_header, "Content-Type: text/plain; charset=utf-8\r\n") != NULL);
    assert(strstr(client.resp_header, "Content-Length: 30\r\n") != NULL);
    assert(strstr(client.resp_header, "Connection: close\r\n") != NULL);
    assert(client.bytes_remaining == 30);
    assert(client.file_fd == s_mock_file_fd);
    assert(client.is_head == false);

    close_client(&client);
    printf("  -> GET 200 OK PASS\n");
}

static void test_request_handling_head(void) {
    printf("[4/5] Testing HTTP HEAD request parsing...\n");
    copy_str(s_docroot, "/srv/http");

    http_client_t client;
    memset(&client, 0, sizeof(client));
    client.sock = s_mock_client_fd;
    client.file_fd = -1;
    client.state = CLIENT_STATE_READING_REQ;

    const char *raw_req = "HEAD /index.html HTTP/1.0\r\n\r\n";
    copy_str(client.req_buf, raw_req);
    client.req_len = strlen(raw_req);

    handle_request(&client);

    assert(client.state == CLIENT_STATE_SENDING_HEADER);
    assert(strstr(client.resp_header, "HTTP/1.0 200 OK\r\n") != NULL);
    assert(strstr(client.resp_header, "Content-Type: text/html; charset=utf-8\r\n") != NULL);
    assert(client.is_head == true);

    close_client(&client);
    printf("  -> HEAD PASS\n");
}

static void test_error_responses(void) {
    printf("[5/5] Testing error responses (404, 403, 405)...\n");
    copy_str(s_docroot, "/srv/http");

    http_client_t client;

    /* 1. 404 Not Found */
    memset(&client, 0, sizeof(client));
    const char *req_404 = "GET /missing_file.html HTTP/1.0\r\n\r\n";
    copy_str(client.req_buf, req_404);
    client.req_len = strlen(req_404);
    handle_request(&client);
    assert(strstr(client.resp_header, "404 Not Found") != NULL);
    assert(client.is_error_body == true);
    assert(strstr((char *)client.file_buf, "404 Not Found") != NULL);

    /* 2. 403 Forbidden (traversal attempt) */
    memset(&client, 0, sizeof(client));
    const char *req_403 = "GET /../etc/passwd HTTP/1.0\r\n\r\n";
    copy_str(client.req_buf, req_403);
    client.req_len = strlen(req_403);
    handle_request(&client);
    assert(strstr(client.resp_header, "403 Forbidden") != NULL);
    assert(client.is_error_body == true);

    /* 3. 405 Method Not Allowed (POST) */
    memset(&client, 0, sizeof(client));
    const char *req_405 = "POST /api HTTP/1.0\r\n\r\n";
    copy_str(client.req_buf, req_405);
    client.req_len = strlen(req_405);
    handle_request(&client);
    assert(strstr(client.resp_header, "405 Method Not Allowed") != NULL);

    printf("  -> Error responses PASS\n");
}

int main(void) {
    printf("=========================================\n");
    printf("  FortressOS Tiny HTTP Server Host Tests \n");
    printf("=========================================\n");
    test_mime_resolution();
    test_path_sanitization();
    test_request_handling_get();
    test_request_handling_head();
    test_error_responses();
    printf("\n>>> ALL HTTPD HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
