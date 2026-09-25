# Shell S5: Environment, Variables, Expansion, Aliases & Globbing

Implementation date: 2026-09-25. Design reference: [SHELL_DESIGN.md](../plans/SHELL_DESIGN.md).

## Delivered

### S5 — Environment, Variables, Expansion, Aliases & Globbing
- **Architectural Scope & Stack Invariants:**
  - *Variable Scoping:* Flat for S5, scoped for S9. The shell maintains a single flat variable table per session with export flags; command-local assignments (`FOO=bar cmd`) temporarily apply to child execution and revert immediately upon completion.
  - *Envp Budget & User Stack Invariant:* The user stack remains strictly 4 KiB (one page frame).
    - `MAX_TOTAL_ARGS_LEN` = 1024 bytes (max 32 args).
    - `MAX_TOTAL_ENVP_LEN` = 1024 bytes (max 32 env entries).
    - `MAX_POINTER_TABLE_BYTES` = 568 bytes ((32 + 32 + 5) * 8 + 16-byte alignment).
    - `MINIMUM_USER_STACK_FLOOR` = 512 bytes.
    - Compile-time assertion `_Static_assert(MAX_TOTAL_ARGS_LEN + MAX_TOTAL_ENVP_LEN + MAX_POINTER_TABLE_BYTES + MINIMUM_USER_STACK_FLOOR <= 4096, ...)` in `src/kernel/elf.h`.
    - Runtime check in `process_setup_user_stack` ensures `rsp >= USER_STACK_PAGE_VIRT + MINIMUM_USER_STACK_FLOOR`.
    - Strings packed top-down from `USER_STACK_TOP_VIRT - 1`, with standard System V AMD64 ABI layout: `argc`, `argv[0..argc-1]`, `NULL`, `envp[0..envc-1]`, `NULL`, `AT_NULL` pair.
- **Kernel Extended Spawn ABI (`SYS_SPAWN_EXT`):**
  - Added `SYS_SPAWN_EXT (20)` in `src/include/syscall_abi.h` taking 64-byte `spawn_opts_t` (with `_Static_assert(sizeof(spawn_opts_t) == 64)`).
  - Validates user pointers, string buffers, and null-termination in kernel space.
  - Passes user `envp` string vector into `process_spawn_internal` and `process_setup_user_stack`.
  - CWD override support when specified in `spawn_opts_t`.
- **Variable Table & Builtins (`user/shell/vars.c`, `user/shell/vars.h`):**
  - Flat table supporting up to 64 variables with `VAR_EXPORTED` flags.
  - Default variables initialized on shell startup: `PATH=/bin`, `HOME=/`, `?`, `$`.
  - Pure assignments: `NAME=value` sets shell variable; multiple assignments on a single line supported.
  - Temporary command-local scoping: `vars_scope_begin()`, `vars_scope_set()`, and `vars_scope_end()` ensure command-local prefixes revert immediately after invocation.
  - Builtins:
    - `set`: lists all current shell variables.
    - `unset <var...>`: removes variables from table.
    - `export [var[=val]...]`: exports variables to environment or lists exported variables.
    - `env`: lists only variables flagged as exported.
  - Dynamic `PATH` lookup: commands without slashes search colon-delimited directories in `$PATH` (falling back to `/bin`).
- **Alias Table & Builtins (`user/shell/alias.c`, `user/shell/alias.h`):**
  - Table supporting up to 32 aliases.
  - Line-level alias expansion before tokenization.
  - Cycle detection and recursion depth bound (16 levels max).
  - Quoting / backslash suppression (`\ll` or `'ll'` disables alias expansion).
  - Builtins:
    - `alias`: prints current aliases formatted as `alias name='value'`.
    - `alias name='value'`: defines or updates an alias.
    - `unalias name`: removes alias.
- **Quote-suppression Tracking in Lexer & Parser (`user/shell/lexer.c`, `user/shell/parser.c`):**
  - Token and AST structures enhanced with `quote_flags` parallel array (`QUOTE_NONE`, `QUOTE_SINGLE`, `QUOTE_DOUBLE`, `QUOTE_ESCAPED`) and `has_quotes` flag.
  - Full backward compatibility preserved: token values remain unquoted strings for seamless parsing.
- **Shell Expansion Engine (`user/shell/expand.c`, `user/shell/expand.h`):**
  - Strict 5-stage POSIX-compliant expansion pipeline:
    1. **Tilde expansion:** Leading `~` or `~/...` expanded using `$HOME` (when unquoted).
    2. **Parameter expansion:** `$VAR`, `${VAR}`, `$?` (exit status), `$$` (PID) expanded inside unquoted or double-quoted tokens, suppressed inside single-quoted tokens.
    3. **Word splitting:** Unquoted whitespace in expanded parameter values splits tokens into separate argument fields. Quoted whitespace strictly preserved.
    4. **Pathname expansion (Globbing):** Unquoted `*`, `?`, `[...]`, `[!...]` patterns matched against filesystem directories via `SYS_READDIR`. Non-matching patterns preserved literally.
    5. **Quote removal / suppression:** Final argument array prepared with exact empty quoted argument preservation.
- **Ring 3 Stack Budget Safety:**
  - Audited all user shell modules via `-fstack-usage`.
  - All token pools, AST trees, expansion character/flag arrays, and argument buffers reside in static BSS storage.
  - Stack usage strictly under 720 bytes for all functions, well within the 4 KiB user stack frame.

## Verification Evidence

All tests run on WSL `Ubuntu-24.04` at `/mnt/c/Sources/FortressOS`:

| Target / Command | Result and Scope |
| --- | --- |
| `make` | PASS: zero warnings/errors (`-Wall -Wextra -Werror`), freestanding kernel and shell ELF, bootable ISO and raw GPT/ext2 disk image. |
| `make test-shell-host` | PASS: host ASan/UBSan unit tests across all 7 suites (editor, lexer, parser, variables, aliases, globbing, expansion pipeline). |
| `python3 scripts/test_shell_s5.py bios` | PASS: legacy BIOS QEMU integration suite (variables, assignments, export, env, unset, $?, $$, command-local scoping, aliases, tilde, globbing, word splitting). |
| `python3 scripts/test_shell_s5.py uefi` | PASS: UEFI QEMU integration suite with paired OVMF firmware. |
| `python3 scripts/test_shell_s3_s4.py` | PASS: legacy BIOS and UEFI regressions for S3 (quotes, chaining, cwd, direct execution) and S4 (completion, prompt, history). |
| `python3 scripts/test_ext2_write.py` | PASS: 3-boot cross-boot writable ext2 persistence and `e2fsck -fn` integrity verification across BIOS and UEFI. |
