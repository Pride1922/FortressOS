# FortressOS Tiny HTTP Server Plan (Issue #43)

## 1. Objective and Requirements

Implement `/bin/httpd`, a robust, single-threaded, non-blocking static file HTTP web server for FortressOS adhering to:
- **Single-Threaded Non-Blocking Architecture**: Exclusively uses non-blocking sockets (`NET_SOCK_NONBLOCK`), non-blocking accept, and `SYS_POLL` multiplexing across listener and concurrent client connections. Zero multi-threading or worker processes required.
- **Static File Serving**: Serves static assets from a designated document root directory (`-d <path>`, default `.`).
- **HTTP/1.0 & HTTP/1.1 Subsets**: Supports `GET` and `HEAD` methods, standard status codes (`200 OK`, `400 Bad Request`, `403 Forbidden`, `404 Not Found`, `405 Method Not Allowed`, `500 Internal Server Error`, `503 Service Unavailable`).
- **Security & Path Sanitization**: Strict directory traversal prevention (rejection of `..`, null bytes, absolute escape paths).
- **MIME Types**: Extension-based MIME detection (`text/html`, `text/plain`, `text/css`, `application/javascript`, `application/json`, `image/png`, `image/jpeg`, `application/octet-stream`, etc.).
- **Zero Dynamic Allocations**: No heap allocations (`malloc`/`free` omitted). Fixed client slots and streaming buffers in BSS.
- **Gate G1 Stack Budget**: All stack frames strictly $\le 512$ bytes (well below the 1024-byte ceiling).

---

## 2. State Machine & Event-Driven Architecture

```
[Incoming TCP Connection]
           |
      SYS_ACCEPT (NET_SOCK_NONBLOCK | NET_SOCK_CLOEXEC)
           |
           v
   +---------------------------+
   | CLIENT_STATE_READING_REQ  | <---+ (POLLIN, non-blocking read)
   +---------------------------+     |
           |                         |
   (Full request header received)----+
           |
           v
   +---------------------------+
   | CLIENT_STATE_SENDING_HDR  | <---+ (POLLOUT, non-blocking write)
   +---------------------------+     |
           |                         |
    (Header sent)--------------------+
      /         \
   (HEAD)     (GET)
    /             \
   v               v
 [CLOSE]   +---------------------------+
           | CLIENT_STATE_SENDING_BODY | <---+ (POLLOUT, non-blocking write)
           +---------------------------+     |
                   |                         |
             (Body sent)---------------------+
                   |
                   v
                [CLOSE]
```

---

## 3. Security Boundaries & Invariants

1. **Path Traversal Mitigation**:
   - Any URI containing `..` path segments (`/../`, `/..`, `../`) returns `403 Forbidden`.
   - Hidden or forbidden access checks verify the resolved path remains rooted under `docroot`.
2. **Buffer Limits & Slowloris Resistance**:
   - Maximum request header size: 2048 bytes. Exceeding without finding header delimiter terminates with `400 Bad Request`.
   - Inactivity timeout: Connections idle for $> 10$ seconds are automatically closed.
3. **Socket Table Invariant**:
   - Client table size: 8 concurrent connections, respecting `NET_SOCKET_MAX = 16`.
   - When table is full, additional connections are rejected with `503 Service Unavailable`.

---

## 4. Verification Plan

1. **Host Unit & Mock Test Suite (`make test-httpd-host`)**:
   - Request parser: valid GET, HEAD, malformed lines, path query stripping, directory mapping to index.html.
   - Path security: traversal attempts (`/../../etc/shadow`, `/a/../b`) return 403.
   - MIME type resolution.
   - Non-blocking multiplexing simulation under ASan/UBSan.
2. **QEMU System Integration (`make test-httpd`)**:
   - Multi-firmware execution under BIOS and UEFI.
   - Client interaction via `/bin/wget` and `/bin/nc` against `/bin/httpd`:
     - Fetch `GET /` and verify content.
     - Fetch `HEAD /` and verify headers without body.
     - 404 Not Found on missing file.
     - 403 Forbidden on directory traversal attempt (`wget http://localhost:8080/../etc/passwd`).
     - Concurrent request handling over non-blocking loop.
     - Clean poweroff.
