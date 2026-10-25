# FortressOS — Wget v1.1 Architectural Plan & Specification (Issue #42)

**Status:** IN PROGRESS  
**Date:** 2026-10-25  
**Issue:** [#42](https://github.com/Pride1922/FortressOS/issues/42) (`wget v1.1: Bounded chunk streaming, redirection handling, and progress display.`)  
**Scope:** Ring 3 HTTP/HTTPS client extension (`/bin/wget`), RFC 7230 chunked framing, dot-segment redirect resolution, and stderr progress reporting.

---

## 1. Overview & Objectives

`/bin/wget` v1.1 enhances the FortressOS HTTP/HTTPS client with three production-grade capabilities:
1. **Live Progress Display**:
   - Real-time ASCII progress bar rendered to `stderr` (fd 2).
   - Shows percentage, transferred bytes, and total size when `Content-Length` is available.
   - Shows transferred bytes counter when downloading chunked or stream-delimited content.
   - Suppressed in quiet mode (`-q`) and never emitted to `stdout` (ensuring clean pipeline streaming `wget -O - | ...`).
   - Rate-limited to prevent UART / serial flooding.
2. **Robust Bounded Chunk Streaming (RFC 7230 §4.1)**:
   - Chunk size hex overflow protection (guards against 64-bit integer wrapping).
   - Bounded chunk extensions (max 1024 bytes per extension).
   - Support for RFC 7230 multi-line chunk trailers after the `0\r\n` terminal chunk until the terminating empty line.
   - Streaming directly to output without buffering unbounded payloads into memory, enforcing the 10 MiB cumulative cap.
3. **Enhanced Redirection Handling**:
   - Full support for 301, 302, 303, 307, and 308 redirect status codes.
   - Dot-segment normalization (`../` and `./`) in relative redirect URLs.
   - Bounded hops ($\le 5$) with circular loop detection and cumulative payload accounting.
   - Strict HTTPS to HTTP downgrade refusal.

---

## 2. Invariants & Guardrails

1. **Gate G1 Stack Ceiling ($\le 1024$ bytes):**
   - All state buffers (`s_io_buf`, `s_chunk_buf`, `s_header_buf`, url structs) remain in static BSS.
   - Progress bar rendering routines must use small stack buffers ($\le 128$ bytes).
2. **Stream Isolation (Pipeline Purity):**
   - Progress display MUST write only to file descriptor 2 (`stderr`).
   - Piped output (`-O -`) to file descriptor 1 (`stdout`) must strictly contain the exact binary response body.
3. **Memory Safety & Non-blocking I/O:**
   - Zero dynamic heap allocation (`malloc`/`free`) in Ring 3 client code.
   - Absolute BSP-tick deadlines or `SYS_POLL` multiplexing on network operations.

---

## 3. Implementation Blueprint

### 3.1 Progress Display Engine
In `user/wget.c`:
* Maintain transfer state: `bytes_transferred`, `total_expected` (if `has_content_length`), `last_reported_bytes`, `last_percent`.
* Function `wget_report_progress(uint64_t current, uint64_t total, bool has_total, bool force)`:
  - If `quiet` or `out_fd == 1 && !isatty(2)` (optional) -> return.
  - Rate limit: only update if `current - last_reported >= 16384` or percentage changes, or `force` is true.
  - Format:
    - Known total: `\r  [=========>         ]  45%  92160/204800`
    - Unknown total: `\r  [ <=>               ]  92160 bytes`
  - Flush with `\r` during transfer, and print `\n` on completion.

### 3.2 Chunked Decoder Hardening
In `user/wget_codec.c`:
* Hex overflow check:
  `if (dec->chunk_remaining > (UINT64_MAX >> 4)) return WGET_ERR_MALFORMED_CHUNK;`
* Chunk extension length limit:
  Track `ext_bytes`: if `ext_bytes > 1024`, return `WGET_ERR_MALFORMED_CHUNK`.
* Multi-line trailer state machine:
  Parse trailer header lines until `\r\n\r\n` or `\n\n` before transitioning to `WGET_CHUNK_STATE_DONE`.

### 3.3 Redirection Resolver Hardening
In `user/wget_codec.c`:
* Implement RFC 3986 path normalization helper `wget_normalize_path(char *path)`:
  - Resolves `.` (current directory) and `..` (parent directory).
  - Preserves root `/` prefix.

---

## 4. Verification & Testing

* **Host Unit Tests (`tests/wget_host.c`)**:
  - Test chunk size integer overflow rejection.
  - Test chunk extension length capping.
  - Test multi-line chunk trailer handling.
  - Test relative path normalization (`/a/b/c` + `../d.html` -> `/a/d.html`).
  - Test progress formatting.
* **QEMU Acceptance Testing (`scripts/test_wget.py`)**:
  - Verify chunked streaming, redirects, and progress display in guest execution.
