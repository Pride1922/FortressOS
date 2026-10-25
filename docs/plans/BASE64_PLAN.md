# FortressOS Base64 CLI Utility Plan (Issue #45)

## 1. Objective and Requirements

Implement a robust, freestanding, POSIX/GNU-compatible `/bin/base64` CLI utility for FortressOS adhering to:
- **RFC 4648 Base64 Specification**: Alphabet `A-Z`, `a-z`, `0-9`, `+`, `/`, with `=` padding.
- **Streaming Architecture**: Fixed stream buffers in BSS (`4096` bytes input, `4096` bytes output) enabling arbitrarily large file/pipe encoding and decoding without memory exhaustion.
- **Zero Dynamic Allocations**: No heap allocations (`malloc`/`free`).
- **Gate G1 Stack Budget**: Stack frames $\le 1024$ bytes per function.
- **Command Line Interface**:
  - `base64 [OPTION]... [FILE]`
  - `-d`, `--decode`: Decode data.
  - `-i`, `--ignore-garbage`: When decoding, ignore non-alphabet characters.
  - `-w COLS`, `--wrap=COLS`: Wrap encoded lines after `COLS` characters (default 76, `0` disables wrapping).
  - `-h`, `--help`: Display usage and exit.
  - Input from `FILE` or standard input (`-` or omitted).
  - Clean error exit on invalid input, unreadable files, or storage errors.

---

## 2. Architecture & Stream Pipeline

### 2.1 State Management Across Buffer Boundaries

Streaming base64 requires preserving intermediate state across read calls (`TOOL_BUFFER_SIZE = 4096` bytes):

```
       +--------------------+      +--------------------+
Input  | Stream Read Buffer | ---> | Encoder / Decoder  | ---> Write Buffer
Stream |   (4096 bytes)     |      |  Carryover State   |      (4096 bytes)
       +--------------------+      +--------------------+
```

#### Encoder State:
- `carry[2]`: Up to 2 bytes leftover from previous chunk when input is not a multiple of 3.
- `carry_len`: 0, 1, or 2.
- `col_count`: Columns printed on current line for wrapping (reset upon `\n`).
- `wrap_cols`: Target column limit (default 76, 0 = disabled).

#### Decoder State:
- `carry[4]`: Up to 3 raw base64 quintets accumulated across chunk boundaries.
- `carry_len`: 0 to 3.
- `ignore_garbage`: Boolean flag (`-i`).
- `saw_pad`: Boolean tracking if padding character `=` was observed.

---

## 3. Invariants & Security Boundaries

1. **Gate G1 Compliance**: Buffer arrays reside in static BSS storage, not on the stack. Function stack frames stay well under 256 bytes.
2. **Padding Integrity**:
   - In decode mode, single `=` requires 2 valid preceding characters; double `==` requires 1 valid preceding character.
   - Any non-zero unused bits in the final quantum are strictly rejected as invalid input (unless `-i` specified where non-alphanumeric chars are ignored, but padding bits must still remain consistent).
3. **Fail-Closed on Malformed Input**:
   - Non-alphabet characters without `-i` immediately stop decoding with `base64: invalid input` and return exit code 1.
   - Incomplete base64 streams (e.g. 1 leftover character at EOF) return exit code 1.

---

## 4. Verification Plan

1. **Host Unit Tests (`make test-base64-host`)**:
   - RFC 4648 canonical vectors (empty, "f", "fo", "foo", "foob", "fooba", "foobar").
   - Binary arbitrary data (0x00..0xFF patterns).
   - Split-chunk stress testing (feeding input in 1-byte, 2-byte, 3-byte chunks to test carryover across boundaries).
   - Wrap formatting at 76, custom `-w 12`, `-w 0`.
   - Decoding with embedded CR, LF, CRLF, and spaces.
   - Rejection of invalid characters without `-i`, and tolerance with `-i`.
   - Trailing garbage and truncation detection.
2. **System Integration & QEMU Acceptance**:
   - Integration into `USER_STREAM_TOOLS` in `Makefile`.
   - Live execution in shell pipelines (`echo "hello" | /bin/base64 | /bin/base64 -d`).
   - File round-trip verification under BIOS and UEFI.
