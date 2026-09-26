# S6 acceptance audit

Audited 2026-09-25 at commit `e0545d0`. Phase 2 fixes landed at commit `d1abc2f` on top of `e0545d0`. Verdict: **S6 incomplete** (Phase 2 closed; Phase 3 append and Phase 4 redirection pending).
This is a code audit and test checklist, not an implementation change.
Contract: [SHELL_DESIGN.md, S6](SHELL_DESIGN.md#s6--descriptors-and-redirection).

## Findings, in repair order

### 1. [CLOSED 2026-09-26] Duplication returns positive errors

`src/kernel/thread.c:1497–1529` returned `-SYSCALL_EBADF` and
`-SYSCALL_EMFILE`, but the ABI constants were already negative (-3 and -6).
Invalid duplication returned 3 and exhaustion returned 6.
**Fix:** Changed return values to return `SYSCALL_EBADF` and `SYSCALL_EMFILE`
directly. Added direct self-test in `src/kernel/main.c:test_fd_scope_begin`
asserting that `fd_dup(-1) == SYSCALL_EBADF`, `fd_dup(31) == SYSCALL_EBADF`,
`fd_dup2(31, 1) == SYSCALL_EBADF`, `fd_dup2(1, 32) == SYSCALL_EBADF`,
`fd_dup2(1, 1) == 1`, descriptor allocation exhausts to `SYSCALL_EMFILE` at 32,
and `fd_free` restores ref_count to 1 without leaks.

### 2. [CLOSED 2026-09-26] Terminal reads use regular-file EOF rules

`src/fs/vfs.c:565–595` tested offset against node size before calling the read
callback, returning EOF on zero-sized terminal nodes. Terminal writes also
advanced file offset and node size.
**Fix:** Added `bool is_stream` to `vfs_node_t`. Marked `g_terminal_node` and
`g_null_node` with `.is_stream = true`. `vfs_read` and `vfs_write` bypass
regular-file size and offset checks when `node->is_stream` is true, passing
offset 0 to callbacks without modifying `file->offset` or `node->size`.
Verified in `tests/ext2_host.c` that terminal reads reach `input_read` with
size 0, writes leave offsets at 0, and access modes (`VFS_O_RDONLY` /
`VFS_O_WRONLY`) remain strictly enforced with `-VFS_EBADF`.

### 3. Missing: redirection execution

`user/shell.c:563–665` executes arguments but never consumes `cmd->redirs`.
`spawn_program` at line 93 zeroes spawn options and never supplies fd actions.
`expand_redir_target` has no caller. Consequently syntax such as `echo x >out`
is accepted, but the redirection is ignored. Builtin save/apply/restore is absent.
Implement ordered expansion/application for parent builtins and child spawn,
including redirection-only commands and setup failure without command execution.

### 4. [CLOSED 2026-09-26] Descriptor-number parsing is unbounded

Adjacent descriptor numbers in `lexer.c` and separate numeric tokens in `parser.c`
could overflow or exceed the 0–31 process descriptor table bounds.
**Fix:** In `user/shell/lexer.c`, adjacent digits are bounded and saturated at 32
as an invalid sentinel. In `user/shell/parser.c`, descriptors are verified against
`r->redir_fd < 0 || r->redir_fd >= 32 || r->redir_dup_fd >= 32`, and numeric
token accumulation checks for integer multiplication overflow before conversion
(`dfd > (31 - digit) / 10`). Tested in `tests/shell_host.c` against `2>&257`,
`99>out`, `999999999999999>out`, `2>& 999999999999999999999999`, and verified
that valid maximum descriptors (`31>&30`, `2<& 0`, `1>&-`) parse cleanly.

### 5. Incorrect: append is not atomic

`src/fs/vfs.c:628–631` selects EOF before entering `ext_write`.
`src/fs/ext2.c:825–833` acquires the ext2 lock only after the offset was chosen.
Separate writers can select the same EOF and overwrite each other. EOF selection
and writing must occur together under the filesystem operation lock. Preserve
IRQ-save block I/O and USB durability policy. Test independently opened append
handles with distinct records and assert every record occurs exactly once.

### 6. Missing: retained terminal and usable close-on-spawn policy

`user/shell/ui.c:31` defaults the UI handle to stdout; its setter has no caller.
Thus the intended retained terminal handle is not established. `FD_FLAG_CLOEXEC`
is checked during inheritance, but no current caller sets it. Introduce a bounded
way to retain and protect shell-private handles while excluding them from child
inheritance. Test prompt/editor output after stdout redirection and closure.

### 7. Partial: short writes and error propagation (Open — not yet landed)

`user/shell/io.c:17–29` retries positive short writes and stops on zero/error,
avoiding recursive error output. However the void return discards failures, so
callers cannot reliably report a failing command status. `puts_err` has no caller;
existing file/spawn diagnostics still use stdout. Propagate errors to command
status, report setup errors on stderr, and test a closed stderr without recursion.

### 8. Partial: ownership works sequentially; concurrency needs a contract

Duplication and inheritance share `file_t`, while independent opens allocate
separate objects. Access modes are checked by VFS. Failed spawn closes descriptors,
frees the TCB and stack, and destroys its address space before publication.
Exit/reaping closes remaining descriptors. These are useful implemented foundations.

Reference increments/decrements and shared offset updates are unsynchronized.
Shell spawns retain CPU affinity and IRQ exclusion, which limits ordinary races;
that alone is not an SMP lifetime proof because the reaper scans all CPU dead
lists. Trace fault/reap versus inheritance and shared I/O before choosing locking
or atomic lifetime rules. Do not hold a spinlock across blocking terminal input.
The public lifecycle comment in `thread.h:117` still says "no inherited file
descriptors" and must be reconciled with the implementation.

---

## Phase 3 Architecture Contracts

Before implementing Phase 3 (Atomic Append & Concurrency), the following two
architectural contracts govern VFS stream typing and file descriptor synchronization.

### Contract A: Node-Type Contract (Streams vs. Regular Files)

The VFS distinguishes stream nodes (`node->is_stream == true`) from regular files:

1. **No Linear File Offset or Size:** A stream node represents a continuous,
   non-seekable byte stream (e.g. `/dev/tty`, `/dev/null`, and S7 anonymous pipes).
   Stream nodes have no persistent size or linear byte offset.
2. **Callback Ownership of Framing and EOF:**
   - In `vfs_read`, stream nodes bypass the `file->offset >= file->node->size` EOF check.
     Reads pass offset 0 directly to `node->read`; the callback is responsible for
     framing, line-buffering, timeouts, or returning 0 on EOF (`input_read`, pipe buffers,
     or `/dev/null`).
   - In `vfs_write`, stream nodes pass offset 0 directly to `node->write`. Writes do not
     advance `file->offset` or increment `node->size`.
3. **Truncation and Append:** Truncating a stream node is a no-op returning 0 (`dummy_truncate`).
   The `VFS_O_APPEND` flag is ignored for stream nodes because writes are inherently at the
   stream head.
4. **Regular Files:** All nodes with `is_stream == false` adhere strictly to standard
   filesystem offset/size semantics: `vfs_read` terminates with 0 at `offset >= size`,
   and writes advance `file->offset` and update `node->size`.

### Contract B: File Descriptor & `file_t` Concurrency Contract

The lifetime and synchronization of descriptors and shared file descriptions follow these rules:

1. **Process Isolation & Single-Thread Ownership:** A process's descriptor table
   `tcb->fd_table[0..31]` and flags `tcb->fd_flags[0..31]` are strictly per-process.
   Because FortressOS processes are single-threaded, descriptor table lookup, allocation,
   and replacement (`fd_alloc`, `fd_free`, `fd_dup`, `fd_dup2`) require no intra-process
   locking during normal user execution.
2. **Atomic Spawn Inheritance:** During process spawning (`process_spawn_from_vfs_ext`),
   descriptor cloning occurs under CPU IRQ exclusion on the spawning core. Non-CLOEXEC
   `file_t` references are copied to the child, and `file->ref_count` is incremented.
   If any spawn action or address-space setup fails, the kernel calls `fd_close_all(child)`
   to tear down all cloned and newly opened descriptors before freeing the TCB. The child
   is never enqueued to any scheduler runqueue on failure.
3. **Cross-Process `file_t` Lifetime:** Multiple processes can share an open file description
   `file_t` via inheritance or duplication. Reference counting (`file->ref_count`) must use
   atomic operations (`__atomic_fetch_add` / `__atomic_sub_fetch` with `__ATOMIC_SEQ_CST`).
   When `ref_count` reaches 0, the `file_t` is safely freed (`kfree(file)`).
4. **Atomic Append Serialization (Finding 5):**
   - Setting `file->offset = file->node->size` in the VFS layer prior to locking the filesystem
     is prohibited because concurrent appenders can interleave offset selection and writes.
   - For filesystems supporting append (such as `ext2`), atomic append must be serialized at
     the filesystem operation lock boundary. When `file->flags & VFS_O_APPEND` is set, `ext2_write`
     acquires its IRQ-save filesystem lock, selects the file's current EOF (`inode.i_size`), allocates
     blocks and writes data, and commits the updated `inode.i_size` before releasing the lock.
   - For processes sharing a single `file_t` description, `file->offset` is updated under the
     atomic return of the write operation. For independent `file_t` descriptions pointing to the
     same inode, each append atomically targets the serialized end-of-file.

---

## Acceptance matrix

| Requirement | Current assessment | Required evidence |
| --- | --- | --- |
| Uniform descriptors and access modes | Partial: stream bypass active; stdin works; access modes enforced | Host ext2 suite verified; Ring 3 terminal/null/file reads and writes verified |
| Dup/shared offsets/independent opens | Partial: negative errors verified; boot self-test covers exhaustion/cleanup | Kernel boot self-test verifies -EBADF, -EMFILE, self-dup, exhaustion, cleanup; Ring 3 integration pending |
| Spawn fd actions and ordering | Kernel path present; shell disconnected | Child stdout/stderr routing, close/open/dup order, invalid-action unwind |
| Builtin redirection | Missing | Parent save/apply/restore, including initially closed fds and every failure |
| One-path target expansion | Helper present, unused | Quoting, spaces, empty/unset expansion, zero/multiple glob matches |
| Retained UI terminal | Missing wiring | Prompt and editing after redirection, closure and failed setup |
| Concurrent append | Phase 3 focus (Contract B defined) | Separate writers, no lost/overwritten records, exact output verification under ext2 lock |
| Resource exhaustion and allocation failures | Not established for S6 | Exhaust fd/process capacity; inject allocation failures; no runnable partial child |
| RO/tainted storage and short I/O | Existing layers, S6 not established | Distinct errors, command not executed after setup failure, stable prompt/status |
| Existing programs and shell | Prior regression pass | Preserve existing host, BIOS/UEFI and no-UART coverage |

## Evidence from this audit

- `wsl -d Ubuntu-24.04 -- make test-shell-host`: PASS. Verified ASan/UBSan
  editor, lexer/parser, bounded redirection numbers (e.g. `2>&257`, `99>out`,
  overflow protection), variables, aliases, globbing, and expansion.
- `wsl -d Ubuntu-24.04 -- make test-ext2`: PASS across all 8 block/sector geometry
  permutations (1k/2k/4k block × 512/4k sector). Verified stream I/O bypass
  (read reaching `input_read`, zero-offset preservation, access mode rejection).
- **Phase 2 Verification Runs:**
  - QEMU Legacy BIOS: PASS ×3 consecutive runs (including socket retry fix in `scripts/test_shell.py`).
  - QEMU UEFI: PASS ×1 run.
  - Bare-metal Dell Latitude 5590: PASS boot and interactive shell response.
- **Kernel Boot Self-Tests:** `test_phase7_checkpoint2_syscalls` / `test_fd_scope_begin`
  verified negative error codes (`SYSCALL_EBADF`, `SYSCALL_EMFILE`), self-duplication,
  replacement, full table exhaustion up to descriptor 31, and ref_count decrement/cleanup.

## Execution checklist

1. [x] Fix duplication errors, stream semantics and parser bounds with focused tests (Phase 2 closed).
2. [ ] Phase 3: Implement atomic append in `ext2` and `vfs` under Contract B; add concurrent append test.
3. [ ] Phase 4: Wire ordered redirections into children (`SYS_SPAWN_EXT`) and scoped parent builtins; retain
   controlling terminal handle (`g_term_fd`); propagate short write errors and exclude private handles from spawn.
4. [ ] Exercise `<`, `>`, `>>`, `2>`, `2>>`, `n>&m`, `n<&m`, `n>&-`, and compare
   `cmd >out 2>&1` with `cmd 2>&1 >out` using a child that writes to both streams.
5. [ ] Run bounded BIOS/UEFI cases on disposable fixtures and verify file contents,
   heap/reference/stack/page baselines, failed-child absence and prompt recovery.
   Run relevant ext2/storage, shell and power regressions after implementation.
6. [ ] Dell hardware checklist in a dedicated directory on the explicitly
   selected writable USB. Verify builtin/child output, input, append, stderr,
   ordered duplication, failure recovery and persistence. Record exact commands and observed contents.
7. [ ] Mark S6 complete only after every row has evidence; update the roadmap and
   stale design status separately from historical results.
