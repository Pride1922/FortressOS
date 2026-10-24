# Gate 1 private backend continuation

2026-10-24, starting checkpoint `f2e162e`. The untracked production integration
plan is preserved. Gate 1 is **not complete**. This continuation does not assign
a syscall, change production mount/boot behavior, or enable installation,
production trust provisioning, release builds, pushes or physical writes.

## Source-backed lifetime trace

`src/kernel/process_table.h` explicitly states that snapshots are values and
TCB lifetime is scheduler-owned. `process_group_ref_t` retains a group generation,
not a process. Neither is a package owner reference. The private `pw_owner`
arena in `tests/pkg_writer_candidate.c` supplies independent slot/generation
identity and references: birth owns one; a session retains one; each admitted
operation retains one. Detach cancels and releases birth ownership. Cancellation
cannot retire the session while an operation remains. `pw_complete` consumes
the exact operation generation once; its final release retires a canceled
session and permits reuse. This is private candidate machinery, not an existing
production lifecycle hook.

`process_spawn_internal` in `src/kernel/thread.c` allocates and zeroes a new TCB,
assigns selected/inherited credentials, and constructs descriptors before
publication. `fd_inherit` skips `exclusive_owner` files; explicit descriptor
actions and dup paths also reject them. `fd_close_cloexec` runs during child
construction. There is no `SYS_EXEC`/execve path in `syscall_abi.h`: ELF loading
is new-process spawn, not in-place exec. A future in-place exec implementation
must cancel package sessions before replacing the image; `pw_exec` alone does
not establish that integration.

`fd_close` removes a descriptor and calls `vfs_close`. `vfs_close` decrements
file references atomically; only the final reference invokes the node close
callback, releases the retained node and frees the file. A numeric fd alone
therefore cannot serve as package owner identity. No package file descriptor
is introduced by this continuation.

`process_signal_interrupt` and the user-return signal path observe pending control
signals at safe boundaries; default terminating delivery reaches `process_exit`.
`process_exit` abandons network handoff, excludes local IRQs, calls `fd_close_all`,
publishes process exit metadata outside scheduler exclusion, then calls
`thread_exit`. `fd_close_all` calls the existing **account** PID exit hook before
closing files. That hook is not repurposed here. `thread_exit` itself acquires
the scheduler lock, marks TERMINATED, retires the address space and switches.
Any package cancellation hook must run before that lock acquisition, including
direct thread exit, rather than doing storage work under the scheduler lock.

`sched_post_switch` publishes the previous zombie on its CPU's dead list after
the context save. `sched_reap_dead` detaches each dead list under its scheduler
lock and releases it before asserting stack/CR3 separation, destroying user
tables/stacks, forgetting the process record, closing residual descriptors and
freeing the TCB. Generation owner objects must survive independently until the
last operation release. Reaper calls must be idempotent after normal exit.
No actual package birth/exit/reaper wiring is claimed by the host candidate.

## Source-backed mount trace

`installed_root_boot` in `src/kernel/installed_root.c` selects by recovery
manifest/PARTUUID, validates filesystem identity and flush readiness, and calls
`ext4_prepare_root_diagnose`. This selector is not authenticated package
enrollment authority. Its preflight flush also means a production zero-I/O
refusal claim cannot be inferred from the private constructor tests.

`ext4_prepare_root_diagnose` begins VFS root preparation and invokes
`e4_mount_journal_admitted` in `src/fs/ext4_mount_journal.inc`. The constructor
analyzes JBD2, creates a read-only projected device (`e4_preview_sector`, writes
and flushes reject), opens/checks the projected state, disposes that view,
replays, reopens the real device, allocates maps, durably marks dirty/RECOVER,
recovers orphans and flushes before returning a detached root. Thus package
authentication must precede `jbd2_replay`, and package orphan/namespace fences
must precede generic recovery. Checking only `accounts_prepare` is too late.

The existing copied-source overlay inserts `ad_preview` before disposal of the
read-only projected engine. The extracted finite-profile validator authenticates
the fixture's signed record sets and checks native ownership/maps; the copied
commit/namespace hooks fence subsequent work. These are private explicit
fixture hooks. They do not implement a production enrollment selector, absence
classification, legacy-root policy or incompatible-kernel exclusion.

After detached preparation, `validate_tree` calls `accounts_prepare` before
reading canonical account files; only after tree/account validation does
`ext4_publish_root` call `vfs_boot_publish_root` and publish `e4_active`.
`ext4_root_account_namespace_admit` is structural account admission and cannot
authenticate package records or install a durable package grant.

## Extraction and private value contract

`tests/pkg_private_completion.inc` extracts the accepted validator/recovery
function region verbatim. `tests/pkg_private_status_recover.inc` extracts the
status snapshot and actual EXT4/JBD2 adapter verbatim. Fixture configuration,
locators, keys, certificates, host traps, export and test mains remain in the
private harness. These modules remain private test includes with dependencies
on that harness; this is not a production-independent engine library. No fixture
authority has been placed under `src/` or linked into default dispatch.

The extraction manifest records equality to `f2e162e` after newline normalization:
completion SHA-256 `ffd3ef5074e886f49ed4597651438168312b0de728c7d11ce2a97db13dbe81a4`;
status/recover `1c2c0b32b7ff8c595c6dd00d4c1fcd7ef77f98224f1454115f9417928ca6bc9f`.
Behavioral checks are separate from this textual equivalence check.

`tests/pkg_backend_candidate.h/.c` defines a 32-byte v1 request and 80-byte v1
result with fixed-width fields and explicit zero reserved fields. Operations
1..4 are status/acquire/recover/release; other values reject. Status/acquire
require session zero; recover/release require a nonzero session. Result is
assigned only on success, after storage completes. It contains no pointers,
callbacks, selectors, keys or maps. No syscall number is assigned.

The kernel-only function accepts copied values and a trusted owner key. The
private fixture admission boolean and backend table are supplied by trusted
embedding code, never command-line or request fields. Acquire requires current
effective UID zero and CAP_SYS_ADMIN before storage inspection and rechecks at
writer admission; recover rechecks before admitting its operation. Release can
clean up after credential loss. A pending authenticated grant can be acquired
for recovery; acquisition does not falsely require or publish durable IDLE.
The session binds the backend context and callbacks, rejecting context changes.

A nonblocking atomic execution credit serializes this backend; no spinlock is
held across storage callbacks. The private owner arena uses its existing ordinary
rank-1 lock, disjoint from registry/filesystem locks. An admitted recovery
finishes its outcome after cancellation; the operation reference is released
exactly once before return. Errors leave caller output unchanged and never
automatically repeat a mutation. Session release does not clear on-media state.

## Remaining Gate 1 work

The value backend is currently exercised on the host with actual process registry
code and, separately, the extracted filesystem adapter. `pkg_backend_fs.inc`
supplies host owner birth and fixture admission. This is not a Ring 3 syscall
transport, user-memory validation, production TCB binding or retained mount API.
There is no new boot acceptance. The lifecycle mock tests do not establish real
signal/reaper behavior for this backend. The request layout is a private
candidate pending that integration, not a frozen public ABI.

Still required: remove harness dependencies from production-independent engine
modules; retain a real boot-admitted mount identity; wire trusted owner birth,
cancellation and final drain into copied-kernel lifecycle paths; implement bounded
copy-in/out and noninheritance; run private Ring 3 status/recover on disposable
enrolled roots with real signal/reaper tests and fresh-owner reuse. Resolve
selector integrity and incompatible older kernels before production activation.

Host fault injection and Linux fsck do not establish physical power-loss safety.

## Exact verification and retained failures

All commands ran with WSL `Ubuntu-24.04` at `/mnt/c/Sources/FortressOS`:

| Command | Result and evidence directory under `.codex-remote-attachments/` |
| --- | --- |
| `python3 scripts/test_pkg_cli_transaction.py --block 4096 --sector 4096 --smoke` | 11 scenario groups and 4 Linux audits PASS; overall preservation FAIL retained in `pkg-cli-transaction-20261024-072612-982313` |
| `python3 scripts/test_pkg_writer_lifetime.py --backend` (initial) | 128 mocked-storage backend lifetime cycles PASS in `pkg-backend-20261024-072746-554962`; superseded by final run |
| `python3 scripts/test_pkg_cli_transaction.py --kernel-backend --block 4096 --sector 4096 --smoke` | 11 scenario groups and 4 Linux audits PASS; overall preservation FAIL retained in `pkg-kernel-backend-host-20261024-072852-634262` |
| `python3 scripts/test_pkg_writer_lifetime.py --backend` (final) | ASan/UBSan, strict warnings, 128 backend cycles, freestanding compile PASS in `pkg-backend-20261024-073023-402498`; measured `pb_control` frame 256 bytes, not a transitive stack bound |
| `python3 scripts/test_pkg_writer_lifetime.py` | Original 128 actual-registry/pthread owner-lifetime cycles PASS in `pkg-writer-lifetime-20261024-073154-990583` |
| `python3 scripts/test_pkg_cli_transaction.py --kernel-backend` | Full six-geometry ASan/UBSan actual extracted engine/backend campaign PASS in `pkg-kernel-backend-host-20261024-073146-531274` |
| `git diff --check` | PASS (Git also reports existing working-copy newline conversion information) |

The first smoke campaign retained FAIL because the continuing implementation
changed `pkg_writer_candidate.c/.h` and `test_pkg_writer_lifetime.py` during its
immutable copied-source run. The second retained FAIL because implementation
continued in `pkg_backend_candidate.c`, `pkg_backend_host.c` and
`test_pkg_writer_lifetime.py`. Neither had changed compiled copies or changed
shared-media guards. These are preservation failures, not storage-scenario
failures; they are not relabeled PASS. The final campaign ran with source inputs
unchanged and reports empty changed-copy/source/guard lists.

The final campaign includes 11 CLI scenario groups per geometry, exact signed
five-record terminal comparison, status of committed-uncheckpointed IDLE without
writes, failed stdout after successful durable recovery without retry, malformed
or unsupported CLI inputs and damaged completion-evidence refusal. Existing
inspect/verify regression tests pass through the optional frontend. Host output
and lifecycle transport remain mocked.

| Block/sector bytes | Recovery I/O events | Selected interruptions | Fresh-process recoveries | Linux audits |
| --- | ---: | ---: | ---: | ---: |
| 1024/512 | 908 | 24 | 48 | 4 |
| 1024/4096 | 485 | 24 | 48 | 4 |
| 2048/512 | 1402 | 24 | 48 | 4 |
| 2048/4096 | 397 | 24 | 48 | 4 |
| 4096/512 | 2390 | 24 | 48 | 4 |
| 4096/4096 | 353 | 24 | 48 | 4 |

Totals: **66 scenario groups, 144 selected interruptions, 288 fresh recoveries,
24 Linux filesystem/record audits**. Interruption coverage samples first/middle/
last write/flush events across the inherited fault profiles; it is not exhaustive.
Linux audits cover four retained normal/repeat/output-failure/journal-replay
outputs per geometry, not every temporary interruption image. Passing temporary
cut images are deleted after two fresh-process recovery checks; failures retain
images/logs according to the existing campaign runner.

The backend lifetime test covers absent fixture admission before callbacks,
reserved-field refusal, competing writer, child/stale owner misuse, capability
loss, foreign backend context, unconfirmed recovery failure with unchanged output,
cancellation during an admitted callback, final-operation release and fresh-owner
reuse. It uses the actual process registry but supplies host lifecycle events and
mocked storage. The original writer regression separately exercises pthread
completion barriers and equal-numeric-PID/new-generation rejection.

The [aggregate manifest](../../.codex-remote-attachments/pkg-gate1-summary-20261024/manifest.json)
records exact campaign manifest hashes and the final guard hashes. The
[extraction manifest](../../.codex-remote-attachments/pkg-gate1-extraction-20261024/manifest.json)
records the two source-equivalence regions. The untracked production plan has
not been edited; its recorded current SHA-256 is
`f1e8a769becaac55509a82a1633fda9c6057167588becab8c9cba91521fb1017`.

**Preservation:** final copied/compiled inputs, source inputs, AGENTS.md,
README.md, SSL checksum and shared ELF/initramfs/ISO/raw image guards PASS.
Production `src/`, default Makefile and default CLI sources are unmodified.
Changes remain uncommitted. **Boot acceptance:** none run or claimed; no new
Ring 3, scheduler teardown, enrolled-root boot or physical acceptance follows
from those preservation checks.

## Later continuation

The subsequent [private kernel acceptance](PKG2_GATE1_KERNEL_ACCEPTANCE.md)
supersedes the remaining private Ring 3/lifetime work above for the same finite
profile. Its native boot/runtime results, retained failures and preservation
checks are recorded separately; this earlier host-only evidence remains intact.
The later session implementation retains only a kernel mount identity, with
immutable operations supplied by the trusted embedding, rather than storing
callback addresses in a session. Production activation remains disabled.
