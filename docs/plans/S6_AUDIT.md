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

### 3. [PARTIAL 2026-09-26] Redirection execution

- **Phase 4A & 4B (Child Redirections): COMPLETE (2026-09-26).**
  Implemented `user/shell/redir.c` (`redir_build_spawn_actions`):
  - Iterates over parsed AST `redir_t` records in lexical sequence.
  - Expands target paths using `expand_redir_target` (parameter expansion, quote suppression, word splitting, globbing); rejects ambiguous redirections with error code and diagnostics without spawning child.
  - Maps redirection operators to `spawn_fd_action_t`:
    `REDIR_IN` -> `SPAWN_FD_ACTION_OPEN` (`VFS_O_RDONLY`),
    `REDIR_OUT` -> `SPAWN_FD_ACTION_OPEN` (`VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC`),
    `REDIR_APP` -> `SPAWN_FD_ACTION_OPEN` (`VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND`),
    `REDIR_DUP_OUT` / `REDIR_DUP_IN` -> `SPAWN_FD_ACTION_DUP2`,
    `REDIR_CLOSE` -> `SPAWN_FD_ACTION_CLOSE`.
  - Wired into `user/shell.c`: `execute_parse_tree` builds actions into static file-scope storage (adhering strictly to 512B stack budget), passes `opts.fd_actions` and `opts.action_count` to `SYS_SPAWN_EXT` in `spawn_program` (clearing `opts.fd_actions` when `action_count == 0` per kernel validation contract).
  - Verified under host ASan/UBSan (`tests/shell_host.c`) and live QEMU integration (`scripts/test_shell_s6.py` / `make test-shell-s6`) on BIOS and UEFI.
- **Phase 4C (Builtin Redirections) & 4D (Retained UI Terminal): PENDING.**
  Parent builtin save/apply/restore and shell-private UI terminal handle retention remain to be wired.

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

### 5. [VERIFIED 2026-09-26] Append serialization and atomic EOF

`src/fs/vfs.c` previously selected EOF outside and before filesystem locking.
`src/fs/ext2.c` acquired the ext2 lock only after the offset was chosen.
Separate writers could select the same EOF and overwrite each other.
**Fix:**
- Updated filesystem write callback signature to `int64_t (*write)(struct vfs_node *node, uint64_t *off, bool append, const void *buf, size_t len)`.
- In `ext2.c:ext_write`, `ext2_lock` is acquired first via `spin_lock_irqsave(&ext2_lock)` before inspecting file size.
  When `append == true`, `*off` is set to the authoritative `in->size` inside the lock.
  When `append == false`, writes past EOF (`*off > in->size`) are rejected with `-VFS_EINVAL` (`-22`).
  On write completion, `node->size = in->size`, `*off += (uint64_t)r`, and the updated offset is written back to `file->offset` under lock release.
- In `thread.c` and `vfs.c`, `file->ref_count` increments and decrements use `__ATOMIC_ACQ_REL` atomic operations.
**Verification Evidence:**
- Sequential semantics (EOF selection, offset writeback, interleaved handles, and EOF past-write rejection) verified in `tests/ext2_host.c:639–668`.
- Multi-core SMP race verification implemented in `src/kernel/main.c:test_smp_ext2_concurrent_append` and runner `scripts/test_smp_append.py`:
  - Tested under QEMU `-smp 4` with AP workers pinned to dedicated cores (Core 1 and Core 2) slamming concurrent writes against a shared start barrier.
  - Scenario 1 (Independent handles): Both workers open independent `file_t` handles with `VFS_O_APPEND`. Exactly 200 records (3200 bytes) delivered with 0 lost, 0 duplicates, and strict per-worker ordering preserved.
  - Scenario 2 (Shared handle): Both workers share a single `file_t` handle with `VFS_O_APPEND` (refcount managed atomically). Exactly 200 records (3200 bytes) delivered with 0 lost, 0 duplicates, and strict per-worker ordering preserved.
  - Real contention and physical interleaving confirmed: 38 and 31 interleaving transitions on BIOS; 64 and 75 transitions on UEFI.
  - Offline filesystem audit: Clean ACPI S5 shutdown via `poweroff` flushes NVMe storage; host `e2fsck -fn` verified clean ext2 filesystem with 0 errors across both BIOS and UEFI.

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

The VFS distinguishes stream nodes (`node->is_stream == true`, with a planned enum cleanup to `VFS_NODE_STREAM` in `vfs_node_type_t` during Phase 3) from regular files:

1. **No Linear File Offset or Size:** A stream node represents a continuous,
   non-seekable byte stream (e.g. `/dev/tty`, `/dev/null`, and S7 anonymous pipes).
   Stream nodes have no persistent size or linear byte offset. Seeking on stream nodes
   returns `-ESPIPE` (future-proofing `SYS_LSEEK`).
2. **Callback Ownership of Framing and EOF:**
   - In `vfs_read`, stream nodes bypass the `file->offset >= file->node->size` EOF check.
     Reads pass offset 0 directly to `node->read`; the callback is responsible for
     framing, line-buffering, timeouts, or returning 0 on EOF (`input_read`, pipe buffers,
     or `/dev/null`).
   - In `vfs_write`, stream nodes pass offset 0 directly to `node->write`. Writes do not
     advance `file->offset` or increment `node->size`.
3. **Truncation and Append:** Truncating a stream node is a no-op returning 0 (`dummy_truncate`).
   The `VFS_O_APPEND` flag is accepted for compatibility but is an operational no-op for streams
   because writes are inherently at the stream head. Write atomicity on streams is bounded and
   governed per-stream (e.g. pipe buffer limits or line-buffered terminal input/output).
4. **Regular Files:** All nodes with `is_stream == false` adhere strictly to standard
   filesystem offset/size semantics: `vfs_read` terminates with 0 at `offset >= size`,
   and writes advance `file->offset` and update `node->size`.

### Contract B: File Descriptor & `file_t` Concurrency Contract

The lifetime and synchronization of descriptors and shared file descriptions follow these rules:

1. **Process Isolation & Single-Thread Ownership Assumption:** A process's descriptor table
   `tcb->fd_table[0..31]` and flags `tcb->fd_flags[0..31]` are strictly per-process.
   FortressOS currently operates under the structural assumption that processes are single-threaded;
   therefore, descriptor table lookup, allocation, and replacement (`fd_alloc`, `fd_free`, `fd_dup`, `fd_dup2`)
   require no intra-process locking during normal user execution.
   *What breaks this assumption:* Introducing multi-threaded user processes (shared address space with
   multiple TCBs) or kernel worker threads modifying another process's descriptor table would break this,
   requiring per-process fd-table spinlocks.
2. **Explicit CLOEXEC Semantics:**
   `FD_FLAG_CLOEXEC` is tracked per-descriptor in `tcb->fd_flags[fd]`. It is evaluated during process
   spawning (`process_spawn_from_vfs_ext`), where any descriptor marked with `FD_FLAG_CLOEXEC` is excluded
   from inheritance and left unmapped in the child. In S6, `SYS_DUP` / `SYS_DUP2` clear `FD_FLAG_CLOEXEC`
   on the newly created descriptor, while shell-private handles (such as the retained UI terminal)
   are explicitly marked with `FD_FLAG_CLOEXEC` to prevent accidental leakage into child processes.
3. **Atomic Spawn Inheritance:** During process spawning (`process_spawn_from_vfs_ext`),
   descriptor cloning occurs under CPU IRQ exclusion on the spawning core. Non-CLOEXEC
   `file_t` references are copied to the child, and `file->ref_count` is incremented.
   If any spawn action or address-space setup fails, the kernel calls `fd_close_all(child)`
   to tear down all cloned and newly opened descriptors before freeing the TCB. The child
   is never enqueued to any scheduler runqueue on failure.
4. **Cross-Process `file_t` Lifetime & Atomic Memory Ordering:** Multiple processes can share an open
   file description `file_t` via inheritance or duplication. Reference counting (`file->ref_count`) must use
   atomic operations with acquire-release semantics (`__atomic_fetch_add(..., __ATOMIC_ACQ_REL)` /
   `__atomic_sub_fetch(..., __ATOMIC_ACQ_REL)`).
   *Reasoning:* Full sequential consistency (`__ATOMIC_SEQ_CST`) enforces a global total order across all
   processors with expensive bus synchronization penalties. Acquire-release ordering (`ACQ_REL`) is both
   necessary and sufficient for reference counting: the release ensures all prior modifications to `file_t`
   are visible before the refcount drops, and the acquire ensures that whichever core decrements the refcount
   to zero synchronizes-with all prior releases before executing final cleanup and deallocation (`kfree(file)`).
5. **Atomic Append Serialization & Concrete Fix Shape (Finding 5):**
   - Setting `file->offset = file->node->size` in the VFS layer prior to locking the filesystem
     is prohibited because concurrent appenders can interleave offset selection and writes.
   - The concrete fix updates the filesystem write signature to:
     `ssize_t ext2_write(ext2_fs_t *fs, ext2_inode_t *inode, uint64_t *offset, bool append, const void *buf, size_t count);`
     When `append == true`, `ext2_write` acquires its IRQ-save filesystem lock (`ext2->lock`), reads the
     authoritative current EOF (`inode->size`), sets `*offset = inode->size`, allocates required blocks,
     writes data, updates `inode->size`, and releases the lock atomically.
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
| Concurrent append | Verified (2026-09-26) | Two pinned workers on cores 1 and 2, independent and shared handles, 200 records (3200 bytes) 100% delivered, 0 lost/duplicate, strict per-worker ordering, interleaving confirmed, clean S5 shutdown, 0 e2fsck errors on BIOS and UEFI |
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
- **Phase 3 Verification Runs (`make test-smp-append`):**
  - QEMU BIOS (-smp 4): PASS. Two dedicated AP workers (cores 1 & 2) slamming concurrent appends; 200 records (3200 bytes) delivered with 0 lost, 0 duplicate, strict ordering, 38 & 31 interleaving transitions, clean ACPI S5 poweroff, offline `e2fsck -fn` 0 errors.
  - QEMU UEFI (-smp 4): PASS. Two dedicated AP workers (cores 1 & 2) slamming concurrent appends; 200 records (3200 bytes) delivered with 0 lost, 0 duplicate, strict ordering, 64 & 75 interleaving transitions, clean ACPI S5 poweroff, offline `e2fsck -fn` 0 errors.
- **Phase 4A/4B Verification Runs (`make test-shell-host` and `make test-shell-s6`):**
  - Host ASan/UBSan: PASS (`tests/shell_host.c`). Verified input redirection, output creation and truncation, append, lexical duplication ordering (`>out 2>&1` vs `2>&1 >out`), descriptor close (`>&-`, `<&-`), target variable expansion (`>$TARGET`), and ambiguous redirect rejection (`>$AMBIG`).
  - QEMU BIOS (`make test-shell-s6`): PASS. Initial shell reached; child stdout redirection (`run /bin/hello 10 > file`) silenced terminal; redirected file content verified; child append redirection (`run /bin/hello 20 >> file`) verified; direct path execution (`/bin/hello 30 >> file`) verified; target variable expansion (`/bin/hello 42 > $TARGET`) verified; ambiguous redirect rejection verified; redirection open failure handling verified.
  - QEMU UEFI (`make test-shell-s6`): PASS. Identical 7 verification points verified cleanly under OVMF UEFI.

## Execution checklist

1. [x] Fix duplication errors, stream semantics and parser bounds with focused tests (Phase 2 closed).
2. [x] Phase 3: Implement atomic append in `ext2` and `vfs` under Contract B (`ext2_write(..., &offset, append, ...)`),
   atomic acquire-release refcounting on `file_t`, deliver host append tests in `tests/ext2_host.c`, and deliver
   true multi-core SMP concurrent append integration test suite in `scripts/test_smp_append.py` / `src/kernel/main.c`
   (verified on BIOS and UEFI under `-smp 4` with full offline `e2fsck -fn` audits).
3. [ ] Phase 4: Wire ordered redirections into children (`SYS_SPAWN_EXT`) and scoped parent builtins; retain
   controlling terminal handle (`g_term_fd`); propagate short write errors and exclude private handles from spawn.
   - [x] Phase 4A & 4B: Parse `cmd->redirs`, expand targets via `expand_redir_target`, build `spawn_fd_action_t[]`, populate `opts.fd_actions` and `opts.action_count`, call `SYS_SPAWN_EXT`, and handle ambiguous redirection failures (verified host ASan/UBSan and QEMU BIOS/UEFI).
   - [ ] Phase 4C: Scoped parent builtin redirection (save/apply/restore for builtins).
   - [ ] Phase 4D: Retained UI terminal handle (`g_term_fd`) and CLOEXEC exclusions.
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
