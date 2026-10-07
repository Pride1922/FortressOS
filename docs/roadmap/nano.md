# nano — Text Editor

`nano` is a lightweight, bounded in-terminal text editor for FortressOS designed for system configuration and shell workflows.

## Shipped (Phases 1–3)

- **Phase 1 (v1.1 Polish):** Differential terminal redraw without flicker, hardware cursor placement, status bar updates, viewport scroll sanity.
- **Phase 2 (v2.0 Essentials):**
  - Line numbers (`-l` CLI flag, `Ctrl+N` toggle) with gutter-aware viewport math.
  - Go to line (`Ctrl+G`) with interactive prompt.
  - Word-wise navigation (`Ctrl+Left` / `Ctrl+Right`, `Alt+B` / `Alt+F`).
  - Search & Replace (`Ctrl+R`) with match confirmation (`y`, `n`, `a`, `q` / `Esc` abort).
  - Undo & Redo (`Ctrl+U` / `Ctrl+E`) bounded history persisting across saves.
  - Safety & pipes: Read-only mode (`-R`), standard input pipe reading (`nano -`), large file capacity warnings (>256 KiB cap), CRLF line ending detection.
- **Phase 3 (v2.1 Power Features):**
  - Case-sensitivity toggle for search (`Alt+C`).
  - Pike VM regex search & replace (`.` , `*`, `+`, `[...]`, `^`, `$`).
  - Multi-buffer support (`Alt+<` / `Alt+>`, `Ctrl+X` buffer close).
  - Nanorc configuration parser (`/etc/nanorc`, tab size, auto-indent, soft wrap).

## Known Limitations

- **Undo depth:** Fixed-capacity circular history stack (32 entries, max 4096 bytes delta per operation).
- **Regex engine subset:** Pike VM bitmask NFA supports basic and extended tokens (`.`, `*`, `+`, character classes `[...]`, anchors `^`/`$`), but omits backreferences, `{m,n}` bounds, and grouping/alternation `(...)`/`|`.
- **No syntax highlighting:** Plain ANSI text rendering; language tokenizers and colorization are omitted to keep the binary small and bounded.
- **Buffer limits:** 256 KiB max buffer size, 4096 max lines per buffer, up to 8 concurrent buffers.

## Grep Engine Parity Gap

- **`+` Quantifier:** Implemented in `nano` via preprocessor expansion ($X+ \equiv X \cdot X^*$) over the shared Pike VM. In `user/tools/grep.c`, the compiler accepts `-E` but does not yet implement the `+` expansion.

## Deferred / Out of Scope

- Split windows / multiple viewport panes.
- Visual block selection / external clipboard integration.
- Language syntax highlighting engines.
- Macro recording and plugin systems.
