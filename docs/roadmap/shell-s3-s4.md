# Shell S3–S4: Single Parser, Working Directories, Command Discovery, Completion, Prompt & Persistent History

Implementation date: 2026-09-25. Design reference: [SHELL_DESIGN.md](../plans/SHELL_DESIGN.md).

## Delivered

### S3 — Single Parser, Working Directories & Command Discovery
- **Quote-aware Lexer & Parser (`user/shell/lexer.c`, `user/shell/parser.c`):**
  - Single quotes (`'...'`): literal preservation of all characters including metacharacters and whitespace.
  - Double quotes (`"..."`): character preservation with escape processing (`\"`, `\\`, `\n`, `\t`, etc.).
  - Backslash escapes outside quotes (`\ ` for literal space, etc.).
  - Concatenated word parts (`foo"bar"baz` -> `foobarbaz`).
  - Empty quoted arguments (`""`, `''`) preserved as distinct empty arguments.
  - Comment handling (`# ...` to end of line).
  - Operators: sequential (`;`), short-circuit logical AND (`&&`), logical OR (`||`), and pipeline negation (`!`).
  - Multi-line continuation prompt (`> `) when input is incomplete (unclosed single/double quotes, trailing backslash, or trailing operator).
  - Static token/parser buffers in BSS preventing user stack overflow in Ring 3 (stack usage audited < 528 bytes, well within the 4 KiB limit).
- **Working Directory Support (`SYS_GETCWD`, `SYS_CHDIR`):**
  - Added `SYS_GETCWD (18)` and `SYS_CHDIR (19)` to `src/include/syscall_abi.h`.
  - Added process-owned `char cwd[256]` to `tcb_t` in `src/kernel/thread.h`.
  - CWD initialized to `/` for initial threads and inherited across thread creation and process spawning (`thread_create_internal`, `process_spawn_internal`).
  - Kernel relative path resolution (`resolve_path` in `src/kernel/syscall.c`) normalizing `.`, `..`, redundant slashes, clamping at `/`, and enforcing `VFS_MAX_PATH` across `sys_open`, `sys_stat`, `sys_mkdir`, `sys_unlink`, `sys_rename`, and `sys_spawn`.
  - Shell builtins: `pwd`, `cd [path]`, and `cd -` (OLDPWD toggle).
- **Direct Execution & Discovery:**
  - Direct execution by absolute path (`/bin/hello`) or relative path (`./bin/hello`).
  - Bare command discovery: commands without slashes are looked up in `/bin` and executed directly.
  - Compatibility wrapper: `run` builtin preserved for backwards compatibility.
  - Builtins: `type` (reports builtin vs binary path), `command` (invokes builtin/command), `true` (status 0), `false` (status 1).

### S4 — Tab Completion, Prompt Customization & Persistent History
- **Tolerant Tab Completion (`user/shell/complete.c`):**
  - Invoked via Tab (`\t` / `EDIT_COMPLETE`).
  - Completes builtins, `/bin` executables, and filesystem paths.
  - Context-sensitive completion: command position completes commands and builtins; argument position completes files/directories (or builtins/binaries for `type`/`command`/`help`).
  - Directories completed with trailing `/`.
  - Common prefix insertion on ambiguous matches; candidate listing when double-tabbed.
  - Uses static buffers to avoid Ring 3 stack consumption.
- **Configurable Prompt Templates (`user/shell/ui.c`):**
  - `prompt` displays current prompt template.
  - `prompt default` sets prompt back to legacy `fortress> `.
  - `prompt cwd` enables dynamic prompt template `fortress:<cwd> $ `.
  - Status indicator prefix: when last command exits with non-zero status, displays `[<status>] fortress:<cwd> $ `.
  - `prompt <template>` supports custom prompt strings.
- **Optional Persistent History (`user/shell/history_persist.c`):**
  - Format: length-framed records (`FOSHIS1\n` header, followed by `<len> <cmd>\n`).
  - Target: `/mnt/.fortress/history`. Automatically creates `/mnt/.fortress` directory.
  - Safety & Mount Policy: write probe validates writable mount status before attempting write; refuses with explicit error on read-only mounts without damaging storage.
  - Durability: invokes `SYS_SYNC` after writing history.
  - Shell builtins: `history save`, `history load`, `history clear`.

## Verification Evidence

All tests run on WSL `Ubuntu-24.04` at `/mnt/c/Sources/FortressOS`:

| Target / Command | Result and Scope |
| --- | --- |
| `make` | PASS: zero warnings/errors (`-Wall -Wextra -Werror`), freestanding kernel and shell ELF, bootable ISO and raw GPT/ext2 disk image. |
| `make test-shell-host` | PASS: host ASan/UBSan unit tests for input decoder, framebuffer console, shell line editor, quote-aware lexer, and parser AST. |
| `python3 scripts/test_shell.py bios` | PASS: legacy BIOS QEMU integration suite (real PS/2 & UART input, prompt wait, commands, editor, status codes, resource leak audits). |
| `python3 scripts/test_shell.py uefi` | PASS: UEFI QEMU integration suite with paired OVMF firmware. |
| `python3 scripts/test_shell_no_uart.py` | PASS: UEFI 8 GiB framebuffer-only keyboard input and screenshot capture. |
| `python3 scripts/test_ext2_write.py` | PASS: 3-boot BIOS and UEFI ext2 write persistence, truncation, and offline `e2fsck -fn` integrity audits. |
| `python3 scripts/test_power.py` | PASS: ACPI S5 clean shutdown and reboot. |
| `make test-shell-s3-s4` | PASS: dedicated S3/S4 integration suite across both BIOS and UEFI: quotes (`'...'`, `"..."`, escapes, concatenation), comments (`#`), operators (`;`, `&&`, `||`, `!`), Tab completion (builtins and paths), `pwd`, `cd`, `cd -`, relative paths across syscalls, direct execution (`hello`, `/bin/hello`, `./bin/hello`), `type`, `command`, configurable prompt with status indicator, and `/mnt/.fortress/history` persistence. |
