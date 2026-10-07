# FortressOS Nano v2.0 & v2.1 Architecture and Implementation Plan

## 1. Overview & Vision

`/bin/nano` is FortressOS's standalone, freestanding visual editor. 
- **Nano v1.0** established the baseline visual editor (static BSS 256 KiB pool, row metadata table, basic navigation, save/exit workflows).
- **Nano v1.1 (Polish)** resolved rendering flicker, eliminated erase-before-write line blanking, added instant cursor-only navigation, status bar prompt cursor tracking, and transient status auto-clearing.
- **Nano v2.0 (Essentials)** upgrades nano into a robust, daily-driver text editor with essential editing features (Undo/Redo, Line Numbers, Search & Replace, Go to Line, Word-wise motion, Pipes, Readonly, CRLF).
- **Nano v2.1 (Power-User)** introduces multiple buffers, config file parsing (`/etc/nanorc`), tab configuration, and regex search powered by the existing Pike VM NFA engine.

```
+---------------------------------------------------------------------------------------+
|  v1.0 (Foundation)     -->  v1.1 (Polish)        -->  v2.0 (Essentials)   --> v2.1... |
|  - 256 KiB BSS pool         - Event-driven draw       - Undo / Redo           - Multi |
|  - Row index table          - In-place overwrite      - Line numbers          - Regex |
|  - Basic nav & cut          - Fast cursor jump        - Search & Replace      - nanorc|
|  - File save/load           - Status prompt cursor    - Go to line            - Tabs  |
|                             - Ctrl+C position         - Word motion                   |
|                                                       - Readonly / Pipes              |
+---------------------------------------------------------------------------------------+
```

---

## 2. Hard Bounds and Freestanding Constraints

In keeping with FortressOS Ring 3 guidelines:
- **Zero dynamic allocations:** No `malloc()`, `SYS_BRK`, or `SYS_MMAP`. All tables and buffers live in `.bss`.
- **Stack discipline:** Frame sizes strictly $< 1 \text{ KiB}$. No deep recursive calls.
- **Bounded memory budget:**
  - Active text pool: 256 KiB (`NANO_POOL_SIZE`).
  - Row descriptor table: 4096 rows (`NANO_MAX_ROWS`).
  - Undo/Redo journal: 32 operations in static ring buffer (~16 KiB total).
  - Multi-buffer table: 4 buffers maximum (v2.1).

---

## 3. Phased Implementation Roadmap

```mermaid
flowchart TD
    subgraph Phase 1: Correctness & Safe Operations [Phase 1: v2.0 Part 1]
        P1A["C4: Readonly Mode (-R, --view)"]
        P1B["C5: Pipe / Stdin Stream (nano -)"]
        P1C["C3: High Capacity Proximity Warning"]
        P1D["C2: CRLF Detection & [DOS]/[Unix] Indicator"]
    end

    subgraph Phase 2: Core Editing Essentials [Phase 2: v2.0 Part 2]
        P2A["A2: Line Numbers (-l & Alt+N Gutter)"]
        P2B["A4: Go to Line (Ctrl+G)"]
        P2C["A5: Word-wise Cursor Motion (Ctrl+Arrows, Alt+F/B)"]
        P2D["A3: Search & Replace (Ctrl+R interactive)"]
        P2E["A1: Bounded Undo / Redo Journal (Ctrl+Z / Ctrl+Y)"]
    end

    subgraph Phase 3: Power-User & Persistence [Phase 3: v2.1]
        P3A["B2: Case Sensitivity Toggle in Search"]
        P3B["B3: Regex Search Engine (Pike VM reuse)"]
        P3C["B5 & B6: Config File & Tab Config (/etc/nanorc)"]
        P3D["B1: Multi-Buffer Document Switching (Alt+< / Alt+>)"]
    end

    Phase 1 --> Phase 2
    Phase 2 --> Phase 3
```

---

## 4. Phase 1: Correctness, Flags & Safe Operations (v2.0 Part 1)

### 4.1 Feature C4: Read-Only Mode (`-R` / `--view`)
- **CLI Flag:** `/bin/nano -R <file>` or `view <file>`.
- **Semantics:**
  - Sets `s_state.readonly = true`.
  - All mutating keystrokes (typing characters, Enter, Backspace, Delete, Cut line, Paste line) are blocked and trigger `[ Buffer is read-only ]`.
  - Top header displays `[Read-Only]` in reverse video badge.
  - `Ctrl+O` writeout is blocked: `[ Cannot write in read-only mode ]`.
  - `Ctrl+X` exits immediately without prompting to save.

### 4.2 Feature C5: Pipe & Standard Stream Integration (`nano -`)
- **CLI Invocation:** `dmesg | /bin/nano -` or `cat config.txt | /bin/nano -`.
- **Input Strategy:**
  - Detect `argv[1] == "-"`.
  - Before initializing terminal UI on fd 1, read all available bytes from fd 0 via `sys_read(0, ...)` until EOF (`rd == 0`).
  - Populate `s_text_pool` and parse rows via `nano_load_buffer()`.
  - Filename set to `[Standard Input]`.
  - If output is a terminal (`SYS_TERMCTL(TERM_ISATTY)`), reopen interactive input descriptor via `/dev/tty` (or fd 31) to allow interactive navigation and review of piped input.
  - `Ctrl+O` prompts for a destination path before saving.

### 4.3 Feature C3: Buffer Proximity Warning
- **Thresholds:**
  - Warning alert when text pool exceeds 80% (209,715 bytes) or rows exceed 3,500.
  - Status bar displays: `[ Warning: Buffer at 85% capacity (222 KB / 256 KB) ]`.
  - When user reaches exact saturation limit (262,144 bytes or 4,096 lines), mutations stop cleanly with `[ Buffer full: cannot insert ]` without corrupting pool pointers or hanging.

### 4.4 Feature C2: CRLF Detection & Line Ending Display
- **Detection:** During `nano_load_buffer()`, count `\r\n` pairs vs lone `\n`.
  - If CRLF is prevalent: `s_state.dos_mode = true`.
  - Top header bar renders `[DOS]` or `[Unix]`.
- **Saving:** When writing file in `save_file()`, write `\r\n` if `dos_mode` is set, else `\n`.
- **Conversion:** Status toggle `Alt+D` toggles between DOS and Unix line endings.

---

## 5. Phase 2: Editing Essentials (v2.0 Part 2)

### 5.1 Feature A2: Line Number Gutter (`-l` & `Alt+N`)
- **Gutter-Aware Viewport Math:**
  Adding a gutter shrinks the usable text viewport. The geometry is maintained throughout rendering and cursor calculation:
  - Dynamic gutter width:
    $$G = \text{digits}(\text{num\_rows}) + 3 \quad (\text{minimum } 5 \text{ cols, e.g. } \text{"  1 | "})$$
    When line numbers are disabled, $G = 0$.
  - Viewport effective width:
    $$\text{text\_cols} = \text{screen\_cols} - G$$
  - Horizontal scrolling:
    Characters are cropped to $[ \text{col\_offset}, \text{col\_offset} + \text{text\_cols} )$.
  - Screen cursor coordinate mapping:
    $$\text{scr\_x} = 1 + G + (rx \ge \text{col\_offset} ? rx - \text{col\_offset} : 0)$$

### 5.2 Feature A4: Go to Line (`Ctrl+G` / `Ctrl+_`)
- **Interactive Prompt:**
  Leverages the existing on-demand `prompt_input()` mechanism:
  1. Status bar prompt: `Go to line, column: `.
  2. Parses user input format (`<line>` or `<line>,<col>`).
  3. Clamps line to $[1, \text{num\_rows}]$ and column to $[1, \text{line\_len} + 1]$.
  4. Cursor updates to target row/col (0-indexed).
  5. Vertically centers the viewport around the destination line:
     $$\text{row\_offset} = (\text{cy} > \text{text\_rows} / 2) ? (\text{cy} - \text{text\_rows} / 2) : 0$$

### 5.3 Feature A5: Word-Wise Motion (`Ctrl+Left`, `Ctrl+Right`, `Alt+B`, `Alt+F`)
- **Keybindings:**
  - CSI sequences: `\x1b[1;5D`, `\x1b[5D` (`KEY_WORD_LEFT`), `\x1b[1;5C`, `\x1b[5C` (`KEY_WORD_RIGHT`).
  - Alt-key sequences: `\x1b\x62` / `\x1bb` (Alt+B), `\x1b\x66` / `\x1bf` (Alt+F).
- **Boundary Semantics:**
  - Character classes: Alphanumeric (`[a-zA-Z0-9_]`), Punctuation, Whitespace (`[ \t]`).
  - Word left skips whitespace then skips backwards to the word start (wrapping to previous line end at column 0).
  - Word right skips the word then skips whitespace to the start of the next word (wrapping to next line start at EOL).

### 5.4 Feature A3: Interactive Search & Replace (`Ctrl+R`)
- **Workflow & Abort Guard:**
  1. Query prompt: `Search to replace: `.
  2. Replacement prompt: `Replace with: `.
  3. Interactive confirmation per match:
     `Replace this instance? (Y)es, (N)o, (A)ll, (Q)uit: `
     - `Y`: Replace current occurrence, search forward for next.
     - `N`: Skip current occurrence, search forward for next.
     - `A`: Replace all remaining occurrences without further prompting.
     - `Q` / `Esc` / `Ctrl+C`: **Immediate abort**. Stops scanning and leaves remaining instances intact.
  4. Final status: `[ Replaced N occurrences ]`.

### 5.5 Feature A1: Bounded Undo / Redo Stack
- **Memory Budget & Delta Size:**
  - 32 undo operations in static BSS ring buffer.
  - Per-entry delta data payload: **1,024 bytes (1 KiB)**, matching `NANO_MAX_LINE_LEN` and `NANO_MAX_CLIPBOARD`.
  - Total BSS footprint: $\approx 34 \text{ KiB}$ static.
  ```c
  #define NANO_UNDO_MAX 32
  #define NANO_UNDO_PAYLOAD 1024

  typedef enum {
      UNDO_OP_INSERT,  /* Coalesced text run inserted */
      UNDO_OP_DELETE,  /* Text run deleted */
      UNDO_OP_SPLIT,   /* Line split on Enter */
      UNDO_OP_JOIN     /* Lines merged on Backspace/Delete */
  } nano_undo_type_t;

  typedef struct {
      nano_undo_type_t type;
      size_t           row;
      size_t           col;
      uint16_t         len;
      char             data[NANO_UNDO_PAYLOAD];
  } nano_undo_entry_t;
  ```
- **Persistence Across Saves:**
  - Undo stack **persists across `Ctrl+O` writes**, matching GNU nano and modern editors.
  - Undo inverts operations cleanly via `Ctrl+Z`; Redo re-applies via `Ctrl+Y` / `Alt+U`.

---

## 6. Phase 3: Power-User & Customization (v2.1)

### 6.1 Feature B2: Case-Sensitivity Toggle
- Search prompts accept `Alt+C` toggle:
  `Search [Case-Sensitive]: ` vs `Search [Case-Insensitive]: `.
- Matching routine checks `s_state.search_case_sensitive`.

### 6.2 Feature B3: Regex Search Engine (Pike VM Reuse)
- Reuses the existing Pike VM NFA implementation from `user/tools/grep.c`:
  - Fixed-array bitmask state sets (up to 64 states).
  - Operators: `.`, `*`, `+`, `?`, `[a-z]`, `[^0-9]`, `^`, `$`.
  - Zero heap allocation: compiled NFA fits in a static struct.
- Status toggle: `Alt+R` in search prompt toggles between Literal and RegEx search.

### 6.3 Features B5 & B6: Configuration File (`/etc/nanorc`) & Tab Configuration
- **File Format:** Simple `key value` lines:
  ```text
  set tabsize 4
  set tabstospaces
  set linenumbers
  set casesensitive
  ```
- **Startup:** If `/etc/nanorc` exists, parse settings on boot before processing CLI args. CLI flags (e.g. `-l`) override configuration defaults.
- **Tab Expansion:**
  - `tabsize`: Configurable between 2, 4, 8 (default 8).
  - `softtabs`: When enabled, pressing `Tab` inserts $N$ spaces instead of `\t`.

### 6.4 Feature B1: Multiple Buffer Switching
- Array of 4 document states: `nano_state_t s_buffers[4]`.
- Each buffer has its own file metadata, cursor coordinates, and modified flag.
- Text pools allocated as partitioned static slices (e.g. $4 \times 128 \text{ KiB}$).
- Keybindings: `Alt+,` (previous buffer) and `Alt+.` (next buffer).
- Header bar displays active buffer indicator: `[1/3: main.c]`.

---

## 7. Verification and Testing Matrix

| Feature | Unit Test Harness (`make test-nano-host`) | QEMU Automated Acceptance (`scripts/test_nano.py`) | Status |
| :--- | :--- | :--- | :--- |
| **Undo / Redo (A1)** | Coalesced typing undo, multi-char delete undo, split/join reversal | Interactive typing, Ctrl+Z verification, Ctrl+Y redo verification | ✅ **VERIFIED (BIOS + UEFI)** |
| **Line Numbers (A2)** | Gutter width formatting and screen offset calculations | Boot QEMU with `-l`, verify gutter rendering and cursor alignment | ✅ **VERIFIED (BIOS + UEFI)** |
| **Search & Replace (A3)** | String replacement, word boundary tracking, bulk replace | Interactive Ctrl+R replace test across multiple instances | ✅ **VERIFIED (BIOS + UEFI)** |
| **Go To Line (A4)** | Line jump, clamping to bounds, viewport center math | Jump to line 2, query cursor via Ctrl+C, verify position | ✅ **VERIFIED (BIOS + UEFI)** |
| **Word Motion (A5)** | Word boundaries, whitespace/punctuation skips | Unit tested Ctrl+Left/Right, Alt+B/F cursor transitions | ✅ **VERIFIED (Host Tests)** |
| **Readonly Mode (C4)** | Mutation rejection, readonly flag checks | Verify typing has no effect, save rejected, clean exit | ✅ **VERIFIED (BIOS + UEFI)** |
| **Pipe Input (C5)** | Loading multiline stream into buffer via mock stdin | `cat test \| /bin/nano -` test in QEMU, verify text presence | ✅ **VERIFIED (BIOS + UEFI)** |
| **CRLF Display (C2)** | CRLF / LF line count heuristics, DOS indicator | Status bar `[DOS]` / `[Unix]` mode display and Alt+D toggle | ✅ **VERIFIED (BIOS + UEFI)** |
| **High Capacity (C3)** | Warning threshold checks at >80% pool capacity | Status notification when buffer approaches pool limit | ✅ **VERIFIED (Host Tests)** |
| **Multi-Buffer (B1)** | Array buffer state preservation, wrap-around index | `nano file1 file2`, `Alt+,` and `Alt+.` buffer cycling | ✅ **VERIFIED (BIOS + UEFI)** |
| **Case-Sensitivity (B2)**| Exact vs case-insensitive match comparisons | Live prompt `Alt+C` toggle and pattern matching | ✅ **VERIFIED (BIOS + UEFI)** |
| **Regex Search (B3)** | Pike VM regex match on buffer lines (`.`, `*`, `+`, `[...]`, `^`, `$`) | Search for `[A-Z]+` and verify cursor lands on match | ✅ **VERIFIED (BIOS + UEFI)** |
| **Config File (B5/B6)**| Tab sizing (4 vs 8) and `tabstospaces` rendering/insertion | `/etc/nanorc` / `/mnt/nanorc` persistence on startup | ✅ **VERIFIED (BIOS + UEFI)** |

