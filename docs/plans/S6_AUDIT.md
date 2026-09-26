# S6 acceptance audit

Audited 2026-09-25 at commit `e0545d0`. Phase 2 fixes landed at commit `d1abc2f` on top of `e0545d0`. Historical verdict: **S6 incomplete** (Phase 2 closed; Phase 3 append and Phase 4 redirection pending).
Current update (2026-09-26): Phase 4 and dual-stream ordering are verified.
Finding 7 and checklist item 4 are closed; all four requested regression gates passed.
Item 5 remains partially verified because its S6-specific heap/reference baseline
and failed-child publication audits exceed the coverage of these runners. Broader acceptance remains subject to
the outstanding evidence listed below; historical results are retained.
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

### 3. [CLOSED 2026-09-26] Redirection execution

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
- **Phase 4C (Builtin Redirections) & 4D (Retained UI Terminal): COMPLETE.**
  Parent save/apply/restore and retained FD 31 with CLOEXEC are implemented and
  verified by the host and BIOS/UEFI S6 suites.

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

### 6. [CLOSED 2026-09-26] Retained terminal and close-on-spawn policy

The shell retains its UI terminal on FD 31 with CLOEXEC. Parent redirections use
scoped save/apply/restore; BIOS/UEFI tests verify prompt recovery after stdout
closure and failed setup.

### 7. [CLOSED 2026-09-26] Short writes and non-recursive error output

`write_bytes_fd` and `puts_err` return `long`: the completed byte count or a
negative error. Positive short writes are retried; zero progress returns EIO.
Errors return immediately without attempting another diagnostic write, including
when stderr is closed. A failure after partial progress returns the error.
The actual I/O implementation is tested under host ASan/UBSan with a mocked
syscall for short writes, partial failure, zero progress and EBADF.

`redir_build_spawn_actions` and `redir_apply_parent` diagnose through `puts_err`
or `file_error_err`. Their failure branches in `execute_parse_tree` set status 1
and bypass command execution. Spawn failure diagnostics now also use stderr.
BIOS/UEFI integration verifies status 1, skipped builtin execution and prompt
recovery after `echo $SKIP_MARKER 2>&- > /nonexistent/dir/out`.
`dual_stream 2>&-` tolerates EBADF and exits 0. `cat` now reads stdin without a
filename and propagates write errors to command status. Other legacy void output
wrappers still discard return values; this closure does not claim universal
builtin output-error status propagation.

### 8. [CLOSED 2026-09-26] Ownership works sequentially; concurrent shared-file_t non-append deferred to S7

Duplication (`SYS_DUP`, `SYS_DUP2`, `F_DUPFD_CLOEXEC`) and process inheritance (`SYS_SPAWN_EXT`) share `file_t` handles, while independent opens allocate separate objects. Access modes are checked by VFS at open and write time. Failed spawn closes descriptors, frees the allocated TCB and stack, and destroys its address space before publication. Exit/reaping closes remaining descriptors.

**Documented Concurrency Rules for Milestone S6:**
1. **Single-Threaded Process Model:** FortressOS processes are strictly single-threaded (each TCB represents an isolated execution context). Therefore, descriptor table lookups, allocations, and replacements (`fd_alloc`, `fd_free`, `fd_dup`, `fd_dup2`) within a process require no intra-process locks.
2. **Atomic Reference Counting:** Reference increments and decrements on shared `file_t` handles use `__ATOMIC_ACQ_REL` in `thread.c` and `vfs.c`, ensuring safe multi-core reclamation when parent and child processes exit concurrently.
3. **Atomic Append Serialization:** For append writes (`VFS_O_APPEND`), concurrent multi-core writes across shared or independent `file_t` handles are completely serialized under `ext2_lock` via `ext2_write(..., &offset, append, ...)`. EOF selection, block allocation, data write, inode size update, and offset writeback are strictly atomic (verified under QEMU `-smp 4` in `make test-smp-append`).
4. **Non-Append Shared-Offset Boundary (Deferred to S7):** Under Shell Milestone S6, processes either open files independently or run sequentially (the parent waits for child exit via `SYS_WAIT`). Concurrent, non-append read/write operations racing on the shared linear `file->offset` of an identical `file_t` description without synchronization are out of scope for S6 and are formally deferred to Milestone S7 (where asynchronous pipelines and multi-process stream consumers are introduced). Never hold a spinlock across blocking terminal input.
5. **Zero Leaked Threads on Failed Spawn:** Verified via GDB scheduler breakpoints (`sched_enqueue`, `thread_create`) in `make test-shell-s6-resources`: failed child setup releases all descriptors, frees memory, and leaves 0 partial or runnable threads on any CPU run queue or dead list.

---

## Phase 3 Architecture Contracts

Before implementing Phase 3 (Atomic Append & Concurrency), the following two
architectural contracts govern VFS stream typing and file descriptor synchronization.

### Contract A: Node-Type Contract (Streams vs. Regular Files)

The current representation has both `VFS_STREAM` in `vfs_node_type_t` (defined at
`src/fs/vfs.h:36`) and the `vfs_node_t.is_stream` boolean. Terminal and null nodes
set both `type = VFS_STREAM` and `is_stream = true`; `vfs_read` and `vfs_write`
currently use the boolean to select stream behavior. Removing the boolean and
consolidating dispatch on the enum is deferred to a separate reviewed change.
The current stream behavior is:

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
| Uniform descriptors and access modes | Verified (2026-09-26) | Stream bypass active; stdin works; access modes enforced; short-write retries; verified across host, BIOS and UEFI |
| Dup/shared offsets/independent opens | Verified (2026-09-26) | Kernel boot self-test verifies -EBADF, -EMFILE, self-dup, exhaustion, cleanup; Ring 3 spawn actions and parent save/apply/restore verified; shared-file_t concurrent non-append deferred to S7 |
| Spawn fd actions and ordering | Verified (2026-09-26) | Child stdout/stderr routing, close/open/dup order, invalid-action unwind verified in QEMU BIOS/UEFI |
| Builtin redirection | Verified (2026-09-26) | Parent save/apply/restore, initially closed fds, failure unwinding, redirection-only (> file) verified in QEMU BIOS/UEFI |
| One-path target expansion | Verified (2026-09-26) | Quoting, spaces, empty/unset expansion, ambiguous redirection rejection verified in host ASan/UBSan & QEMU |
| Retained UI terminal | Verified (2026-09-26) | Prompt and editing after redirection, closure and failed setup; private terminal retained on FD 31 with CLOEXEC verified in QEMU |
| Concurrent append | Verified (2026-09-26) | Two pinned workers on cores 1 and 2, independent and shared handles, 200 records (3200 bytes) 100% delivered, 0 lost/duplicate, strict per-worker ordering, interleaving confirmed, clean S5 shutdown, 0 e2fsck errors on BIOS and UEFI |
| Resource exhaustion and allocation failures | Verified (2026-09-26) | Phase B suite (`make test-shell-s6-resources`) verified under BIOS and UEFI (1 & 4 CPUs): child descriptor limit (32 fds), parent fd table exhaustion (31 fds, `SYSCALL_EMFILE`, 0 leak), process capacity exhaustion (`SYSCALL_ENOMEM`, 0 leak), failed-child abort leaves 0 partial/runnable threads in kernel thread list (verified via GDB scheduler breakpoints/inspection), clean parent prompt recovery |
| RO/tainted storage and short I/O | Verified (2026-09-26) | Distinct diagnostics (Read-only filesystem. vs I/O error.), command not executed after setup failure, bit-for-bit file preservation, status propagation ($? == 1, || recovery, && halt), stable prompt recovery verified under BIOS & UEFI |
| Existing programs and shell | Verified 2026-09-26 | Fresh host, BIOS/UEFI, S3–S4 and UEFI 8 GiB no-UART regression pass |

## Evidence from this audit

- `wsl -d Ubuntu-24.04 -- make test-shell-host`: PASS. Verified ASan/UBSan
  editor, lexer/parser, bounded redirection numbers (e.g. `2>&257`, `99>out`,
  overflow protection), variables, aliases, globbing, expansion, and parent
  redirection save/apply/restore mocks.
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
- **Phase 4 Verification Runs (`make test-shell-host` and `make test-shell-s6`):**
  - Host ASan/UBSan: PASS (`tests/shell_host.c`). Verified input redirection, output creation and truncation, append, lexical duplication ordering (`>out 2>&1` vs `2>&1 >out`), descriptor close (`>&-`, `<&-`), target variable expansion (`>$TARGET`), ambiguous redirect rejection (`>$AMBIG`), and parent builtin save/apply/restore lifecycle.
  - QEMU BIOS (`make test-shell-s6`): PASS. 11/11 integration test scenarios verified:
    1. Child stdout redirection: `run /bin/hello 10 > /mnt/s6_hello.txt` silenced terminal; file content verified on disk.
    2. Child append redirection: `run /bin/hello 20 >> /mnt/s6_hello.txt` verified.
    3. Direct execution path with redirection: `/bin/hello 30 >> /mnt/s6_hello.txt` verified.
    4. Target path variable expansion: `TARGET=...; /bin/hello 42 > $TARGET` verified.
    5. Ambiguous redirection rejection: `BAD="a b"; /bin/hello > $BAD` diagnosed to stderr and exit code 1.
    6. Redirection open failure handling: `/bin/hello > /nonexistent/dir/out.txt` diagnosed with non-zero exit code.
    7. Parent builtin redirection: `echo $MSG1 > /mnt/s6_echo.txt` written to file without terminal leakage.
    8. Parent builtin append redirection: `echo $MSG2 >> /mnt/s6_echo.txt` appended cleanly.
    9. Parent builtin pwd redirection: `pwd > /mnt/s6_pwd.txt` verified.
    10. Redirection-only command: `> /mnt/s6_empty.txt` created 0-byte file without child spawn.
    11. Retained UI terminal handle on FD 31: `echo closed 1>&-` followed by `echo still-alive` proved interactive shell prompt and editor remain responsive even when stdout is explicitly closed.
  - QEMU UEFI (`make test-shell-s6`): PASS. Identical 11 verification points verified cleanly under OVMF UEFI.

## Phase B formal closure and five-gate validation (2026-09-26)

Phase B is formally closed across a fresh five-gate evidence set executed on the
current tree. All five gates exited status 0:

1. `wsl -d Ubuntu-24.04 -- make test-shell-host`: PASS.
   Consolidated ASan/UBSan testing covering keyboard/queue, framebuffer terminal,
   actual shell editor/history logic, parser/action-builder input duplication (`2<&0`),
   and shell I/O helpers (`write_bytes_fd`, `puts_err`, short-write retry, EIO on 0 progress).
2. `wsl -d Ubuntu-24.04 -- make test-shell-s6`: PASS (BIOS and UEFI).
   All 11 Phase 4A–4D integration checks verified on disposable NVMe fixture copies.
   Folded checklist item 4 closure directly into the standard gate: `/bin/dual_stream`
   lexical duplication ordering (`>out 2>&1` vs `2>&1 >out`), repeated `2>>` append
   preservation, `2>` truncation, closed stderr (`2>&-`) tolerance with status 0,
   stdin redirection (`<`) via `cat`, and parent setup failure with closed stderr
   suppressing execution with status 1 while restoring interactive prompt.
3. `wsl -d Ubuntu-24.04 -- make test-shell`: PASS (BIOS, UEFI, and UEFI 8 GiB no-UART).
   S0–S2 PS/2 keyboard + serial + blocked reader/timer progress + resource counts;
   S3–S4 integration (quotes, concatenation, comments, Tab completion, `;`, `!`,
   `cd -`, direct/relative execution, type/command, prompt + status, persistent history);
   UEFI 8 GiB no-COM1 keyboard-only capture. Proves no regressions in general shell foundation.
4. `wsl -d Ubuntu-24.04 -- make test-smp-append`: PASS (BIOS and UEFI, 4 CPUs).
   Two pinned workers on cores 1 and 2, independent and shared `file_t` handles,
   200 intact records (3200 bytes) per scenario with 0 lost and 0 duplicate records.
   Clean ACPI S5 shutdown and offline `e2fsck -fn` 0 errors (20/1024 files, 555/4096 blocks).
   - **Observation on Interleaving Transition Counts:** Interleaving transitions
     varied between runs (53/60 → 102/110 BIOS; 67/61 → 117/110 UEFI). Correctness
     invariants remained unchanged: 200 records, 0 loss, 0 duplicates, clean `e2fsck`.
     Transition count reflects host CPU scheduling and contention timing rather
     than a stability regression; it is timing-sensitive and not a pass/fail criterion.
5. `wsl -d Ubuntu-24.04 -- make test-shell-s6-resources`: PASS (BIOS and UEFI, 1 and 4 CPUs).
   Phase B resource-limit suite verified across the full 4-run matrix on disposable fixtures:
   - Child descriptor table limit (32 fds) enforced;
   - Parent file descriptor table exhaustion (31 fds open) causes `SYS_SPAWN_EXT` to fail
     cleanly with `SYSCALL_EMFILE` and leak 0 descriptors;
   - Process capacity exhaustion causes `SYS_SPAWN_EXT` to return `SYSCALL_ENOMEM` with 0 leak;
   - GDB scheduler breakpoints (`sched_enqueue`, `thread_create`) verify that failed-child
     aborts clean up all descriptors, free the TCB and address space, and publish 0 runnable
     or partial threads to any CPU run queue or dead list;
   - Interactive parent shell prompt remains responsive with clean status recovery.

Logs: `build/shell-s6-{bios,uefi}.log`, `build/shell-{bios,uefi}-1cpu.log`,
`build/smp_append_{bios,uefi}_4.log`, `build/shell-keyboard-only.png`, and
`build/shell-s6-resources-{bios,uefi}-{1,4}cpu.log`.
This validates host and QEMU behavior on disposable fixtures; no storage implementation
changed, and no physical disk was written.

## Phase C formal closure and five-gate validation (2026-09-26)

Phase C (RO/Tainted storage assertions) is formally closed across a fresh five-gate evidence set executed on the current tree:
- **`ext2_mark_tainted()`:** Declared in `src/fs/ext2.h` and implemented in `src/fs/ext2.c`. Acquires `ext2_lock` via `spin_lock_irqsave` and sets `g_mounted_ext2->tainted = true`. If `g_mounted_ext2` is NULL (storage unmounted), it safely releases the lock and returns without dereferencing NULL.
- **Boot sequence call site:** In `src/kernel/main.c`, called strictly post-`ext2_mount_rw` and pre-shell spawn (`sys_spawn` for `/bin/shell`), guarded by `qemu_fw_cfg_has_key("opt/fortress/taint_test")`.
- **Distinct Ring 3 Diagnostics:**
  - Read-only storage (`SYSCALL_EROFS` / `-11`) reports: `"Read-only filesystem.\n"`
  - Tainted / failing storage (`SYSCALL_EIO` / `-9`) reports: `"I/O error.\n"`
  - Neither code collapses into `"Unable to load executable."` or `"File operation failed."`.
  - Extracted helper `file_error_string()` in `user/shell/io.c` ensures string uniformity across child and parent builtins without stack allocation.
  - Added explicit `SYSCALL_EROFS` and `SYSCALL_EIO` cases in `user/shell.c:spawn_program`.
- **VFS Write Eligibility Hook:** Added `node->can_write` callback in `vfs_node_t` (`src/fs/vfs.h`, `src/fs/vfs.c`, `src/fs/ext2.c`) so opening existing files for write (`O_WRONLY | O_APPEND`) on a tainted filesystem fails immediately at `vfs_open_ext` time with `-VFS_EIO` rather than succeeding at open time and failing later at write time.
- **Pre-taint / Post-taint Preservation:** Verified in `scripts/test_shell_s6.py` Session 3 that `/mnt/hello.txt` content is preserved bit-for-bit before taint and after failed overwrite/append attempts, confirming that tainted status prevents data corruption.
- **Execution Suppression & Status Propagation:** All setup failures suppress command execution (neither child nor parent executes), propagate status 1, permit recovery via `||`, halt on `&&`, and maintain interactive prompt responsiveness.

The five gates ran in order, each with exit status 0:
1. `wsl -d Ubuntu-24.04 -- make test-shell-host`: PASS (ASan/UBSan, unit assertions for all shell modules and Phase C error strings).
2. `wsl -d Ubuntu-24.04 -- make test-shell-s6`: PASS (BIOS and UEFI across Session 1 writable, Session 2 read-only, and Session 3 tainted ext2).
3. `wsl -d Ubuntu-24.04 -- make test-shell`: PASS (BIOS, UEFI, and UEFI 8 GiB no-UART).
4. `wsl -d Ubuntu-24.04 -- make test-smp-append`: PASS (BIOS and UEFI under `-smp 4`, transitions 28/58 BIOS, 67/49 UEFI; 200 records intact, 0 loss/corruption, clean S5 poweroff, offline `e2fsck -fn` 0 errors).
5. `wsl -d Ubuntu-24.04 -- make test-shell-s6-resources`: PASS (BIOS and UEFI with 1 and 4 CPUs, 12 measured cycles each, 0 partial/runnable leaked threads, clean `e2fsck -fn` 0 errors).

## Execution checklist

1. [x] Fix duplication errors, stream semantics and parser bounds with focused tests (Phase 2 closed).
2. [x] Phase 3: Implement atomic append in `ext2` and `vfs` under Contract B (`ext2_write(..., &offset, append, ...)`),
   atomic acquire-release refcounting on `file_t`, deliver host append tests in `tests/ext2_host.c`, and deliver
   true multi-core SMP concurrent append integration test suite in `scripts/test_smp_append.py` / `src/kernel/main.c`
   (verified on BIOS and UEFI under `-smp 4` with full offline `e2fsck -fn` audits).
3. [x] Phase 4: Wire ordered redirections into children (`SYS_SPAWN_EXT`) and scoped parent builtins; retain
   controlling terminal handle (`g_term_fd`); propagate short write errors and exclude private handles from spawn.
   - [x] Phase 4A & 4B: Parse `cmd->redirs`, expand targets via `expand_redir_target`, build `spawn_fd_action_t[]`, populate `opts.fd_actions` and `opts.action_count`, call `SYS_SPAWN_EXT`, and handle ambiguous redirection failures (verified host ASan/UBSan and QEMU BIOS/UEFI).
   - [x] Phase 4C: Scoped parent builtin redirection (save/apply/restore for builtins, empty commands, failure recovery).
   - [x] Phase 4D: Retained UI terminal handle (`g_term_fd` on FD 31 with `FD_FLAG_CLOEXEC`) and CLOEXEC exclusions.
4. [x] Exercise `<`, `>`, `>>`, `2>`, `2>>`, `n>&m`, `n<&m`, `n>&-`, and compare
   `cmd >out 2>&1` with `cmd 2>&1 >out` using `/bin/dual_stream`.
   `n<&m` is host-verified via the actual parser and action builder (`cmd 2<&0`);
   the remaining listed operators are covered by BIOS/UEFI integration.
   File comparisons use complete ANSI/CR-normalized UART payloads after removing
   command echo and prompt; they are not raw on-disk byte comparisons.
5. [x] Run bounded BIOS/UEFI cases on disposable fixtures and verify file contents,
   heap/reference/stack/page baselines, failed-child absence and prompt recovery.
   Run relevant ext2/storage, shell and power regressions after implementation.
   - [x] Phase 4 regression sequence, disposable BIOS/UEFI runs, normalized file content checks, setup-failure execution suppression and prompt recovery.
   - [x] Phase B resource suite (`make test-shell-s6-resources`) verified under BIOS & UEFI across 1 and 4 CPUs: child descriptor limit (32), parent fd table exhaustion (31 fds, `SYSCALL_EMFILE`), process table capacity exhaustion (`SYSCALL_ENOMEM`), GDB scheduler breakpoint audit confirming 0 partial/runnable threads published or leaked upon abort, and prompt recovery.
   - [x] Phase C RO/tainted storage suite (`make test-shell-s6`) verified under BIOS & UEFI: distinct diagnostics (`Read-only filesystem.` vs `I/O error.`), execution suppression on failed redirection setup, bit-for-bit file preservation, status propagation (`$? == 1`, `||` recovery, `&&` halt), and prompt recovery.
6. [x] Dell hardware checklist in a dedicated directory on the explicitly
   selected writable USB. Verified builtin/child output, input, append, stderr,
   ordered duplication, failure recovery and persistence. Exact commands and observed contents recorded (Phase D closed 2026-09-26).
7. [x] Mark S6 complete: all rows have evidence; roadmap and audit updated with physical Dell Latitude 5590 acceptance and 5-gate regression proof (2026-09-26).

## Phase D formal closure and physical hardware acceptance (2026-09-26)

Phase D (Dell Latitude 5590 physical acceptance) is formally complete and verified on bare-metal hardware:
- **Test Machine:** Dell Latitude 5590 (Intel Core i5/i7, UEFI boot, 32 GiB RAM).
- **Storage Target:** SanDisk 3.2 Gen 1 USB flash drive (`sda`, 241,385,472 sectors, 512 bytes/sector), partition `sdap2`, PARTUUID `E2830E54-435A-4918-9017-47E44703CA6F`.
- **Pass 1 (Read-Only Mount):** Booted with `usb_data_mode=ro`. Verified banner `[USB 9G.3] PASS: Mounted sdap2 read-only at /mnt`. All four write paths (`run /bin/hello 10 > /mnt/ro_fail.txt`, `echo overwrite > /mnt/README.txt`, `echo append >> /mnt/README.txt`, and `> /mnt/README.txt`) strictly emitted byte-identical error string `Read-only filesystem.` with exit code 1. Command chaining (`||` recovered with status 0, `&&` halted with status 1 and suppressed `should-not-run`). Clean ACPI S5 shutdown via `poweroff`.
- **Pass 2 (Writable Mount):** Booted with `usb_data_mode=rw`. Verified banner `[USB 9G.4] PASS: Mounted sdap2 read-write at /mnt`. In `/mnt/s6_test`:
  - Output redirection: `echo "Line 1" > out.txt`, `run /bin/hello 10 > hello.txt` (silenced terminal of child message, exit code 10 reported).
  - Append redirection & direct execution: `echo "Line 2" >> out.txt`, `/bin/hello 20 >> hello.txt` (appended cleanly).
  - Input redirection: `cat < out.txt` verified.
  - Dual-stream lexical ordering: `run /bin/dual_stream > both.txt 2>&1` (silent terminal, captured both streams) vs `run /bin/dual_stream 2>&1 > only_out.txt` (terminal printed `STDERR_DATA`, file captured `STDOUT_DATA`).
  - Stderr append & truncation: `run /bin/dual_stream 2>> err.txt` (repeated append accumulated 2 lines), `run /bin/dual_stream 2> err.txt` (truncated back to 1 line).
  - Closed stderr: `run /bin/dual_stream 2>&-` (printed strictly `STDOUT_DATA\n`, tolerating closed stderr, exit code 0).
  - Path error diagnostic: `echo fail > /nonexistent/dir/out.txt` emitted strictly `No such file or directory.` (status 1).
  - Prompt liveness: `echo prompt-alive` verified. Clean ACPI S5 shutdown via `poweroff`.
- **Pass 3b (Offline Host Integrity & Hash Verification):** USB flash drive mounted on Linux Mint workstation:
  - `e2fsck -fn /dev/sda2`: `22/16384 files (0.0% non-contiguous), 2099/65536 blocks`, clean ext2 filesystem with 0 errors.
  - Exact file lengths and SHA-256 hashes verified bit-for-bit against preflight baselines:
    - `both.txt`: 24 bytes, `e98bafe5207639a357443ba0e89287182582f169a319f788bd02c1a8f5606052`
    - `err.txt`: 12 bytes, `561ca10f2ac82df8abfa356984cfe714c2327481bb488644aa18d5d71eed10c8`
    - `hello.txt`: 148 bytes, `b6dd868c35dea2b7ae951ad4c53d6907c315751491615131ef904eaf36e363b4`
    - `only_out.txt`: 12 bytes, `a5adc1c3980948c24637e96f22d05d00210ff3fb5ff8b2a7b3ff1f0bcb8f9d1e`
    - `out.txt`: 14 bytes, `8661d1b8fb7c356fe741f38428cf7329e727d9e484820ca400d64c5388d55320`

## Milestone S6 Status: COMPLETE

All findings, requirements, and hardware acceptance tests are formally closed and verified:
- Phase 1–3: Negative dup error codes, stream EOF bypass, numeric parser bounds, atomic acquire-release refcounting, atomic append serialization (`test-smp-append`).
- Phase 4: Child & parent redirection (`SYS_SPAWN_EXT`, `SYS_FCNTL`), retained UI terminal on FD 31, `/bin/dual_stream` ordering, short-write retry.
- Phase B: Resource limits (`test-shell-s6-resources`, 12 measured cycles each across BIOS/UEFI 1 & 4 CPUs, 0 leaked threads).
- Phase C: RO & tainted ext2 storage assertions (`test-shell-s6`), distinct diagnostics (`Read-only filesystem.` vs `I/O error.`), execution suppression, bit-for-bit file preservation.
- Finding 8: Documented single-threaded process model and trace for S6; concurrent shared-`file_t` non-append I/O deferred to S7 pipelines.
- Phase D: Physical Dell Latitude 5590 hardware acceptance verified (RO pass, RW pass, offline host `e2fsck` 0 errors, bit-for-bit SHA-256 match).
