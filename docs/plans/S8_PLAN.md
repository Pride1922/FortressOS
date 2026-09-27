# Shell S8 — Jobs, signals, and process groups

Status: architecture plan, updated 2026-09-27. No S8 implementation or test pass
is implied. This revision resolves the six design questions against the current
S7 code and retains the six-phase implementation sequence.

## Architectural decisions

| Decision | Resolution |
| --- | --- |
| 1. Child records | A: global bounded records, with a dedicated global process lock; the existing `g_sched_lock` is per-CPU. |
| 2. Terminal | A: one real kernel terminal object, shared by all handles to that terminal. |
| 3. Signal invocation | A: user trampoline and `sigreturn`, delivered at safe syscall and user IRQ return boundaries. |
| 4. Control keys | Corrected B: kernel interception at input arrival, independent of reads; shell handles prompt policy through signals. |
| 5. Child notification | B + C: asynchronous SIGCHLD notification, main-loop reaping, blocking foreground wait and nonblocking background collection. |
| 6. Wait ABI | C's compatibility principle via new `SYS_WAITPID`, A's public status encoding, B's explicit internal event fields. Keep `SYS_WAIT` unchanged. |

### 1. Global process identity and child records

`src/kernel/thread.c` currently places child records in `scheduler_cpus[]`.
Both `g_child_records` and `g_sched_lock` select the calling CPU. Merely moving
the array to file scope and retaining that lock would introduce a data race.
Cross-CPU scans would also leave parentage and group membership tied to queue
placement. Choose globally owned metadata while retaining per-CPU run queues.

Use a dedicated rank-1 `g_process_lock` for a bounded live-process registry,
parent/child records, group/session identity and signal publication. It must
never nest with scheduler, ext2, or other rank-1 locks. Do not replace the
scheduler's locks with one global runqueue lock. Update the documented lock
contract and debug assertions when implementing this addition.

Start with 64 global child reservations (`MAX_EXIT_RECORDS`); this is a deliberate
system-wide limit, replacing 64 slots per CPU. Keep legacy kernel-test
`exit_records[]` separate: its overwrite policy must never apply to waitable
children. Reserve a record before constructing a child; fail with the documented
resource error when full. Commit parentage, pgid/sid and signal state before
runnable publication. Release the reservation on every failed spawn. Retain
terminal status until consumed or the parent exits; never retain a dead TCB
merely to preserve wait status.

A child record stores explicit lifecycle/event kind, exit code or signal,
parent PID, PID, PGID and a generation. Live process registry slots remain
reserved through teardown; stable PID/generation handles, rather than naked
TCB pointers, cross lock boundaries. `kill` uses the live registry, including
stopped and orphaned processes; child records are not a target registry.
Snapshot group recipients and publish pending signals under the process lock.
The owning CPU applies scheduling changes after that lock is released. Revalidate
identity before dereference, and never free a registry slot while a consumer owns
an outstanding reference. Do not hold any metadata lock across filesystem work,
user copies, descriptor cleanup, address-space destruction or context switches.

For S8, interactive spawns, input and pipe peers remain BSP-affine. Background
execution is concurrency, not permission to migrate these tasks. Global lookup
removes CPU ownership from identity/status, but does not make `sched_wait_until`
or `sched_wake_all` cross-core. Publish an atomic event sequence and pending
signal state before a persistent notification to the owning CPU; consume that
notification outside the process lock. On the BSP, wait predicates read atomic
state without acquiring the process lock under the scheduler lock. A waiter
snapshots the sequence, scans under the process lock, then sleeps only if the
sequence is unchanged and no actionable signal exists. Producer publication,
notification drainage and BLOCKED insertion must have a tested no-lost-wakeup
protocol. Remote delivery uses owner-CPU notification (bounded timer servicing
is sufficient initially); arbitrary remote runqueue mutation is forbidden.
Cross-core blocking wait channels remain a separate milestone.

Give processes inherited `pgid` and `sid`; bootstrap the interactive shell as
leader of the initial session/group. No public `setsid` in S8. `setpgid` permits
self or eligible direct children in the same session, rejects session leaders
and cross-session joins, and creates a new group only with PGID equal to the
target PID. Keep group identity alive while members or staged launches refer
to it, even if its leader exits. Do not recycle a referenced PGID.

### 2. A real terminal with distinct input and output settings

Introduce one kernel terminal object containing controlling SID, foreground
PGID, input flags (`ISIG` initially enabled), control characters and bounded input
state. Existing `terminal_mode`/`terminal_cols` and `SYS_TERMCTL` describe
per-process output routing, not POSIX input modes. Preserve their ABI; add a
versioned terminal-attributes interface for the shared input settings.

`tcsetpgrp(fd, pgid)` and `tcgetpgrp(fd)` resolve a terminal handle, validate the
controlling session and target group, and operate on that shared object. FD 31
is the shell's retained CLOEXEC handle to the same terminal; it grants no
foreground-read exemption. Redirection of FD 0 must not prevent the shell from
handing off or reclaiming the terminal through FD 31.

Apply the foreground check to every terminal read path, including
`SYS_INPUT_READ`, and recheck before dequeue after each wake. Background access
to a pipe or file remains normal. A background terminal read generates SIGTTIN
for the caller's group; blocked/ignored SIGTTIN returns EIO rather than stealing
input or looping. Background terminal-control mutations generate SIGTTOU unless
blocked/ignored; the shell blocks or ignores it to reclaim the terminal.
Background output stays allowed (TOSTOP is deferred).

Terminal input/foreground publication stays BSP IRQ-excluded in S8. Cross-CPU
terminal mutation is rejected explicitly until its synchronization is implemented.
Never acquire the process lock from an ordinary keyboard/UART handler.

### 3. Trampoline signals, with a complete return path

Pin the following Linux x86-64 signal numbers in `syscall_abi.h`, shared by kernel
and user code. POSIX standardizes signal names/semantics, not these numeric
values; SIGSTKFLT is a Linux-specific reservation, not a POSIX requirement.

| Number | Name | S8 support |
| --- | --- | --- |
| 1 | SIGHUP | Terminate by default; required for shell-exit cleanup |
| 2 | SIGINT | Terminate by default |
| 3 | SIGQUIT | Reserved |
| 4 | SIGILL | Reserved |
| 5 | SIGTRAP | Reserved |
| 6 | SIGABRT | Reserved |
| 7 | SIGBUS | Reserved |
| 8 | SIGFPE | Reserved |
| 9 | SIGKILL | Unconditional termination |
| 10 | SIGUSR1 | Reserved |
| 11 | SIGSEGV | Reserved |
| 12 | SIGUSR2 | Reserved |
| 13 | SIGPIPE | Terminate by default |
| 14 | SIGALRM | Reserved |
| 15 | SIGTERM | Terminate by default |
| 16 | SIGSTKFLT | Reserved, Linux-specific |
| 17 | SIGCHLD | Ignore notification by default; preserve wait status |
| 18 | SIGCONT | Continue |
| 19 | SIGSTOP | Unconditional stop |
| 20 | SIGTSTP | Stop by default |
| 21 | SIGTTIN | Stop by default |
| 22 | SIGTTOU | Stop by default |
| 23 | SIGURG | Reserved |
| 24 | SIGXCPU | Reserved |
| 25 | SIGXFSZ | Reserved |
| 26 | SIGVTALRM | Reserved |
| 27 | SIGPROF | Reserved |
| 28 | SIGWINCH | Reserved |
| 29 | SIGIO | Reserved |
| 30 | SIGPWR | Reserved |
| 31 | SIGSYS | Reserved |

Reserved numbers must never be reassigned. Their definitions do not imply
delivery support: reject them with EINVAL in signal APIs and reject unsupported
mask bits. Supporting fault-generated signals, alarms or SIGQUIT is separate
work; preserve existing fault handling until then. Signal 0 is only a `kill`
existence/permission probe. Use a uint64_t mask with bit `(signal - 1)` and a
named supported-mask constant; validate before shifting. Ordinary signals
coalesce. Unsupported action flags return explicit errors. No realtime queues,
alternate stack, SA_SIGINFO or automatic syscall restart in S8.

SIGKILL and SIGSTOP cannot be caught, ignored or blocked. **SIGKILL wakes a
stopped process to terminate it**, without requiring SIGCONT. The owner CPU
makes it eligible for kernel cleanup, gives kill priority over pending stop or
handler delivery, and never resumes its user instructions first. This is a
scheduler/lifecycle operation, not asynchronous execution of a user handler.
SIGSTOP still waits for a safe boundary; neither signal interrupts a kernel
critical section to tear down its resources.

The mechanism is STOPPED → THREAD_READY → THREAD_RUNNING → normal process exit.
After publishing pending SIGKILL under `g_process_lock`, notify the owner CPU.
With only its scheduler lock held, that CPU removes the task from stopped
membership and enqueues it exactly once as THREAD_READY (the current enum's
runnable state). Resume its saved kernel continuation with its own stack and
address space; the safe-boundary signal check observes SIGKILL before any user
return and invokes normal termination outside all locks. The stopping path must
save a continuation which rechecks signals on resume. This does not generate a
CONTINUED child report. Do not implement a second remote destruction path for
stopped tasks, and test simultaneous SIGCONT/SIGKILL against double enqueue.

Delivery occurs only to a user process at a safe return boundary, with no locks
held and after IRQ acknowledgement where applicable. Cover syscall return and
ordinary interrupt return to CPL3, including timer interruption of a syscall-free
user loop. Do not deliver on NMI, on a kernel-mode interrupt frame, or inside a
filesystem critical section. This is option A; syscall-only delivery would make
CPU-bound jobs immune to Ctrl+C. Signal latency during bounded kernel I/O remains
bounded by that operation's completion.

Use a versioned user signal frame with GPRs, user RIP/RSP/RFLAGS and old mask.
Validate the entire writable stack range and overflow/alignment before building
it; enter the handler with SysV alignment and the signal number in RDI.

#### Version-1 signal-frame ABI and handler entry

All fields below are little-endian uint64_t values. This is a dedicated user
ABI structure, not `interrupt_frame_t`; it contains no kernel vector/error code,
kernel pointer, or return-disposition field. Let S be interrupted user RSP,
F = align_down(S - 224, 16), and H = F - 8. Check subtraction underflow before
calculating either address. Validate the entire writable interval [H, S),
including padding, before writing. The project uses no user red zone; this
placement relies on that existing compilation contract.

| Offset from F | Bytes | Field / initial contents |
| --- | --- | --- |
| -8 (H) | 8 | Handler return-address slot: USER_SIGRESTORER_VIRT |
| 0 | 8 | version = 1 |
| 8 | 8 | size = 224 |
| 16 | 8 | frame_id: matches the kernel's active-frame entry |
| 24 | 8 | reserved = 0 |
| 32, 40, 48, 56 | 32 | Saved RAX, RBX, RCX, RDX |
| 64, 72, 80 | 24 | Saved RSI, RDI, RBP |
| 88, 96, 104, 112 | 32 | Saved R8, R9, R10, R11 |
| 120, 128, 136, 144 | 32 | Saved R12, R13, R14, R15 |
| 152 | 8 | Saved interrupted RFLAGS, including DF |
| 160 | 8 | Saved user RIP |
| 168 | 8 | Saved user RSP = S |
| 176 | 8 | Saved user CS = 0x23 |
| 184 | 8 | Saved user SS = 0x1b |
| 192 | 8 | Saved old blocked mask |
| 200 | 8 | Delivered signal number |
| 208 | 16 | Reserved padding, zero-filled |
| 224 .. S-F-1 | 0–15 | External alignment padding, zero-filled |

The kernel snapshots interrupted registers before overwriting handler arguments.
At handler entry, RIP is the validated handler address, RDI is the signal
number, and RSP = H (8 modulo 16), exactly as after a normal SysV call. The
handler's prologue/calls use space below H. Its epilogue restores RSP = H just
before `ret`; `ret` consumes [H] and enters the restorer with RSP = F (0 modulo
16). The restorer performs `mov rdi, rsp; mov eax, SYS_SIGRETURN; syscall` with
no intervening stack adjustment. If the syscall returns an error, the stub must
terminate via SYS_EXIT and have a non-returning failure fallback; it must never
execute `ret` into the frame header. Successful sigreturn does not resume the
restorer: it restores the interrupted RIP/RSP and GPRs.

Add a shared C ABI header `src/include/signal_frame.h` with the v1 structure,
`SIGFRAME_V1_SIZE`, `SIGFRAME_RETURN_SLOT_OFFSET` and a named offset for every
field above. Emit matching NASM `%define` constants into a generated include
from that same definition source, including SYS_SIGRETURN; do not maintain a
second handwritten offset table. Add `_Static_assert` checks for sizeof and
every offsetof, and explicit Makefile dependencies for the generator, header
and restorer object. Assert the copied position-independent stub fits its page
and contains no unresolved relocation. The negative return-slot offset is not
part of the 224-byte structure.

Validate frame version, size, reserved fields, signal and identity against the
top active-frame entry; reject out-of-order, replayed or absent frames. Nested
handlers allocate a new frame below their current RSP and return strictly LIFO.
Copy and validate the complete proposed restoration before changing registers,
mask, or active-frame depth. Pop the active entry only on successful commit.

**RFLAGS:** Save the interrupted DF bit in the frame before modifying the live
return context. Handler-entry construction must explicitly set
`handler_rflags = sanitize_user_rflags(saved_rflags) & ~(1ULL << 10)` so every
C handler enters with DF clear. Kernel-entry `cld` alone is insufficient: IRET
or SYSRET reloads the user flags. On sigreturn use
`restored_rflags = sanitize_user_rflags(frame.saved_rflags)`, preserving its DF
while enforcing the existing permitted-bit mask, IF/bit 1 and privileged-bit
restrictions. Exact roundtrip tests apply to permitted bits; forbidden bits are
sanitized rather than restored.

**Automatic masking:** With `bit(s) = 1ULL << (s - 1)` and
`UNBLOCKABLE = bit(SIGKILL) | bit(SIGSTOP)`, save the pre-delivery mask as
`old_mask` and install
`handler_mask = (old_mask | action_mask | bit(delivered_signal)) & ~UNBLOCKABLE`.
Validate supported bits before publication. Snapshot the action/mask and commit
the delivery bookkeeping under the process lock without holding it across VMM
validation or user copying; revalidate the pending delivery before commit.
Install the mask before making the handler runnable in user mode. SIGRETURN
restores `frame.old_mask & ~UNBLOCKABLE` after supported-bit validation, alongside
the validated context. Pending signals newly unblocked by restoration are
eligible at the next safe delivery check. A nested frame saves the mask active
at that nesting level. SA_NODEFER remains unsupported; repeated instances of
the executing signal stay pending/coalesced instead of recursively reentering.

Choose a **loader-owned fixed virtual page**, not a toolchain-added PT_LOAD
segment. Reserve `USER_SIGRESTORER_VIRT = 0x00007FFFEFFFE000` (4096 bytes),
immediately below the existing stack guard at `0x00007FFFEFFFF000`. Keep that
guard unmapped. Add the constant to the loader's reserved-region definitions;
reject every ELF segment whose page-rounded range overlaps it, including
overflowing or boundary-straddling ranges. Do not overwrite an existing mapping.

For every user address space, allocate a private zeroed physical frame, copy
the kernel-built, position-independent restorer stub through the kernel mapping,
then install its user PTE as present/user/read-only/executable before publishing
the process. There is no user-writable mapping of this frame. Include the frame
in page accounting, allocation-failure rollback and normal address-space
teardown exactly once. Private frames fit the existing exclusively owned user
leaf-frame destruction contract; do not introduce shared user-frame ownership
for this small stub. The current ABI exposes no user unmap/remap/protect calls;
future such calls must reject modification of this reserved page. This adds one
page per process but requires no changes to individual ELF linker scripts and
works for existing assembly programs as well as the C shell.

The handler's return address points to real
restorer code issuing `SYS_SIGRETURN`; neither an unmapped magic address nor an
executable user stack is acceptable. A bounded active-frame stack records frame
addresses/generations; define a finite nesting limit and terminate only the
faulting process on overflow or an unusable signal stack.
The current user stack is only 4 KiB, with a 512-byte minimum startup floor.
Phase 2B must account for the signal frame plus handler stack usage explicitly;
the nesting cap is not a promise that every frame fits. Test exhausted stacks
and maximal argv/envp without changing the guard or startup packing silently.

Treat every sigreturn field as untrusted. Validate active-frame identity, user
mappings and canonical RIP/RSP; enforce user CS/SS and sanitized RFLAGS, exclude
IOPL/NT/VM and privileged state, and clear unblockable mask bits. Never copy a
user-supplied raw kernel interrupt frame. Preserve the restored RAX instead of
letting the dispatcher overwrite it with a syscall result. An IRQ-origin frame
requires full GPR restoration, including RCX/R11: the existing SYSRET path
clobbers those registers. Add a validated IRETQ user-return path for sigreturn
with correct SWAPGS; do not simply route through the current kernel-test IRET
branch. Keep frame layout, entry stack safety and NMI transition contracts intact.

#### Kernel-owned return disposition

The verified Phase 2B implementation uses a per-entry vector marker in the
kernel-stack syscall frame rather than the originally proposed separate
16-byte scratch slot. Entry builds vector `0x80`. Only successful, fully
validated sigreturn causes the dispatcher to replace it with `0x100`, above
the real interrupt-vector range, and bypass ordinary `frame->rax` assignment.
The marker is kernel-owned and kernel-stack-resident; it is not in the user
signal-frame ABI and is never derived from restored RAX or a user-supplied
vector. A blocking syscall cannot share it with another task. Failed sigreturn
uses the ordinary syscall result/return path.

On return from C, assembly checks the marker **before** popping GPRs. The
sigreturn branch restores all GPRs, skips vector/error and keeps the five-word
IRET frame on the kernel stack, then executes SWAPGS exactly once and IRETQ.
It does not `pop rsp`: IRETQ restores user RSP atomically. The ordinary syscall
branch retains validated SYSRET and the existing armed kernel-test recovery
branch, which returns without the user-return SWAPGS. There is no scratch area
to discard and no assembly-frame-layout or dispatch-signature change.

The user confirmed BIOS/UEFI `test-nmi` acceptance of the four named sigreturn
boundaries (three distinct addresses because after-SWAPGS/before-IRETQ alias),
covering syscall and timer origins. Phase 2C preserves this return mechanism.
Retain NMI-safe GS handling and make no C call after restoring GPRs.

Extend interruptible waits for terminal input, pipes and child waits. A caught
signal returns EINTR if no I/O transferred; preserve positive partial counts and
never replay committed writes or spawn fd actions. Default termination uses the
normal exit/descriptor/reaper path outside locks. Stop only at safe boundaries;
blocked tasks must become eligible to reach such a boundary. STOPPED tasks retain
resources but cannot run on ordinary channel wakes. SIGCONT resumes even when
blocked/ignored as a handler signal; SIGKILL must make a stopped task eligible
for termination. When SIGCONT is generated (queued), clear pending stop-signal
bits; when a stop signal is generated, clear pending SIGCONT. Here "generated"
means signal publication, not incrementing a lifecycle or identity counter.
Perform this cancellation under the process lock even if the signal is blocked.
On resume, interrupted operations recheck their predicates.

Spawn is an exec-like transaction: pending signals start empty; caught handlers
reset, ignored dispositions and mask inherit unless explicitly overridden. Add
validated spawn signal-mask/default-disposition attributes so shell children do
not inherit the shell's job-control ignores. Complete these and pgid assignment,
ordered fd actions, CLOEXEC sweep, argv/envp packing and child record publication
before execution. Signals must not expose a partially constructed child.

### 4. Kernel control-key generation at ingress

The question's option B has two errors: control characters must signal the
**foreground group**, regardless of the reader, and generation cannot wait for
`input_read`. A foreground CPU loop or a process sleeping without reading must
still receive Ctrl+C/Ctrl+Z.

Route both decoded keyboard input and normalized UART input through one bounded
terminal-ingress path. With ISIG enabled, consume VINTR/VSUSP and publish a
preallocated signal event identifying the foreground group at arrival, including
its stable generation. Do not enqueue those bytes as application data. Preserve
keyboard escape-sequence atomicity. With ISIG disabled, return literal bytes.
Define flushing of pending ordinary input on these control events and test it.

Use a bounded deferred terminal event queue with explicit overflow diagnostics
and safe coalescing only for identical group-generation/signal pairs. Hold a
stable group reference until consumption so a later handoff or ID reuse cannot
redirect an earlier key. An explicitly implemented BSP worker drains events and
calls the signal publisher; timer/idle wake it through the established discipline.
No allocation, logging, group scans, scheduler calls or signal-handler invocation
inside ordinary device IRQ handlers. Test progress when nobody is reading.

At the prompt, shell SIGINT handling sets a flag; the main loop cancels the edit
and redraws. The shell ignores SIGTSTP at the prompt. During foreground waits it
does not read input or forward control bytes. Thus `lineedit.c` cases 3/26 are
not the job-control mechanism, and there is no duplicate kernel/shell signal.

### 5. Asynchronous notification, synchronous state ownership

Implement SIGCHLD in Phase 2 for exit, stop and continue. Publish durable child
state before notifying the parent. Default SIGCHLD action ignores notification
but retains waitable status. Explicit SIG_IGN/SA_NOCLDWAIT auto-reaping semantics
are deferred; reject unsupported action requests rather than silently accepting
a conflicting contract.

The shell installs a minimal handler which only sets a signal-safe scalar flag.
It never prints, allocates, edits the job table or reaps in the handler. The main
loop drains `waitpid(-1, ..., WNOHANG | WUNTRACED | WCONTINUED)` until no report is
available; the record table, not the signal count, is authoritative. Foreground
wait blocks on its process group with WUNTRACED/WCONTINUED, handles EINTR and
collects background changes between iterations.

At the prompt, use the existing bounded timed input reads to drain events and
redraw without requiring Enter; also drain on EINTR and before/after launch and
foreground wait. Always scan on timeout, not only when a flag is set, closing
the flag-check-to-sleep race without requiring a new pselect ABI. Mask SIGCHLD
while publishing shell job entries, then unblock and drain. This combines B's
prompt notification with C's straightforward ownership; prompt-return-only
polling would leave completed children consuming slots while the user is idle.

### 6. Preserve SYS_WAIT; add a waitpid contract

Existing syscall 10 accepts `(uint64_t pid, int64_t *status)` and returns zero
with a raw exit value. Old assembly callers do not initialize a third argument.
Do not reinterpret spare argument registers or silently encode this status.
Keep SYS_WAIT exit-only, preserving raw normal-exit values. It does **not**
return an encoded kernel status word today: `sys_wait` copies `record->status`
and existing `user/shell/program.c` callers consume the raw value. Changing
that interpretation would break the old ABI, even if the parameter size stayed
unchanged. Its syscall return remains zero on success; the value below is the
output through the status pointer.

For old callers only, define a lossy signal-death fallback of `128 + signal`
in that raw output. This is an explicit legacy compatibility adapter, not the
kernel's internal status representation or the new waitpid encoding. A normal
exit 130 and SIGINT death are intentionally indistinguishable through SYS_WAIT.
Likewise, a normal exit with code 137 is indistinguishable from SIGKILL death;
use SYS_WAITPID to distinguish them. Internally record `EXITED, exit_code=137`
versus `SIGNALED, signal=9`, never a preconverted 137 for both. The legacy adapter
produces 137 for either; waitpid produces `0x8900` versus `0x0009`.
No WIF*/W* macros or additional `128 + signal` conversion apply to this output.
An unambiguous legacy status would require an explicitly versioned ABI break;
do not claim both unchanged semantics and a new encoded status word.

Migrate every S8 shell wait path to SYS_WAITPID. There the kernel reports the
encoded cause, and the shell alone computes `$?` as WEXITSTATUS for normal exit
or `128 + WTERMSIG` for signal death. Never store that shell conversion back
into child records. This removes double conversion without breaking old callers.

Add `SYS_WAITPID(int64_t selector, uint64_t *status, uint32_t options)`: positive
PID selects one child, -1 any child, 0 the caller's group, and less than -1 the
specified child group. Validate signed negation (including INT64_MIN). Return
the reported child PID, zero for WNOHANG with eligible children but no report,
or a negative error (ECHILD, EINTR, EFAULT, EINVAL). Support WNOHANG, WUNTRACED
and WCONTINUED; reject unknown flags. Validate output before consuming a report.
A NULL status pointer may consume it. Legacy and new wait share one consumption
path; an exit cannot be collected twice.

Use explicit fields internally, encoding only at the new ABI boundary. Choose
the **Linux-compatible wait-status bit layout** in the uint64_t output: normal exit
`(code & 0xff) << 8`, signal death `signal & 0x7f` (no core bit), stopped
`(signal << 8) | 0x7f`, continued `0xffff`; upper bits zero. Supply all WIF*/W*
macros. POSIX specifies observable macros, not a universal binary layout.
The shell converts exit/signal status to its own `$?` convention, including
pipeline-last-stage and negation rules. This is status-layout compatibility,
not general Linux syscall or binary compatibility. Examples: normal exit 7
produces `0x0700` and shell status 7; SIGINT death produces `0x0002` and shell
status 130; SIGTSTP stop produces `0x147f`; continue produces `0xffff`.
Stop/continue reports update job state and are not decoded as normal exits.

Stop/continue reports do not free the child reservation. Keep a bounded latest
unconsumed state-change report plus lossless terminal status; subsequent
transitions may coalesce, and exit supersedes transient reports. Never infer a
job's current state from an old notification count. The shell marks a job done
only when all members exit, and stopped when every remaining member is stopped.
An independently stopped stage does not prove the entire pipeline stopped.

Make the counters and consumption rules concrete:

- `identity_generation` belongs to a registry/record slot incarnation and changes
  only on reuse. It protects PID/slot references, not wait-event delivery.
- Each child record has uint64_t `event_seq` and `reported_seq`, initially zero,
  and a latest event kind/payload. Under the process lock, increment `event_seq`
  on each committed transition into STOPPED, out of STOPPED by SIGCONT, or into
  terminal exit/death. Repeated stop while already stopped and SIGCONT while
  running do not manufacture transition reports. Signal-bit publication alone
  does not increment this counter.
- A transient report is available when `event_seq != reported_seq` and its
  current kind is selected by the wait options. Successful consumption copies
  the snapshot and sets `reported_seq = event_seq` under the same lock. A report
  excluded by the options stays unconsumed; a later transition replaces it.
  Terminal state supersedes any transient report, is always eligible, and frees
  the reservation only after successful collection. Preserve user-output
  validation and the single-threaded/no-unmap copy guarantee.
- Example: stop(seq=1), collect(reported=1), continue(seq=2), stop(seq=3) makes
  the second stop reportable even though the current state is again STOPPED.
  If nothing was collected, only the latest stop at seq=3 remains; if exit
  follows, only its terminal report remains. No transition history queue is
  promised. The parent wake sequence used by wait predicates is separate and
  advances on publication before notification; predicates do not acquire the
  process lock under a scheduler lock.
- Comparisons use equality, not signed ordering. Treat impending counter wrap
  as an invariant failure in debug builds; never silently reset a live counter
  to a value that could match an outstanding snapshot.

Phase 2C tests must cover these exact sequences, option-filtered reports,
duplicate waits, and exit replacing an uncollected stop/continue report.

## Implementation phases and gates

### Phase 1 — Process identity, child records and wait ABI

Implement global metadata/lifetime rules, inherited pgid/sid, setpgid/getpgrp,
new waitpid and unchanged legacy wait. Use spawn options version 2 rather than
silently repurposing version-1 reserved fields; retain exact v1 validation and
64-byte layout. The existing flags field selects group creation/join and staged
launch, with reserved2 becoming pgid only in v2; further signal attributes use a
versioned extension with exact size checks. Unknown combinations are errors.

Add a staged-launch gate: children are fully built but cannot execute until the
parent releases the group. This is a launch state, not a SIGSTOP notification.
It prevents an early pipeline reader receiving SIGTTIN before foreground handoff
and prevents the group leader exiting before peers join. Define a bounded group
release operation and automatic cancellation if the parent dies. Failure cancels
all staged children and releases descriptors, records and group references.

Documentation is a Phase 1 deliverable: update AGENTS.md L1, PROTECTED.md's
rank table, and synchronization-header ownership comments to name the new
rank-1 process lock. Explicitly prohibit its nesting with scheduler/ext2/other
rank-1 locks in either order. Preserve the existing, narrowly scoped ordered
scheduler-pair exception in `sched_lock_pair` and `spinlock.c`; a blanket
"no two rank-1 locks ever nest" would contradict implemented work stealing.
Give the process lock ordinary lock kind, not LOCK_KIND_SCHED. Documentation
changes accompany implementation; this planning edit does not claim that the
new lock already exists.

Gate: capacity/rollback, parent death, group lifetime/leader exit, session
rejection, legacy ABI callers with arbitrary unused registers, wait selectors,
status encoding and no lost wakeups. Exercise global identity from multiple CPUs
without claiming cross-core pipe/input support.
The gate also checks the updated L1/protected/header text against actual lock
kinds, rejects process↔scheduler and process↔ext2 nesting in debug tests, and
retains passing ordered scheduler-pair/work-stealing checks.

### Phase 2 — Signals and stop/continue, independently gated

#### Phase 2A — Signal infrastructure and default termination

Implement the pinned numbers/supported mask, bounded disposition table, pending
publication, sigprocmask, default/ignore sigaction operations and default-action
return hooks. Custom handlers remain explicitly unsupported until 2B. Add safe
interruptible-wait plumbing and spawn mask/disposition initialization now, so
later phases share the same kernel boundary and wake protocol.

`kill` supports positive PID,
0 for the caller's group and negative PGID; reject -1 broadcast in S8. Signal 0
checks existence/authorization without delivery. Until credentials exist, allow
user targets only in the caller's session, exclude kernel/idle tasks, and state
that this is a temporary permission model. Publish to all eligible group members
in one metadata transaction; exit races must not target recycled identities.

Gate: number/mask rejection, signal 0, self/PID/group targeting, default/ignored/
blocked termination signals, syscall-free busy-loop termination, blocked input/
pipe/wait termination, lifetime races, spawn rollback and resource reclamation.
Unimplemented stop/continue operations return explicit errors at this checkpoint.
Run the transition/NMI regressions whenever return hooks change.

#### Phase 2B — User handlers and full-context restoration

Implement custom sigaction, the loader-owned restorer, versioned user frames,
sigreturn and validated IRETQ return with correct SWAPGS. Finish caught-signal
EINTR/partial-I/O semantics and signal attribute overrides for spawned programs.

Gate: exact GPR/RFLAGS/mask roundtrip from syscall and timer-interrupted user
contexts (especially RAX/RCX/R11), malformed sigreturn, stack exhaustion, bounded
nesting, handler alignment, maximal startup arguments and pending delivery on
unmask. Test restorer RX permissions, ELF overlap rejection, mapping-allocation
failure and exact frame reclamation. NMI regression is mandatory for the new
assembly return path; no kernel-test recovery path may substitute for user IRETQ.

Add explicit tests for DF-set interrupted assembly → DF-clear C handler →
DF-restored interrupted context; automatic same-signal blocking, coalesced
pending delivery and nested old-mask restoration; v1 field/offset assertions;
handler/ret/restorer RSP values; invalid or replayed frame IDs; and successful
versus failed sigreturn selecting the correct kernel-owned disposition.

Extend `scripts/test_nmi_transitions.py` rather than merely rerunning its current
seven SYSRET-path probes. Add zero-byte symbols on the real sigreturn path:

| Probe symbol | Exact boundary |
| --- | --- |
| sigreturn_restore_regs | Kernel vector-marker disposition selected, immediately before the first GPR pop; kernel GS and kernel RSP (no separate scratch area in this implementation) |
| sigreturn_before_swapgs | All GPRs restored and vector/error skipped, immediately before SWAPGS; RSP points to the kernel-resident five-word IRET frame |
| sigreturn_after_swapgs | Immediately after SWAPGS; user GS but CPL0 and the same kernel RSP |
| sigreturn_before_iretq | Immediately before IRETQ; may alias the preceding symbol when SWAPGS and IRETQ are adjacent |

Use a Ring 3 fixture which actually catches a signal and returns through the
mapped restorer. Cover both syscall-origin and timer-origin saved contexts,
BIOS/UEFI and at least the existing four rounds per distinct boundary. Inject
real QMP NMIs via hardware breakpoints, without inserted guest waits or INT 2.
Verify IST2 entry, actual GS-base restoration on both sides of SWAPGS, unchanged
kernel IRET frame/stack, permitted RFLAGS and full GPR restoration (especially
RAX/RCX/R11), then continued execution at the interrupted user RIP/RSP. Recognize
aliased probe addresses explicitly rather than counting one instruction twice
as independent boundary coverage. Preserve all original seven probes and the
separate test-recovery checks. A pass covering only SYSRET does not close 2B.

#### Phase 2C — Stop/continue and durable child notification

Implement STOPPED scheduling, stop/continue cancellation/priority, SIGKILL wake
from STOPPED, child transition records and SIGCHLD publication. Exercise default
SIGCHLD plus waitpid first; test caught SIGCHLD after 2B passes.

Gate: stop/continue while running and blocked, repeated/coalesced transitions,
blocked/ignored SIGCONT resume, ordinary wakes leaving STOPPED untouched,
SIGKILL of a stopped task without SIGCONT, no user instruction executed before
kill cleanup, parent-exit races and lossless terminal status. Verify no leaked
descriptors/records/stacks and no spinlock spanning a switch.

Dependency order is 2A → 2B and 2A → 2C. The default-action/child-record tests of
2C do not depend on custom handler invocation and can proceed if 2B is still
under repair. The combined caught-SIGCHLD test requires both. Phase 2 is complete
only after all three gates; Phase 3 integration starts from that accepted state.



### Phase 3 — Terminal foreground ownership

Implement terminal state, fd-based foreground APIs, versioned shared attributes,
SIGTTIN/SIGTTOU enforcement and deferred ingress signal worker. Preserve output
routing and FD 31 CLOEXEC behavior. Bootstrap ownership explicitly before shell
job control becomes active.

Gate: CPU-bound foreground job interrupted with no reader; keyboard and UART;
foreground pipeline-wide delivery; background `cat` stops without stealing data;
redirected background input succeeds; blocked/ignored SIGTTIN; rapid foreground
handoff; ISIG off; worker overflow; shell reclaim via FD 31 with stdin redirected.

### Phase 4 — Shell jobs and background launch

Implement bounded BSS job table, parser `&` on a pipeline, member PID/state
tracking and main-loop reaping. Reject unsupported asynchronous compound lists
explicitly. Background builtins use child stages, never mutate the parent shell.
Reserve a job slot before launching; masked SIGCHLD plus staged group launch
makes registration atomic with respect to execution.

Foreground launch order: build group, register all members, save shell terminal
attributes, hand terminal to job, release launch gate, wait. Background launch
registers then releases without handoff. Failed handoff/partial spawn cancels
staged members, closes pipe endpoints and reclaims ownership. Exited jobs retain
only bounded notification metadata after kernel reaping.

Gate: immediate exit before prompt, full tables, partial pipeline failure,
background completion while idle, no lost command-line editing, terminal-reading
first stage at launch, retained S7 statuses and descriptor ownership.

### Phase 5 — jobs/fg/bg/kill, prompt policy and terminal restore

Add jobs, fg, bg and kill job specifiers `%n`, `%+`, `%-`; preserve stable job IDs
until notification/removal. `fg`: transfer terminal, restore saved job input
attributes, then SIGCONT and wait. `bg`: SIGCONT without foreground transfer.
On stop/exit reclaim terminal, save stopped-job attributes, restore shell
attributes and redraw. Handle all-member state, not just the leader. Report
signal death separately from a normal exit with the same numeric shell status.

On shell exit, apply an explicit S8 cleanup policy: SIGHUP plus SIGCONT to owned
jobs, bounded wait, then SIGKILL and collect remaining children. Kernel parent-
death cleanup cancels staged launches, releases child reservations and ensures
orphaned stopped jobs do not retain resources forever (terminate such groups;
full POSIX orphan-group semantics are deferred). Preserve registry entries for
running orphans until actual teardown.

Gate: multi-stage stop/bg/fg/kill cycle, one-stage stop, foreground completion
with background activity, prompt Ctrl+C/Ctrl+Z, attribute restoration, shell exit
with stopped/full-pipe jobs. Closing pipe descriptors alone cannot terminate a
stopped child; failure cleanup must use the new signal lifecycle.

### Phase 6 — SIGPIPE and integrated acceptance

Publish SIGPIPE when a pipe write cannot proceed because readers are gone,
without invoking delivery while holding the pipe lock. Default disposition
terminates at a safe boundary; caught/ignored/blocked signals preserve EPIPE
(or an already-written positive count). Blocked SIGPIPE remains pending. Remove
forced exit-141 assumptions from tools where signal disposition now governs;
ignored/caught SIGPIPE lets the application choose its error exit code.

Gate: early reader exit, default/caught/ignored/blocked SIGPIPE, partial writes,
stopped producer teardown and unchanged binary/short-I/O behavior.

## ABI allocation and scope

Signal-number allocation is the fixed Linux x86-64 1–31 table in Decision 3.
Copy it exactly into the shared ABI header in Phase 2A; reserved entries remain
unsupported until separately implemented. Do not renumber the supported subset.

The earlier draft used 25–31; these remain proposed, not implemented:
25 setpgid, 26 getpgrp, 27 kill, 28 sigaction, 29 sigprocmask,
30 tcsetpgrp, 31 tcgetpgrp. Reserve 32 sigreturn, 33 waitpid,
34 terminal attributes, 35 staged-group release, subject to checking the shared
ABI at implementation time. Add named errors such as EINTR/ESRCH/EPERM/ENOTTY
without changing existing values or INPUT_LOST. Specify signatures/structures
in `syscall_abi.h` with bounds and static size assertions before writing callers.

S8 is bounded single-terminal job control, not full POSIX conformance. Defer
setsid, PTYs/multiple terminals, credentials, realtime signals, altstack,
automatic syscall restart, TOSTOP and general cross-core blocking channels.
Do not claim arbitrary existing binaries support caught signals without the
loader restorer mapping. Noninteractive shells skip terminal handoff; define
unsupported job-control builtins as errors.

## Verification and evidence

Before implementation, revisit PROTECTED.md, AGENTS.md sections 4/7/9 and the
subsystem public headers. This plan describes the intended synchronization and
return-path changes; it is not evidence that protected transitions are safe.
Review concrete lock ownership and assembly return changes before landing them.

Add host tests using actual metadata/encoding/parser/job logic with mocks clearly
labelled. Add bounded `test-shell-s8` QEMU acceptance in BIOS and UEFI with
SMP=1/4/8 and AP-count evidence, preserving BSP pipe/input affinity. Use disposable
fixtures and offline byte/integrity checks for storage scenarios. Run relevant
`test-pipe-host`, `test-pipe`, `test-shell-host`, `test-pipeline-host`,
`test-stream-tools-host`, `test-shell-s7`, `test-shell-s6-resources`,
`test-smp-append`, `test-shell`, `test-nmi` and `test-smp-vmm` regressions as their
subsystems change. NMI and register tests are mandatory for Phase 2; host mocks
cannot validate return instructions or SMP exclusion.

Physical Dell acceptance follows the checklist below. Record actual
commands, firmware, CPU count and results in `docs/roadmap/shell-s8.md` as phases
complete, then update AGENTS.md status. No new tests were run for this plan edit.

### Dell S8 acceptance protocol — planned, not yet executed

Run after all host/QEMU gates pass. Record build commit/image SHA-256, Dell
model, firmware boot mode, keyboard layout, detected/online CPU counts, selected
USB PARTUUID and actual mount mode. Use the deliberately selected USB; start
with RO. Repeat the stateful cycle in the existing authorized RW configuration
if that configuration is part of the acceptance run. Never change disk selection
or authorize writes by label alone. These scenarios require no test-file writes;
normal shell history may persist in RW mode. Capture framebuffer photos or video
and available serial logs with timestamps and the scenario ID.

Provide a test-image-only `/bin/s8-probe` helper before this protocol is runnable.
Its proposed modes are `busy` (no syscalls after a readiness banner), `delay N`
(bounded lifetime), `source` (continuous pipe data), `sink` (drain until EOF),
and `term-stop` (report attributes, disable ISIG, self-SIGSTOP, report attributes
after continuation, wait for a literal control byte, restore attributes, exit).
Publish the exact helper CLI and expected output with its implementation; these
are acceptance requirements, not claims that the helper exists today. Add a
test supervisor which owns a nested interactive shell and reports all descendant
PIDs, states and resource counts after that shell exits; it must restore terminal
ownership before presenting the outer prompt. This makes cleanup observable
without relying on the future `ps` command. Include `exit-code N` and a bounded
raw/encoded wait comparison mode for the ABI checks.

For each row record PASS/FAIL, actual PID/PGID/job ID, observed transitions,
time to prompt/event and evidence location. Use the observed job ID in place of
`%n`. Unless a row specifies a delay, a control action must complete within
two seconds; reaching the bound is a failure, not permission to mark it passed
after a reboot. Give each scenario a 30-second outer supervisor timeout; its
cleanup reports failures, sends SIGKILL, collects descendants and restores the
terminal. If the system is unresponsive, record the failure before rebooting.

| ID | Actions | Required observations and cleanup |
| --- | --- | --- |
| D1 Prompt policy | Type an unfinished command, press Ctrl+C, then execute `echo ready`; press Ctrl+Z at an empty prompt. | Line cancelled exactly once, next command intact, shell neither killed nor stopped; no stray control bytes. |
| D2 No-reader interrupt | Run `s8-probe busy`; after its readiness banner press Ctrl+C, then inspect `$?`. | Busy process terminates although it never reads or calls the kernel; status 130, terminal returned to shell. |
| D3 Full job cycle | Run `s8-probe busy &`, `jobs`, `fg %n`, Ctrl+Z, `jobs`, `bg %n`, `jobs`, `fg %n`, Ctrl+C. | Same PID/PGID transitions Running → Stopped → Running; bg returns promptly; fg owns terminal before continuation; final job reaped. |
| D4 Background terminal read | Run `cat &`; type `echo untouched` at the prompt; inspect `jobs`; use `fg %n`, type a line, then Ctrl+C. | Background cat stops with SIGTTIN and consumes none of the command; foreground cat reads the line; final cleanup succeeds. |
| D5 Redirected background read | Run `cat /etc/motd &` and a bounded `s8-probe source | head -c 64 &`. | File/pipe reads work in the background without SIGTTIN; jobs complete and pipe producer is collected. Use a verified existing read-only file if `/etc/motd` differs. |
| D6 Idle notification | Run `s8-probe delay 3 &`; begin typing an incomplete command and wait without Enter. | Completion is reported within two seconds of child exit; edit buffer/cursor survive redraw; no stale Running job or occupied kernel record. |
| D7 Pipeline-wide stop | Run `s8-probe source | s8-probe sink`; Ctrl+Z, inspect jobs, bg, fg, Ctrl+C. | Both members share PGID, both stop before job is labelled Stopped, both resume, both terminate; no retained pipe endpoints. Supervisor confirms member states. |
| D8 Kill while stopped | Stop a busy job with Ctrl+Z; use `kill -9 %n` without bg/fg/SIGCONT. | Stopped task terminates and is collected; no further user progress and no immortal stopped entry. |
| D9 Terminal attributes | Run `s8-probe term-stop`; at its self-stop use prompt commands, then `fg %n`; press Ctrl+C once with ISIG disabled. | Shell prompt uses saved shell attributes; resumed helper reports its saved ISIG-off state and receives byte 0x03 rather than SIGINT; after helper exit shell ISIG is restored and D2 still passes. |
| D10 Retained tty handle | Supervisor launches a nested shell with stdin redirected to a fixture/pipe while retaining the tty handle used for UI. Run a foreground job and stop/resume it through that shell. | Foreground control and reclaim use FD 31 successfully; neither redirected stdin nor FD 31 bypasses the background-read check. Supervisor restores outer shell ownership. |
| D11 Shell exit | In a supervised nested shell create a stopped job and a stopped full-pipe pipeline, then exit that shell. | SIGHUP/CONT policy and bounded kill fallback leave no owned live/stopped children, staged launches or leaked records/FDs; supervisor returns terminal to outer shell. |
| D12 Status distinction | Compare `s8-probe exit-code 130` with a busy probe killed by SIGINT using the wait comparison mode. | New waitpid distinguishes exit 130 (`0x8200`) from signal 2 (`0x0002`); shell reports 130 for either. Legacy raw wait reports 130 for either without a second conversion. |
| D13 Repeat/recovery | Repeat D3, D7 and D8 ten times; run `echo hello | wc -l`, then clean shutdown. | Every cycle succeeds; supervisor confirms baseline resource counts after reaping; pipeline prints 1; shutdown succeeds. |

Run keyboard control-key cases on the actual Dell keyboard; QEMU serial-injected
bytes do not substitute for that evidence. Record UART results separately if
available. For RW runs, follow the existing safe unmount/shutdown and offline
filesystem-integrity protocol on the selected test USB. Do not use this checklist
to claim power-loss durability. Any failed row remains open with its reproduction
and logs; repeat the affected scenario after a fix and record the new build ID.

## Reading basis

Repository: `src/kernel/thread.c` and `.h` (per-CPU locks, publication/wait/reaping),
`src/include/syscall_abi.h`, `src/kernel/syscall.c`,
`src/arch/x86_64/syscall_entry.asm`, `src/arch/x86_64/idt.h`,
`src/drivers/input.c` and `.h`, `src/include/terminal.h`, synchronization/VMM
headers and PROTECTED.md/AGENTS.md contracts.

Reference semantics: [Open Group terminal interface](https://pubs.opengroup.org/onlinepubs/007904975/basedefs/xbd_chap11.html)
for foreground access and terminal signals;
[Open Group wait/waitpid](https://pubs.opengroup.org/onlinepubs/9699919799/functions/wait.html)
for selectors, options and status macros. The bounded ABI, lock protocol,
launch gate and deferred features above are FortressOS design choices.
Numeric reference: [Linux signal(7)](https://man7.org/linux/man-pages/man7/signal.7.html)
for Linux x86 signal numbering and uncatchable/unblockable signals;
[Linux wait(2)](https://man7.org/linux/man-pages/man2/waitpid.2.html)
for status interpretation. Linux-compatible numbers/layout do not imply POSIX
conformance for S8's explicitly limited supported set.

For concrete return-path comparison, use the version-pinned Linux v6.12 sources:

- [arch/x86/kernel/signal.c](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/signal.c):
  signal delivery orchestration and interrupted-syscall handling.
- [arch/x86/kernel/signal_64.c](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/signal_64.c):
  64-bit signal-frame setup, sigcontext restoration and rt_sigreturn validation.
- [arch/x86/entry/entry_64.S](https://github.com/torvalds/linux/blob/v6.12/arch/x86/entry/entry_64.S):
  SYSRET eligibility checks and the register/IRET user-return path.

These are implementation references, not a FortressOS ABI specification or a
substitute for the Intel/AMD architectural rules. Compare validation, privilege
boundaries and register preservation; do not copy Linux frame layouts, FPU,
restart, mitigation or alternate-stack machinery into S8 by implication.
FortressOS's explicit frame validation, stack/CR3 ownership and NMI contracts
remain the acceptance criteria.
