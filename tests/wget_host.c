#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../user/wget_codec.h"

static void test_url_parser(void) {
    printf("[TEST] URL Parser...\n");
    wget_url_t url;

    /* 1. Basic URL with trailing slash */
    assert(wget_parse_url("http://example.com/", &url) == WGET_OK);
    assert(strcmp(url.host, "example.com") == 0);
    assert(url.port == 80);
    assert(strcmp(url.path, "/") == 0);
    assert(strcmp(url.filename, "index.html") == 0);

    /* 2. Basic URL without trailing slash */
    assert(wget_parse_url("http://example.com", &url) == WGET_OK);
    assert(strcmp(url.host, "example.com") == 0);
    assert(url.port == 80);
    assert(strcmp(url.path, "/") == 0);
    assert(strcmp(url.filename, "index.html") == 0);

    /* 3. Custom port and file path */
    assert(wget_parse_url("http://192.168.0.153:8000/test.txt", &url) == WGET_OK);
    assert(strcmp(url.host, "192.168.0.153") == 0);
    assert(url.port == 8000);
    assert(strcmp(url.path, "/test.txt") == 0);
    assert(strcmp(url.filename, "test.txt") == 0);

    /* 4. Subdirectory and query parameter */
    assert(wget_parse_url("http://host.domain:8080/files/archive.tar.gz?token=abc", &url) == WGET_OK);
    assert(strcmp(url.host, "host.domain") == 0);
    assert(url.port == 8080);
    assert(strcmp(url.path, "/files/archive.tar.gz?token=abc") == 0);
    assert(strcmp(url.filename, "archive.tar.gz") == 0);

    /* 5. URL without scheme prefix */
    assert(wget_parse_url("myserver:9000/data.bin", &url) == WGET_OK);
    assert(strcmp(url.host, "myserver") == 0);
    assert(url.port == 9000);
    assert(strcmp(url.path, "/data.bin") == 0);
    assert(strcmp(url.filename, "data.bin") == 0);

    /* 6. Userinfo in URL (user:pass@host) */
    assert(wget_parse_url("http://admin:secret@10.0.2.2/status", &url) == WGET_OK);
    assert(strcmp(url.host, "10.0.2.2") == 0);
    assert(url.port == 80);
    assert(strcmp(url.path, "/status") == 0);
    assert(strcmp(url.filename, "status") == 0);

    /* 7. HTTPS rejection */
    assert(wget_parse_url("https://secure.example.com/index.html", &url) == WGET_ERR_HTTPS);

    /* 8. Unsupported scheme rejection */
    assert(wget_parse_url("ftp://files.example.com/file", &url) == WGET_ERR_UNSUPPORTED_SCHEME);

    /* 9. Invalid ports */
    assert(wget_parse_url("http://example.com:0/test", &url) == WGET_ERR_INVALID_PORT);
    assert(wget_parse_url("http://example.com:70000/test", &url) == WGET_ERR_INVALID_PORT);
    assert(wget_parse_url("http://example.com:abc/test", &url) == WGET_ERR_INVALID_PORT);

    /* 10. Empty host */
    assert(wget_parse_url("http://:8080/test", &url) == WGET_ERR_INVALID_URL);
    assert(wget_parse_url("http:///test", &url) == WGET_ERR_INVALID_URL);

    /* 11. Excessively long URL */
    char long_url[WGET_MAX_URL_LEN + 32];
    memset(long_url, 'a', sizeof(long_url) - 1);
    long_url[sizeof(long_url) - 1] = '\0';
    memcpy(long_url, "http://", 7);
    assert(wget_parse_url(long_url, &url) == WGET_ERR_URL_TOO_LONG);

    printf("  -> URL Parser: PASS\n");
}

static void test_header_delimiter(void) {
    printf("[TEST] Header Delimiter Scanner...\n");
    size_t hdr_len = 0;

    /* 1. CRLF CRLF delimiter */
    const char *h1 = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nHELLO";
    assert(wget_find_header_end((const uint8_t *)h1, strlen(h1), &hdr_len) == WGET_OK);
    assert(hdr_len == strlen("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"));
    assert(memcmp(h1 + hdr_len, "HELLO", 5) == 0);

    /* 2. LF LF delimiter */
    const char *h2 = "HTTP/1.0 200 OK\nContent-Length: 4\n\nBODY";
    assert(wget_find_header_end((const uint8_t *)h2, strlen(h2), &hdr_len) == WGET_OK);
    assert(hdr_len == strlen("HTTP/1.0 200 OK\nContent-Length: 4\n\n"));
    assert(memcmp(h2 + hdr_len, "BODY", 4) == 0);

    /* 3. Incomplete header */
    const char *h3 = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n";
    assert(wget_find_header_end((const uint8_t *)h3, strlen(h3), &hdr_len) == WGET_NEED_MORE_DATA);

    /* 4. Header overflow (8192 bytes without delimiter) */
    uint8_t big_hdr[WGET_MAX_HEADER_BYTES + 10];
    memset(big_hdr, 'A', sizeof(big_hdr));
    assert(wget_find_header_end(big_hdr, sizeof(big_hdr), &hdr_len) == WGET_ERR_HEADER_OVERFLOW);

    printf("  -> Header Delimiter Scanner: PASS\n");
}

static void test_response_parser(void) {
    printf("[TEST] Response Header Parser...\n");
    wget_response_t resp;

    /* 1. 200 OK with Content-Length */
    const char *r1 = "HTTP/1.1 200 OK\r\n"
                    "Date: Fri, 02 Oct 2026 21:00:00 GMT\r\n"
                    "Server: FortressTest/1.0\r\n"
                    "Content-Length: 4096\r\n"
                    "Content-Type: text/html\r\n"
                    "\r\n";
    assert(wget_parse_response_headers((const uint8_t *)r1, strlen(r1), &resp) == WGET_OK);
    assert(resp.status_code == 200);
    assert(strcmp(resp.status_text, "OK") == 0);
    assert(resp.has_content_length == true);
    assert(resp.content_length == 4096);
    assert(resp.header_len == strlen(r1));

    /* 2. 301 Moved Permanently with Location */
    const char *r2 = "HTTP/1.0 301 Moved Permanently\r\n"
                    "Location: http://neverssl.com/online\r\n"
                    "Content-Length: 0\r\n"
                    "\r\n";
    assert(wget_parse_response_headers((const uint8_t *)r2, strlen(r2), &resp) == WGET_OK);
    assert(resp.status_code == 301);
    assert(strcmp(resp.status_text, "Moved Permanently") == 0);
    assert(strcmp(resp.location, "http://neverssl.com/online") == 0);

    /* 3. 404 Not Found without Content-Length */
    const char *r3 = "HTTP/1.1 404 Not Found\r\n"
                    "Connection: close\r\n"
                    "\r\n";
    assert(wget_parse_response_headers((const uint8_t *)r3, strlen(r3), &resp) == WGET_OK);
    assert(resp.status_code == 404);
    assert(strcmp(resp.status_text, "Not Found") == 0);
    assert(resp.has_content_length == false);

    /* 4. Case-insensitivity of header keys */
    const char *r4 = "HTTP/1.1 200 Success\r\n"
                    "content-length: 9999\r\n"
                    "location: /redirected\r\n"
                    "\r\n";
    assert(wget_parse_response_headers((const uint8_t *)r4, strlen(r4), &resp) == WGET_OK);
    assert(resp.status_code == 200);
    assert(resp.has_content_length == true);
    assert(resp.content_length == 9999);
    assert(strcmp(resp.location, "/redirected") == 0);

    /* 5. Malformed header lines */
    const char *bad1 = "INVALID RESPONSE\r\n\r\n";
    assert(wget_parse_response_headers((const uint8_t *)bad1, strlen(bad1), &resp) == WGET_ERR_MALFORMED_HEADER);

    const char *bad2 = "HTTP/1.1 ABC Bad Code\r\n\r\n";
    assert(wget_parse_response_headers((const uint8_t *)bad2, strlen(bad2), &resp) == WGET_ERR_MALFORMED_HEADER);

    printf("  -> Response Header Parser: PASS\n");
}

static void test_redirect_resolver(void) {
    printf("[TEST] Redirect Resolver...\n");
    wget_url_t cur, target;

    assert(wget_parse_url("http://example.com:8080/docs/guide.html", &cur) == WGET_OK);

    /* 1. Absolute URL redirect */
    assert(wget_resolve_redirect(&cur, "http://mirror.org/guide.html", &target) == WGET_OK);
    assert(strcmp(target.host, "mirror.org") == 0);
    assert(target.port == 80);
    assert(strcmp(target.path, "/guide.html") == 0);
    assert(strcmp(target.filename, "guide.html") == 0);

    /* 2. Absolute path redirect (same host and port) */
    assert(wget_resolve_redirect(&cur, "/v2/guide.html", &target) == WGET_OK);
    assert(strcmp(target.host, "example.com") == 0);
    assert(target.port == 8080);
    assert(strcmp(target.path, "/v2/guide.html") == 0);
    assert(strcmp(target.filename, "guide.html") == 0);

    /* 3. Relative path redirect */
    assert(wget_resolve_redirect(&cur, "faq.html", &target) == WGET_OK);
    assert(strcmp(target.host, "example.com") == 0);
    assert(target.port == 8080);
    assert(strcmp(target.path, "/docs/faq.html") == 0);
    assert(strcmp(target.filename, "faq.html") == 0);

    /* 4. Scheme-relative redirect */
    assert(wget_resolve_redirect(&cur, "//cdn.example.com/assets/logo.png", &target) == WGET_OK);
    assert(strcmp(target.host, "cdn.example.com") == 0);
    assert(target.port == 80);
    assert(strcmp(target.path, "/assets/logo.png") == 0);
    assert(strcmp(target.filename, "logo.png") == 0);

    /* 5. HTTPS redirect rejection */
    assert(wget_resolve_redirect(&cur, "https://example.com/secure", &target) == WGET_ERR_HTTPS);

    printf("  -> Redirect Resolver: PASS\n");
}

int main(void) {
    printf("=========================================\n");
    printf("  FortressOS Wget Host Unit Test Suite   \n");
    printf("=========================================\n");

    test_url_parser();
    test_header_delimiter();
    test_response_parser();
    test_redirect_resolver();

    printf("\n>>> ALL WGET HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
