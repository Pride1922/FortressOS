# FortressOS Wget Design and Specification

## 1. Overview and Purpose

`/bin/wget` is a freestanding userspace HTTP client for FortressOS. It allows downloading files from local and internet HTTP servers via standard TCP sockets (`AF_INET`, `SOCK_STREAM`), with integrated DNS resolution, redirect handling, and direct-to-disk or stdout streaming.

## 2. Hard Bounds and Limits

To prevent memory exhaustion, unbounded loops, or buffer overflows in Ring 3, the following hard limits are enforced:

| Parameter | Limit | Rationale |
| :--- | :--- | :--- |
| **Max URL Length** | `1024` bytes | Fits bounded static buffer; standard practical limit |
| **Max `Location:` Header** | `1024` bytes | Matches URL buffer; accommodates long redirect URLs |
| **Max Header Size** | `8192` bytes | Protects static BSS against header exhaustion attacks |
| **Max Filename Length** | `256` bytes | Matches filesystem path component limit |
| **Max Redirect Hops** | `5` hops | Prevents infinite 301/302 redirect loops |
| **Response Body Size** | Unbounded | Direct-to-disk/stdout streaming; not buffered in memory |

## 3. Protocol Framing & Content-Length Semantics

- **Termination Condition**: The response body stream terminates **strictly on `recv() == 0`** (EOF from remote server).
- **HTTP Mode**: `HTTP/1.0` or `HTTP/1.1` with `Connection: close`.
- **Content-Length Handling**:
  - `Content-Length` is parsed if provided by the server.
  - Used for real-time transfer progress reporting (`bytes_received / total`).
  - **Post-download verification**:
    - If `Content-Length` was promised and the stream closes with fewer or more bytes received than promised, the downloaded bytes are retained on disk, but an error diagnostic is printed:
      `wget: read error: expected %lu bytes, got %lu`
    - The tool exits with a non-zero exit code (`1`).
    - This eliminates silent truncation.

## 4. URL and Scheme Rules

- **Supported Scheme**: `http://` (port 80 default, or custom `:port`).
- **HTTPS Scheme**: If URL starts with `https://`, `wget` immediately exits with code 1 and prints:
  `wget: HTTPS (TLS) is not supported yet; please use plain HTTP (http://...)`
- **Default Path**: If no path is provided in URL, default to `/`.
- **Default Filename**:
  - If `-O <file>` is specified: use `<file>` (`-` means stdout).
  - Otherwise, derive from the last path component (e.g. `http://host/dir/file.tar` -> `file.tar`).
  - If path is `/` or ends with `/`: default to `index.html`.

## 5. Ring 3 Architecture and Invariants

- **Static BSS Allocation**: The Ring 3 user stack is small (~4–8 KiB). All large buffers (8192-byte header buffer, 4096-byte I/O chunk buffer, URL structures, DNS contexts) **must reside in static BSS**, never on the stack frame.
- **Freestanding C**: No external libc. Simple, zero-allocation parsing routines.
- **Error Handling**: Graceful diagnostics for DNS resolution failures, connection refused, timeouts, header overflow, 4xx/5xx HTTP errors, and file write failures.

## 6. Verification & Acceptance Gates

1. **Host Unit Tests (`tests/wget_host.c`)**:
   - URL parser (valid URLs, custom ports, paths, filenames, HTTPS rejection, overflow rejection).
   - HTTP response parser (status line 200/301/404, 8 KiB header limit overflow, Content-Length, Location header).
   - ASan and UBSan clean.
2. **QEMU Integration Tests (`scripts/test_wget.py`)**:
   - Python HTTP server running on host.
   - Successful file download and byte-for-byte comparison.
   - 301/302 redirect traversal.
   - 404 Not Found error handling.
   - Content-Length mismatch detection (server sending less data than promised).
   - Piped output via `-O -`.
3. **Dell Latitude 5590 Bare-Metal Acceptance**:
   - **Primary Acceptance**: LAN HTTP server (`python3 -m http.server` on peer). Exact byte match.
   - **Secondary Acceptance**: Public HTTP server (`http://neverssl.com/` or `http://example.com/`). Proves end-to-end internet traversal.
