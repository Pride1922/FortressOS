# FortressOS Nano Design and Implementation Plan

Status (2026-10-03): implemented by Gemini. Host and BIOS/UEFI automated acceptance are recorded in AGENTS.md; Dell hardware acceptance remains pending. Host sanitizer tests were rerun successfully before the combined tools commit. Direct file writes are not an atomic replacement guarantee.

## 1. Overview and Purpose

`/bin/nano` is a standalone, full-screen, non-modal visual text editor for FortressOS. It provides an intuitive, user-friendly editing experience inspired by GNU nano and antirez's Kilo, running entirely in Ring 3.

### Core Goals
- **Full-Screen Interactive Visual Editing:** Direct on-screen character typing, real-time cursor tracking, vertical and horizontal scrolling.
- **Non-Modal Editing:** No confusing modal state switches; users open a file and start typing immediately.
- **Standalone ELF (`/bin/nano`):** Lives in `/bin/` as an isolated user executable. The shell builtin `edit` (the lightweight line-oriented ed-lite) remains completely intact as a recovery tool.
- **Freestanding and Safe:** Built strictly using FortressOS freestanding C and syscall ABIs, without external libc or dynamically expanding kernel heap.

---

## 2. Memory Model and Hard Bounds

To strictly respect FortressOS's Ring 3 constraints (no `SYS_BRK` or `SYS_MMAP` syscalls; small user stack frames), `/bin/nano` uses a **pure static BSS allocation model**.

| Parameter | Limit | Rationale |
| :--- | :--- | :--- |
| **Text Pool Size** | `262,144` bytes (256 KiB) | Accommodates >5,000 lines of configuration or code; zero heap allocation |
| **Max Line Count** | `4,096` rows | Covers large files while keeping row metadata table compact (~64 KiB) |
| **Max Line Length** | `1,024` chars | Bounded row length prevents single-line buffer runaway |
| **Filename Length** | `256` bytes | Matches VFS path component limit |
| **Search Query Length**| `64` bytes | Fits prompt bar without overflowing terminal width |
| **Line Clipboard** | `1,024` bytes | Single-line cut/paste buffer |

### File Size Defense
If a user attempts to open a file exceeding 256 KiB, `/bin/nano` cleanly displays:
```text
nano: file exceeds buffer capacity (max 256 KiB)
```
and exits with status `1` without corrupting memory or hanging.

---

## 3. Buffer and Data Structure Architecture

Instead of a fragmented dynamic linked list, the text buffer consists of a flat static character pool indexed by an array of row descriptors:

```c
typedef struct {
    uint32_t offset;    // Offset of line start within s_text_pool
    uint16_t length;    // Byte length of this line (excluding newline)
    uint16_t render_len;// Visual width on screen (expanding tabs to spaces)
} nano_row_t;

typedef struct {
    nano_row_t rows[NANO_MAX_ROWS];
    size_t     num_rows;
    size_t     total_bytes;
    
    /* Cursor in file coordinate space (0-indexed) */
    size_t     cx;       // File column index
    size_t     cy;       // File row index
    
    /* Viewport scrolling offsets */
    size_t     row_offset; // Top line visible on screen
    size_t     col_offset; // Left column visible on screen
    
    /* Terminal dimensions (queried via SYS_TERMCTL) */
    uint32_t   screen_rows;
    uint32_t   screen_cols;
    
    /* Editor state */
    bool       modified;
    char       filename[256];
    char       status_msg[80];
    uint64_t   status_time_ticks;
    
    /* Line clipboard (Ctrl+K / Ctrl+U) */
    char       cut_buffer[1024];
    uint16_t   cut_len;
} nano_state_t;
```

---

## 4. Visual Layout (80 × 25 or Auto-Detected)

```text
+------------------------------------------------------------------------------+
| [ FortressOS Nano 1.0 ]          File: /mnt/network.conf          [Modified] |  <- Row 1: Header (Inverted/Bold)
|address 10.0.2.15/24                                                          |  <- Rows 2 .. H-2: Text Viewport
|gateway 10.0.2.2                                                              |
|dns 10.0.2.3                                                                  |
|~                                                                             |
|~                                                                             |
|[ Wrote 3 lines, 48 bytes ]                                                   |  <- Row H-1: Status Message Bar
|^O WriteOut   ^X Exit   ^W WhereIs   ^K Cut   ^U Uncut   ^L Redraw            |  <- Row H: Shortcut Legend
+------------------------------------------------------------------------------+
```

1. **Header Bar (Row 1):** Inverted/reverse video (`\x1b[7m`). Displays program version, active filename (or `[New Buffer]`), and `[Modified]` badge.
2. **Text Area (Rows 2 to `H-2`):** Text viewport. Empty lines beyond EOF render with a subtle `~`.
3. **Status Bar (Row `H-1`):** Displays messages, confirmations, and interactive input prompts (`File Name to Write: /mnt/network.conf`).
4. **Shortcut Legend (Row `H`):** Clear two-column or row legend of the primary Ctrl shortcuts.

---

## 5. Keyboard Navigation and Editing Semantics

### Navigation
- **Arrow Keys:** Left/Right/Up/Down navigation.
- **Home / `Ctrl+A`:** Move cursor to beginning of current line.
- **End / `Ctrl+E`:** Move cursor to end of current line.
- **Page Up / Page Down:** Scroll full viewport up or down.

### In-Memory Editing
- **Direct Typing:** ASCII printable characters (32..126) insert at current `(cx, cy)`.
- **Backspace (`0x08` / `0x7F`):**
  - If `cx > 0`: Deletes character at `cx - 1`.
  - If `cx == 0` and `cy > 0`: Merges current line with line `cy - 1`. Cursor moves to end of previous line.
- **Delete (`\x1b[3~`):**
  - If `cx < row_len`: Deletes character at `cx`.
  - If `cx == row_len` and `cy + 1 < num_rows`: Merges line `cy + 1` into line `cy`.
- **Enter (`\n`):**
  - Splits current line at `cx` into two rows.
  - Cursor advances to column 0 of line `cy + 1`.

### Commands and File I/O
- **`Ctrl+O` (WriteOut):**
  - Prompts at status bar: `File Name to Write: <filename>`.
  - Pressing `Enter` commits the write:
    1. Updates status bar to `[ Writing... ]` and forces screen flush.
    2. Opens file with `SYS_OPEN(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644)`.
    3. Writes buffer in bounded 4096-byte chunks via `SYS_WRITE`.
    4. Calls `SYS_SYNC` to flush ext2 data blocks to disk.
    5. Closes file descriptor via `SYS_CLOSE`.
    6. Updates status bar to `[ Wrote N lines, B bytes ]` and clears `modified` flag.
- **`Ctrl+X` (Exit):**
  - If buffer is unmodified: cleans terminal screen and exits with code 0.
  - If buffer is modified: prompts `Save modified buffer? (Y/N/C)`.
    - `Y`: Prompts filename and saves, then exits.
    - `N`: Discards changes and exits immediately.
    - `C` / `Ctrl+C`: Cancels exit and returns to editor.
- **`Ctrl+W` (Where Is / Search):**
  - Prompts: `Search: <query>`.
  - Performs case-insensitive forward search starting from current cursor position.
  - Jumps cursor to match and adjusts `row_offset` / `col_offset` to center viewport.
- **`Ctrl+K` (Cut Line):**
  - Deletes current line and copies its contents into `cut_buffer`.
- **`Ctrl+U` (Uncut / Paste Line):**
  - Inserts `cut_buffer` as a new line above current cursor.
- **`Ctrl+L` (Redraw):**
  - Forces terminal screen clear (`\x1b[2J\x1b[H`) and full frame repaint.

---

## 6. Phased Implementation Plan

### Phase 0: Host-Test Harness (`tests/nano_host.c`) — COMPLETE
- **Scope:** Pure in-memory buffer engine tested on Linux/WSL under ASan/UBSan.
- **Test Matrix:**
  - Initial load of empty buffer and multiline text.
  - Insert characters, beginning, middle, and end of line.
  - Row splitting on `Enter` across various column positions.
  - Row merging on `Backspace` at column 0.
  - Deleting characters at EOF / BOF.
  - Boundary saturation: inserting up to 256 KiB ceiling and rejecting overflow cleanly.
  - Tab expansion and coordinate calculation.
- **Deliverable:** `make test-nano-host` (100% PASS under ASan/UBSan 2026-10-03).

### Phase 1: Read-Only Viewer and Viewport Engine (`user/nano.c`) — COMPLETE
- **Scope:** Full-screen rendering and cursor navigation.
- **Implementation:**
  - Load file from disk into static BSS text pool.
  - Query terminal rows/cols with `SYS_TERMCTL`.
  - Render top bar, text area, status bar, and shortcut legend.
  - Arrow keys, Home, End, PageUp, PageDown navigation.
  - Viewport scrolling (vertical scroll when `cy` moves off screen; horizontal scroll when `cx > screen_cols`).
  - Clean exit on `Ctrl+X` (clears screen, restores cursor).

### Phase 2: In-Memory Editing and Row Mutation — COMPLETE
- **Scope:** Interactive typing, line splitting, line merging, and dirty tracking.
- **Implementation:**
  - Wire keyboard input characters (ASCII 32..126) to buffer insert.
  - Implement Backspace (char delete / line merge).
  - Implement Delete (char delete / forward line merge).
  - Implement Enter (row split).
  - Track `modified` flag and display `[Modified]` in top header bar.

### Phase 3: File I/O and Interactive Prompts — COMPLETE
- **Scope:** Safe saving and exit confirmation workflow.
- **Implementation:**
  - Status bar input prompt reader (for filename input and Y/N prompts).
  - `Ctrl+O` write workflow: `[ Writing... ]` -> direct `O_TRUNC` write -> `SYS_SYNC` -> `[ Wrote N lines, B bytes ]`.
  - `Ctrl+X` unsaved buffer confirmation dialog (`Save modified buffer? (Y/N/C)`).
  - Error diagnostics for read-only filesystem or write failure.

### Phase 4: Productivity Polish and System Integration — COMPLETE
- **Scope:** Search, cut/paste, build integration, and automated verification.
- **Implementation:**
  - `Ctrl+W` forward search with viewport jump.
  - `Ctrl+K` cut line and `Ctrl+U` paste line clipboard.
  - Makefile target: build `bin/nano` and package into `bin/initramfs.tar`.
  - Automated QEMU integration test (`scripts/test_nano.py`):
    - Launch `/bin/nano /mnt/test_nano.txt`.
    - Type text, save via `^O`, exit via `^X`.
    - Verify file contents on ext2 via `cat /mnt/test_nano.txt`.
    - Reopen, edit, and exit with save prompt (`^X -> Y`).
    - Test unsaved buffer discard (`^X -> N`).
  - Updated `COMMANDS.md`, `README.md`, and `AGENTS.md`.
  - **Deliverable:** `make test-nano` (100% PASS under BIOS and UEFI 2026-10-03).

---

## 7. Acceptance Criteria

1. **Host Verification:** `make test-nano-host` passes 100% under ASan and UBSan with zero memory leaks (VERIFIED 2026-10-03).
2. **QEMU Verification:** `python3 scripts/test_nano.py` (`make test-nano`) passes under both BIOS and UEFI (VERIFIED 2026-10-03).
3. **Bare-Metal Usability:** Confirmed interactive editing, file saving, and persistence across reboots on physical Dell Latitude hardware (Pending bare-metal boot session).
