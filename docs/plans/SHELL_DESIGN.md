# FortressOS shell implementation plan

**Status: S0–S4 complete and verified. S5–S10 remain planned.**
**Date: 2026-09-25.** Baseline: repository inspected after SMP completion.

## Approved S0 clarifications (2026-09-25)

1. **Raw byte input.** PS/2 is translated to terminal byte sequences in the kernel;
   UART already supplies bytes. Ring 3 parses escape sequences. No kernel echo.
   Ordinary `SYS_READ` remains compatible. New `SYS_INPUT_READ=17` takes
   `(buffer, capacity, signed timeout_ms)`: -1 blocks indefinitely, 0 polls once,
   1..1000 waits boundedly. Returns bytes, 0 on timeout, -20 on input loss (flushes
   damaged pending input), or an existing error. User ranges are validated before
   sleeping/consuming bytes. Shell requests one byte to avoid stealing a subsequent
   child's input; other clients may request buffered short reads. A syscall alone
   does not imply a scheduler context switch.
2. **Terminal mechanism.** New `SYS_TERMCTL=16` takes `(op, info*, sizeof(info))`;
   shared layout is `src/include/terminal.h` (32 bytes, version 1). GET reports
   dimensions, dropped-byte counter and kernel-display generation. SET selects a
   process output mode: mirror (0), local framebuffer (1), serial (2), or plain (3).
   Standard `SYS_WRITE` for fds 1/2 now uses terminal output, never `dmesg_append`.
   Framebuffer interprets bounded CSI sequences; serial receives the same bytes.
   Early boot/kernel/raw NMI logging retains its existing path. Mirror defaults to
   the smaller of framebuffer width and configured serial width (80 default), or
   framebuffer width when no UART exists; `terminal local` uses full local width.
   Plain mode is a shell rendering fallback, not a kernel sanitization promise.
   PS/2 and UART retain the legacy merged input stream: use one operator/source at
   a time. Separate input sessions are deferred; endpoint selection controls output.
3. **History directory.** In S4, the shell creates `/mnt/.fortress` with `mkdir` on
   the first explicit/eligible save. Existing-directory success requires it actually
   be a directory. No recursive creation for a custom history parent and no write
   attempt outside the authorized mount policy. S2 history is RAM-only.
4. **AltGr is kernel work.** S1 extends the decoder and modifier state, including
   right-Alt make/break. Preserve H4. The requested `AltGr+<` → `|` and `AltGr+(` →
   `{` are supported aliases, alongside Belgian operator positions. Installed XKB
   `be(basic)` was inspected for additional ASCII positions; this is not Dell evidence.
5. **Timeout mechanism.** The scheduler does not currently provide timed waits.
   An input-specific deadline predicate uses `sched_wait_until`; the BSP timer
   increments input ticks and wakes active timed readers. IRQ1/IRQ4 only publish
   bytes/loss/pending flags; the timer performs bounded wakeup before scheduling.
   No new sleep API, polling loop, changed lock rank or lock across context switch.
   Idle prompt reads remain indefinite; only a partial escape uses a 100 ms timeout.
   Ctrl+R redraws after input and needs no periodic wakeup. Kernel log repaint is
   detected on the next input event, not by continuously waking the shell.
6. **Consolidated tests.** `make test-shell-host` runs decoder, console and actual
   editor sanitizer tests; `make test-shell-integration` runs BIOS/UEFI shell plus
   no-UART framebuffer integration. Later parser/jobs/etc. cases extend these
   entry points. Storage-isolated `test-usb-shell-history` remains a separate future
   target to preserve the fixture boundary, not a thirteenth shell target.
7. **Future spawn ABI selected.** Reserve the design below for S5/S6; no syscall
   number or kernel implementation is added in S0–S2:

   ```c
   // spawn_ext(path, &opts, sizeof(opts)) -> pid or negative error
   struct spawn_opts_v1 {           // 64 bytes, 8-byte aligned
       uint32_t size, version;      // offsets 0,4: 64,1
       uint32_t flags, reserved0;   // offsets 8,12: both zero initially
       uint64_t argv, envp;         // offsets 16,24: user pointer vectors
       uint64_t cwd, fd_actions;    // offsets 32,40: user pointers
       uint32_t action_count;       // offset 48: <=32
       uint32_t reserved1;          // offset 52: zero
       uint64_t reserved2;          // offset 56: zero
   };
   struct spawn_fd_action_v1 {      // 32 bytes, 8-byte aligned
       uint32_t op;                 // 0=open, 1=dup, 2=close
       int32_t fd, source_fd;       // target descriptor and dup source
       uint32_t open_flags;         // open only
       uint64_t path, reserved;     // open user string pointer; zero reserved
   };
   ```

   Copy and validate the complete request, vectors, strings and actions before
   publishing a runnable child. Null envp means empty environment; null cwd inherits
   parent cwd; argv is null-terminated under the current argument limits. Null argv
   preserves legacy default argv[0]=path. Actions apply in array order. Unused fields,
   unknown flags/opcodes and nonzero reserved fields fail. Reserve process resources
   before destructive opens; fd/file effects are not transactional filesystem edits.
   Baseline standard terminal streams exist; inheritance of other descriptors is
   explicit through actions. Define limits for envp jointly with stack layout in S5.
   Later group attributes require a versioned extension. Legacy spawn remains intact.
8. **`run` policy.** Silently supported, no deprecation warning. Any later removal
   or warning is a separate user-facing compatibility decision.

Bracketed paste in S2 converts CR/LF/tab to spaces and requires two explicit Enter
presses after the closing marker; syntactic multiline commands arrive in S3. A
truncated paste remains cancellable with Ctrl+C. History/search results are never
executed automatically. Overflow remains editable for inspection but requires
Ctrl+C before execution can resume.

S0 tracing also resolved the apparent global child table: `g_child_records` is a
macro selecting per-CPU storage, and spawned shell children remain CPU-affine. No
cross-core child-table rewrite is part of S0–S2. The input queue is explicitly BSP
owned and reads from another CPU are rejected.

## 1. Recommendation and scope

Build a comfortable interactive shell first, then a composable command environment,
then job control and scripting. Each milestone must be useful on its own. Do not
make arrows and history wait for signals, a persistent root filesystem, or a complete
Unix process model.

The complete usability release (S0–S4) should deliver reliable editing, history, working directories,
consistent quoting, command/path completion, a useful prompt, and optional durable
history. The second should deliver environment inheritance, redirection, pipes and
small reusable utilities. The third adds background jobs, interruption/suspension,
and a deliberately specified scripting language.

Use familiar shell syntax, but describe exactly what FortressOS supports. Full
POSIX/Bash compatibility is not an acceptance criterion. Shell usability does not
require fork, a hosted C library, USB keyboard support, networking, or MicroPython.

This plan incorporates all 22 items from the supplied notes and adds terminal
capabilities, paste safety, AltGr, bounded resource handling, process ownership,
diagnostics, configuration recovery, and a practical utility set. The proposed
defaults below are decisions to review, not questions blocking this draft.

### Delivery map

| Piece | User-visible result | Main dependency | Relative size/risk |
| --- | --- | --- | --- |
| S0 | Stable shell structure and explicit contracts | Baseline audit | Small / low |
| S1 | Arrows, Ctrl keys, terminal cursor/erase support | Input and terminal ABI | Medium / high boundary sensitivity |
| S2 | Editable long lines, history, search and safe paste | S1 | Medium |
| S3 | `cd`, `pwd`, consistent quotes, direct command execution | Parser and cwd support | Medium |
| S4 | Completion, prompt, optional persistent history | S2–S3; storage gates | Medium |
| S5 | Variables, exported environment, aliases and globs | S3 | Medium |
| S6 | Real stdin/stdout/stderr and redirection | Descriptor and spawn redesign | Large |
| S7 | Streaming pipelines and useful command tools | S6 and reviewed blocking contract | Large |
| S8 | Background work, Ctrl+C, Ctrl+Z, `jobs`/`fg`/`bg` | S7 and process/terminal ownership | Large |
| S9 | Scripts, functions, substitution and control flow | S5–S8 in sub-stages | Large |
| S10 | Daily-use polish, introspection and documentation | Incremental after S4 | Variable |

Recommended first implementation authorization: **S0–S2**, followed by a Dell
review before S3–S4. These are review boundaries, not estimates of days or sessions.
S0–S4 together are the first complete usability milestone.

## 2. What actually exists

These are source observations, not new runtime test results.

| Area | Current implementation | Consequence |
| --- | --- | --- |
| Shell | [`user/shell.c`](../../user/shell.c) combines input, builtins, editor and execution; `line[192]` discards overflowed commands | Split responsibilities; preserve refusal to execute truncated input |
| Syntax | `&&`/`||` scan raw text without quote awareness; only `run` has limited double-quote parsing; `$?` is special to `echo` | Replace parsing, do not add more string splitting |
| Commands | `help`, `ls`, `cat`, `edit`, `mkdir`, `rm`, `mv`, `sync`, `echo`, `run`, `layout`, power commands, `exit`, `dmesg` | Preserve supported workflows; no `cd`/`pwd` implementation found |
| Keyboard | [`keyboard.c`](../../src/drivers/keyboard.c) returns one ASCII byte, ignores most extended keys, tracks Shift/Caps but not Ctrl/AltGr | Editing is not a userspace-only change on the Dell |
| Input | [`input.c`](../../src/drivers/input.c) merges PS/2 and UART into a 256-byte queue; local IRQ exclusion and blocking read | Multi-byte key publication, overflow and CPU ownership need explicit contracts |
| Display | [`console.c`](../../src/drivers/console.c) implements CR/LF/tab/destructive backspace; no escape parser; dimensions are kernel-only | Cursor-left cannot be implemented as the current destructive backspace; never hardcode 240×67 |
| Output/logging | [`syscall.c`](../../src/kernel/syscall.c) sends fds 1/2 through `serial_putc`, which also updates framebuffer and `dmesg` | Separate interactive terminal output from kernel diagnostics |
| File I/O | Create/truncate/read/write exist; `VFS_O_APPEND` is declared but rejected by [`vfs_open_ext`](../../src/fs/vfs.c) | `>>` and history append need real implementation |
| Descriptors | Fds 0/1/2 are syscall special cases; allocation starts at 3; no duplication syscall | Redirection needs uniform descriptor objects, not only parser syntax |
| Launch | `SYS_SPAWN` and `SYS_WAIT` exist; children inherit no descriptors; no envp input | Extend the spawn model rather than requiring fork |
| Limits | [`elf.h`](../../src/kernel/elf.h): 32 args, 256-byte argument string limit, 2048 total argument bytes, one 4 KiB initial user stack page | A longer editable command does not imply larger executable arguments |
| Lifecycle | [`thread.h`](../../src/kernel/thread.h) still documents bootstrap-only spawn/wait paths; source uses per-CPU child records through macros and local IRQ publication | SMP completion is not proof that these paths support arbitrary-core concurrency |
| Storage | `/mnt` depends on explicit USB selection and mount eligibility; root/initramfs is not a writable home | History/configuration must work without persistence |

Documentation discrepancies to reconcile in S0: AGENTS §7.6 says no exec syscall
exists while spawn/wait are implemented; input/header comments retain single-CPU
assumptions; AGENTS I1 prohibits ordinary-handler `sched_wake_all`, while existing
input handlers call it and §7.2 describes waking waiters. Trace the actual scheduler
implementation and agree the intended contract before modifying those paths. Do
not silently interpret stale text as authorization to change synchronization.

## 3. Architecture and binding design choices

### 3.1 Keep mechanisms separate

Proposed layout (create modules when their piece lands):

```text
user/shell.c                 entry and interactive/noninteractive loop
user/shell/lineedit.c/.h     editor state machine, no direct kernel dependencies
user/shell/history.c/.h      navigation, search, bounded persistence format
user/shell/lexer.c/.h        tokens with source spans and quote provenance
user/shell/parser.c/.h       bounded syntax tree, incomplete/error distinction
user/shell/expand.c/.h       variables, tilde, patterns, later substitution
user/shell/execute.c/.h      builtins, launch, redirections, pipelines
user/shell/builtins.c/.h     registry, help, completion metadata, state effects
user/shell/jobs.c/.h         job table and child-event handling
user/shell/config.c/.h       options and startup policy
user/lib/                   syscall wrappers, byte I/O, paths, terminal client
user/tools/                 stream utilities and later standalone editor
```

Kernel terminal/input, descriptor, pipe, and process mechanisms belong in their own
subsystems. Kernel code must not know shell history, aliases, completion or grammar.
Keep the existing shell ELF/initramfs path; update explicit Makefile dependencies.
Extract the embedded editor without changing its persistence behavior in S0.

Introduce a small shared user ABI header for user-visible constants/structures,
without importing kernel-only IDT structures into every user program. Preserve all
existing syscall numbers and calling conventions. New structured requests use size,
version, reserved-zero fields, bounded arrays and fully validated user pointers.
Names in this document are proposals, not assigned syscall numbers.

### 3.2 Bounded does not mean destructive

Proposed initial limits, centralized and reported by `help limits`:

| Resource | Initial proposal | Limit behavior |
| --- | --- | --- |
| Logical input command | 4096 bytes excluding NUL | Keep editable buffer; require Ctrl+C cancellation after rejected input before execution |
| History | 1000 entries and 256 KiB total, whichever fills first | Evict oldest complete entries |
| Completion | 256 candidates, 64 KiB storage | Show explicit partial-results notice; never infer a unique match from a truncated search |
| Syntax/expansion arena | 128 KiB; nesting depth 32 | Error before execution of that command unit |
| Pipeline | 8 stages initially | Reject before creating children |
| Background jobs | 16, also constrained by available process slots | Refuse launch, retain existing jobs |
| Command substitution | 64 KiB captured output; nesting depth 8 | Fail expansion and clean up children/endpoints |

Use bounded static/BSS arenas first; no userspace allocator syscall exists today.
Never put these arenas on the one-page user stack. Measure worst-case stack usage
including nested parser calls and syscall wrappers; use iterative parsing where
appropriate. Larger stacks or a userspace heap are separate memory-ownership work.
Shell limits and kernel argument/path/descriptor limits remain distinct; error
messages identify the exhausted limit. Rejected paste bytes must never silently
turn into an executed shortened command.

### 3.3 Input and terminal contract

Use a documented terminal byte stream with a small escape-sequence subset. Translate
PS/2 key events into that stream; accept equivalent UART sequences. Internally the
decoder should return a key event or bounded byte sequence, not pretend an arrow is
an ASCII character. Publish an entire synthesized key sequence or none of it.

Expose terminal capabilities/dimensions and input-overrun state to Ring 3. Stage S1
must select an explicit active interactive endpoint (framebuffer or serial); mirrored
diagnostic output is not two terminals with identical geometry. Default to the local
framebuffer when present, allow explicit serial/plain mode, and retain existing
serial test interaction. No input-source stealing based on arbitrary received bytes.
A full multi-terminal session model is later work.

Initial output subset: CR/LF, non-destructive cursor movement, erase-to-end/all-line,
clear-screen/home, basic colors/reset, cursor visibility. Parse bounded parameters
across write boundaries. Unsupported/malformed sequences recover without allocation,
unbounded scanning or reading outside the buffer. Match wrap behavior explicitly.
Keep a plain-output fallback and disable control sequences for redirected output.

Keep early boot, panic and raw NMI output independent of terminal escape state.
User terminal output must not be appended to the kernel log. Kernel messages should
not interleave inside an escape sequence; define serialized output transactions and
a display-generation/repaint notification for interrupted prompts. No new lock can
violate rank 5 console ordering or keep interrupts disabled for unbounded rendering.

Use a horizontal viewport for the first editor, avoiding right-margin wrap. Logical
continuation lines can come later; do not confuse visual wrapping with shell grammar.
Serial terminals without reported size use configurable conservative dimensions and
plain fallback, never framebuffer dimensions. An isolated Escape must not leave
input stuck: provide a reviewed bounded read timeout/cancellation mechanism, without
busy polling or adding an ad-hoc scheduler sleep primitive.

### 3.4 Execution and process state

Keep spawn-and-wait. Add a versioned spawn request with explicit fd actions, cwd,
environment and later process-group attributes. Construct all child state before
the child becomes runnable. Keep the legacy spawn behavior as a compatibility wrapper.
No parent descriptor mutation as a substitute for configuring an external child.

Use process-owned cwd with `chdir`/`getcwd` semantics and child inheritance. All
path-taking syscalls resolve relative paths consistently, not just `cat` and `ls`.
Initially use a normalized absolute path with revalidation, bounded by `VFS_MAX_PATH`;
reject relative operations if the saved directory no longer resolves. Absolute `cd`
recovers. Document that transparent cwd survival across ancestor rename/removal is
deferred until directory handles/node lifetime support exists. Normalize repeated
slashes, `.` and `..`, clamp traversal at root, reject overflow instead of truncating.

Retain current CPU affinity for the first usability release. Before S6/S7, audit
descriptor references, child records, wakeups, VFS shared state and terminal queues
for multi-core operation. Either keep an explicitly enforced compatible CPU scope or
complete a separately reviewed SMP-safe ownership design. Merely disabling local
interrupts is not inter-core exclusion.

## 4. Implementation pieces and acceptance

### S0 — Foundation and baseline

1. Record baseline behavior and relevant tests; reconcile the documentation issues
   in §2 through code tracing before touching protected contracts.
2. Split shell I/O wrappers, command registry and editor from the main loop without
   intentional behavior changes. Make errors use stderr-ready helpers. Remove the
   current recursive error-reporting path in `write_bytes` when output itself fails.
3. Define shared limits, error/status conventions and module interfaces. Preserve
   `run`, existing power commands and shell-supervisor restart behavior.
4. Add host harnesses that compile actual pure shell modules with syscall adapters.

**Acceptance:** existing shell/input/console/power tests remain valid; standalone
shell links with strict freestanding flags. Capture current known bugs as tests for
later pieces, not assertions that accidental behavior must remain forever.

### S1 — Keys and terminal foundation

1. Decode arrows, Home/End, Delete, left/right Ctrl, Escape and relevant Alt/AltGr
   transitions. Preserve US and Belgian AZERTY Shift-Lock and ISO-key evidence.
2. Make every shell operator typeable on Belgian AZERTY, including `|`, `\\`, `{}`, `[]`
   and `~`. Verify actual Dell combinations rather than assuming a French layout.
3. Implement the terminal subset, capabilities and bounded partial-sequence recovery
   from §3.3. Keep keyboard/UART parsing out of shell grammar.
4. Surface queue loss. On overflow, invalidate the pending command until explicit
   acknowledgement/cancellation; never execute input with unnoticed missing bytes.
5. Resolve the IRQ wakeup discrepancy through the existing publication/deferred-wake
   discipline. Keep IRQ handlers bounded and free of rendering or allocation.

**Acceptance:** host decoder/terminal tests cover press/release, modifiers, full
queues, fragmented/unknown sequences and bounds. QEMU verifies real IRQ1/IRQ4 delivery,
timer progress and no-UART operation. Dell verifies the full key/operator matrix.
Test narrow and wide displays and an escape sequence split at every byte boundary.

### S2 — Line editor and memory history

Implement insertion at cursor, Backspace/Delete, arrows, Home/End, Ctrl+A/E,
Ctrl+W/U/K, Ctrl+Y (single kill buffer), Ctrl+L (clear and repaint), and Ctrl+C
(cancel the current command). Ctrl+D exits only on an empty prompt; otherwise it
deletes at cursor. At this stage Ctrl+C does not claim to interrupt a running child.

Add Up/Down history with restoration of the unfinished draft, consecutive duplicate
suppression, `history`, `history clear`, and incremental Ctrl+R search. Enter accepts
the search result for editing; a subsequent Enter executes it. Escape/Ctrl+G cancels
search and restores the draft. History recall never executes by itself.

Keep history expansion (`!!`, `!N`) disabled by default and interactive-only when
later enabled; display the expanded line for confirmation. Do not reinterpret `!`
inside scripts or single quotes as history syntax.

Handle bracketed paste where supported: pasted newlines create pending input for
review, not immediate execution. Unknown/unbracketed UART input cannot reliably be
distinguished from typing; document this limit. Do not claim universal paste safety.

**Acceptance:** model-driven editing tests verify `cursor <= length < capacity`,
draft/search restoration, no prefix execution after overflow, and byte-exact accepted
commands. QEMU checks lines longer than 191 bytes, end-of-screen editing, queue loss,
rapid keys/paste and shell restart. Dell confirms arrows, Ctrl keys and visible cursor.

### S3 — One parser, working directories and command discovery

Replace raw delimiter scans with lexer/parser output shared by builtins and external
commands. Preserve quote provenance for later expansion. Support single quotes,
double quotes, backslash escapes, empty arguments, concatenated quoted/unquoted word
parts, `;`, `&&`, `||`, and pipeline-negation `!` semantics (initially simple commands).
Specify whitespace and comment handling; `#` starts a comment only at a token start
outside quotes. Distinguish incomplete input from invalid input; use a continuation
prompt for open quotes/backslash-newline, with Ctrl+C cancellation.

Parse the complete logical command before executing it so a syntax error at its end
does not execute an earlier command. For future pipelines/lists use this precedence:
pipeline, then equal-precedence left-associative `&&`/`||`, then list separators.
Expand only branches that actually execute.

Add `cd [path]`, `cd -`, `pwd`, and process cwd support (§3.4). Default initial cwd is
`/`; `cd` without an argument uses configured HOME or `/`. Preserve absolute paths.
Allow `/bin/hello arg`, `./tool` and `hello arg` through initial `/bin` search; retain
`run` as a compatibility wrapper. Builtins win name lookup; add `type`, `command`,
`true`, `false`, `help <name>`, and `exit [status]`.

**Acceptance:** `echo 'a && b'` stays one command; `false && echo no || echo yes`
prints only `yes`; malformed quotes/operators execute nothing. Test empty quoted
args, missing executables (127), invalid executable (126), general failure (1),
syntax failure (2), child status and builtin status consistency. Test relative paths
for every path syscall, inherited cwd, missing cwd, root traversal and overlong paths.

### S4 — Completion, prompt and durable history

Completion uses the lexer in tolerant/incomplete mode. Complete builtin names and
executables from the current search path; complete path arguments through directory
iteration. Respect cursor position, quotes, spaces, hidden files and directory `/`
suffixes. Insert a common prefix first; a second Tab lists bounded sorted candidates
and repaints. Never execute a candidate or command substitution to complete input.
Add command-specific providers for `cd`, `layout`, help topics and later jobs.

Default prompt: `fortress:<cwd> $`, with a compact nonzero last-status indicator.
Provide bounded PS1-style placeholders for cwd, basename, status and hostname, plus
PS2 for continuation. Prompt templates are data, not executable shell code. Color
escapes have zero display width; untrusted path/control bytes are rendered safely.

Default in-memory history always works. Proposed optional persistent path:
`/mnt/.fortress/history`; keep the path configurable for a future `/paradise` home.
Attempt writes only on an already authorized writable mount; do not remount, select
a device or create an alternative writable target. Warn once on RO/unavailable/error
and retain the session history in RAM. Add `history save/load`, recording on/off,
optional leading-space exclusion, and a status display for the last successful save.

Use a versioned, length-framed bounded format so multiline entries and truncated
files are unambiguous. Load only complete validated entries; never execute contents.
No timestamps until a time source is available. Checkpoint at command boundaries
after a configurable batch and on clean shell exit/power-command handling; never
write per keystroke. A successful save includes the applicable sync operation.

**Storage gate:** inspect actual rename replacement and durability behavior before
choosing temp-file replacement. If replacement is unavailable, implement/review it
separately or use two bounded snapshot slots with generation, length and checksum,
always retaining the previous validated slot until the new one validates and syncs.
Do not implement rotation as unlink-old-then-rename. Neither strategy grants ext2
power-loss atomicity. Initially one designated interactive shell owns persistent
history; concurrent sessions need merge/serialization before they may write it.

**Acceptance:** quoted completion, ambiguous/truncated candidate sets, unreadable
directories and prompt-width tests. USB-only disposable multi-boot tests exercise
absent/RO/RW mount, corrupt history, write/sync failure and bounded rotation, with
offline `e2fsck -fn`. Clean reboot demonstrates persistence; no sudden-power-loss
guarantee. Dell checks persistence only on the deliberately selected test USB.

### S5 — Variables, environment and expansion

Implement shell variables, `NAME=value`, command-local assignments, `set`, `unset`,
`export`, `env`, `$NAME`, `${NAME}`, `$?`, `$$`, and later `$!`. Pass bounded copied
envp through the versioned spawn API and document the initial stack layout. Keep
existing argc/argv startup valid and account for argument strings, env strings,
pointer arrays, alignment and remaining stack space together.

**Binding S5 Architecture Decisions:**
1. **Variable Scoping:** Variables are **flat for S5, scoped for S9**. The shell maintains
   a single flat variable table per session with export flags. Command-local assignments
   (`FOO=bar cmd`) temporarily apply to the flat environment during the invocation of `cmd`
   and revert immediately upon completion; block/function local scoping is deferred to S9.
2. **Envp Budget and Stack Layout:** The user stack remains strictly **4 KiB (one single page frame)**,
   preserving existing kernel guard and memory-accounting invariants. To guarantee safe headroom
   for user program stack frames, the string budget is partitioned: `MAX_TOTAL_ARGS_LEN` is budgeted
   at **1024 bytes** (max 32 args, 256 bytes per string) and `MAX_TOTAL_ENVP_LEN` is budgeted at
   **1024 bytes** (max 32 env entries, 256 bytes per string), capping combined string payload at
   2048 bytes. Combined with pointer vectors (argv + envp + auxv = 568 bytes max), this guarantees
   at least 1480 bytes of free stack space for user execution without growing the user stack.

Defaults: `PATH=/bin`, `HOME=/`, explicit terminal mode and PS1/PS2. Do not implicitly
search `.`. Add tilde expansion, `alias`/`unalias` with recursion/cycle bounds, and
pathname patterns `*`, `?`, `[abc]`, `[!abc]`. Hidden names require an explicit leading
dot; sort results; unmatched patterns remain literal unless a documented failglob
option is enabled. Brace expansion is optional sugar, implemented separately from
filesystem globbing and off until specified.

Write an expansion-order table: lexical quoting → optional interactive history/
alias processing at defined token positions → tilde/parameter/substitution → word
splitting for eligible unquoted results → pathname expansion → quote removal.
Initially use fixed whitespace splitting rather than exposing arbitrary IFS. Quoted
empty strings remain arguments. Expanded bytes never become new grammar operators;
there is no implicit `eval`. Bound final argument count and bytes before launch.

**Acceptance:** quotes suppress appropriate expansions; assignments to a child do
not leak into the parent; exported values inherit while local variables do not;
missing/empty variables, expansion explosion and cyclic aliases fail predictably.
Ring 3 tests cover envp pointer validation and combined stack/argument limits.

### S6 — Descriptors and redirection

First make descriptors 0–31 uniform. Standard streams become terminal/null/file/pipe
objects with operations and access modes. Separate descriptor flags (e.g. close on
spawn) from shared open-file descriptions (offset/status flags). Duplication shares
offset and references; independent opens do not. Define locking/refcounts, close,
exit and failed-spawn cleanup before adding syntax. A terminal capability query on
a redirected stream must identify it as nonterminal.

Add duplication/close and spawn fd actions. Validate every action before publishing
a child; reserve process/fd resources before destructive opens where possible.
Apply redirections in source order: `cmd >out 2>&1` differs from `cmd 2>&1 >out`.
Support `<`, `>`, `>>`, `2>`, `2>>`, `n>&m`, `n<&m`, `n>&-`; prefer explicit `2>&1`
to an ambiguous bare `>&` shorthand. Expand a redirection target to exactly one path.
Report setup failures on stderr and do not run the command. File effects already
performed by earlier redirections are not transactionally rolled back.

Builtins executed in the parent use scoped save/apply/restore, including failure
paths. Prompts/editor traffic use a retained controlling-terminal handle, not the
temporarily redirected stdout. Read/write loops handle short I/O and errors without
recursive reporting.

Implement append at the filesystem operation boundary: EOF selection and write must
be serialized together across separate opens. Setting the initial offset to size is
insufficient. Preserve ext2 IRQ-save I/O and existing USB mount/durability policies.
Add optional `noclobber` only with atomic create/exclusive semantics; postpone that
option rather than implementing a racy stat-then-open check.

**Acceptance:** redirection of builtins and children; ordering; closed/bad fds;
descriptor exhaustion; RO/tainted storage; short writes; shared-offset semantics;
concurrent append; and shell prompt restoration after every failure. No leaked
references or runnable half-initialized children. Existing programs still run.

### S7 — Pipes and stream utilities

Implement bounded anonymous pipes with separate reader/writer references. Empty
read blocks while writers exist, then returns EOF after the last writer closes.
Full write blocks while readers exist; absent readers return a defined broken-pipe
error (later default SIGPIPE behavior). Define a small atomic-write bound separately
from total capacity; larger writes may complete partially. Closing an endpoint or
exiting must wake affected waiters safely.

This needs a reviewed predicate/publication/lifetime design using the scheduler's
existing primitives, not a new generic sleep helper. Never hold a pipe spinlock or
filesystem lock across sleep. Wait predicates under scheduler lock cannot acquire
another subsystem lock. Demonstrate the empty/full check-to-sleep race is closed;
include endpoint lifetime while blocked and eventual interruption support.

For each pipeline create endpoints, prepare all stages, launch every stage before
waiting, close unused parent/child copies promptly, then collect every child. Default
status is the final stage; offer explicit `pipefail`. On partial launch failure,
close endpoints and terminate/reap already started stages via a bounded cancellation
mechanism. This minimal cancellation capability must land here, ahead of full job
control; it must work for blocked and CPU-bound children.

Stateless builtins can run in a spawned `/bin/shell` worker with explicitly copied
state or be external tools. `cd`/`export` inside a pipeline execute in child shell
state and cannot change the parent. Never run a producer builtin synchronously in
the parent before starting its consumer.

Convert `cat` into a byte-preserving stream tool with stdin/multiple-file/`-` support;
move safe escaped viewing into `view` or a pager. The current sanitized `cat` cannot
serve as a faithful pipe stage. Add `printf`, `wc`, `head`, `tail`, `tee`, and a bounded
literal-search `grep` first. Add `sort`/`uniq` only with stated memory/input limits.

**Acceptance:** datasets several times larger than pipe capacity; three or more
stages; slow readers/writers; early reader exit; last-writer EOF; inherited-end leaks;
blocked-child exit; partial launch failure; bounded cancellation and complete reaping.
Compare payload bytes independently of screen output. Exercise supported CPU scope
under 1/4/8-CPU boots, and across cores if that scope has been explicitly enabled.

### S8 — Jobs, signals and controlling terminal

Land in three steps:

1. Add child event/status API (exit, later stop/continue), nonblocking wait and wait-any;
   implement `cmd &`, `jobs`, `wait`, and notifications at prompt boundaries. Background
   stdin defaults to null until terminal ownership is enforced. Reap all children,
   including those completing while the shell waits for another job.
2. Add process groups and controlling-terminal ownership with atomic group membership
   before a new child runs. Route Ctrl+C to the foreground group; the shell must not
   race children for input. Support default INT/TERM/KILL dispositions and a `kill`
   command. Never redirect kernel faults into ordinary signal success paths.
3. Add stop/continue and terminal handoff for Ctrl+Z, `fg` and `bg`. Ctrl+Z requests
   **SIGTSTP**, not SIGSTOP; SIGSTOP is the uncatchable stop operation. Represent
   running/stopped/completed jobs and group-wide state accurately, including pipelines.

Signals are kernel process work: define permission checks suitable for the current
identity model, stable PID/group references, pending events, interrupted blocking
syscalls, remote reschedule and safe-point delivery. Termination must reuse normal
resource teardown; never free another CPU's active stack/CR3. Do not inject handlers
from IRQ context. Full user handlers/sigreturn are a later protected ABI sub-piece;
default actions plus safe shell event consumption suffice initially.

The terminal gains explicit raw/editor versus cooked application modes, echo and
signal-character settings. Foreground changes restore mode and repaint after child
exit/fault/stop. Background terminal reads must not steal prompt input; define the
stop/error policy. Job notices appear between prompts. Background stdout may intermix;
provide deterministic repaint rather than claiming arbitrary programs cannot disturb
the display.

Define exit policy: warn once when jobs are live/stopped; explicit repeated exit or
force exits terminates/reaps owned jobs with a bounded escalation policy. The root
shell supervisor may restart it as today; nested shells return to their caller.
Do not abandon uncollectable children on shell failure or restart.

**Acceptance:** Ctrl+C on CPU-bound/blocked/pipelined work; Ctrl+Z/fg/bg; rapid exit
before wait; repeated stop/continue; foreground handoff races; background reads;
shell fault/restart with jobs; no missed events or leaked stacks/fds/address spaces.
Use BIOS/UEFI and SMP stress plus physical keyboard acceptance.

### S9 — Scripting, in useful increments

**S9a (can follow S5):** `/bin/sh` compatibility entry, `sh file`, `sh -c`,
`source`/`.`, comments, positional arguments `$0`/`$1`/`$#`/`"$@"`, `shift`,
`return`, and noninteractive EOF behavior. Scripts use the same parser/executor,
with filename/line/column diagnostics and bounded input. No prompts/history/aliases
by default in scripts. Script dispatch/shebang support needs bounded interpreter
recursion and argument construction; do not assume the ELF loader executes text.

**S9b:** `if/then/elif/else/fi`, `for/in/do/done`, `while`, `until`, `case`,
`break`/`continue`, functions, `test`/`[`, brace groups and defined variable scoping.
Parse each complete compound unit before running it. Bound parser nesting, function
call depth and sourced-file recursion. Add `read` and reusable script examples.

**S9c (after pipes/cancellation):** `$(...)`, grouping in a separate shell environment,
and here-documents. Drain captured output concurrently with the child so a full pipe
cannot deadlock; remove trailing newlines, reject NUL/overflow explicitly, and preserve
stderr. Quoted substitution yields one argument. Here-documents need bounded spooling
or concurrent delivery, not writing an oversized body to a pipe before the reader
starts. Specify quoted delimiter behavior. No backtick substitution initially.

**S9d (after signal events):** `set -x`, optional `set -u`, carefully specified
`set -e`, and `trap`. Document conditional/pipeline exceptions for `-e` before
implementation. Execute traps only at safe interpreter boundaries, never in IRQ
context. Arithmetic expansion can follow with checked overflow/division rules.

**Acceptance:** a behavior corpus covering control flow, quoted positional args,
nested failures, source/function return, cancellation, substitution overflow/deadlock,
here-doc EOF, traps and all documented option exceptions. Do not advertise unspecified
syntax as supported or silently accept it as another command.

### S10 — Tools and polish

Deliver incrementally rather than making these a final monolithic release:

| Feature | Priority and dependency |
| --- | --- |
| `pushd`, `popd`, `dirs`; `clear`; keybinding help; history prefix search | After S4; bounded directory stack |
| `cp`, `touch`, `rmdir`, richer `ls`, `stat`, `find` | After consistent paths/I/O; bound traversal and preserve actual FS capabilities |
| `less`/`more`-style pager with search; improved standalone `edit` | After terminal modes; prompt on modified-buffer exit; safe incomplete-load handling |
| `ps`, `sysinfo`, `uptime`, `free`, `top`, `time` | Dedicated bounded snapshot/time ABI; no raw kernel pointers or unstable TCB iteration |
| Mount/capacity/durability inspection (`mounts`, `df`) | Read-only status ABI; do not add automatic mounts or write authorization |
| `sleep`, `date` | Monotonic wait and wall-clock services respectively; never busy-wait or invent a clock |
| Config file, key bindings, color controls, startup script | Recovery mode that skips startup; script loading requires explicit opt-in |
| Terminal scrollback, alternate screen, resize events | Separate terminal pieces; retain early-console guarantees |
| UTF-8 input/rendering and width-aware editing | Later coordinated keyboard/font/terminal change; initial release remains explicit ASCII |
| Multiple terminals, PTYs, serial sessions | Later session architecture; no accidental multi-reader stdin semantics |

Startup defaults should come from packaged read-only configuration. A selected data
USB is write authorization, not authorization to auto-execute its startup scripts.
Offer `shell --norc`/recovery entry and explicit `source` first. Later accounts,
permissions and home directories can supply the trust boundary for automatic rc files.

Keep networking tools, package management, persistent rootfs installation, full
permissions, regex engines and a full-screen IDE out of this shell milestone. They
remain compatible follow-on work, not hidden dependencies of command history.

## 5. Verification and release gates

All targets below marked **new** are proposed, not existing passes. Run Linux tools
in WSL `Ubuntu-24.04` at `/mnt/c/Sources/FortressOS`.

| Change | Required evidence |
| --- | --- |
| S0–S2 | Existing `make test-input`, `make test-console`, `make test-shell`, `make test-power`; `test-shell-host` and `test-shell-integration` |
| S3–S5 | Extend `test-shell-host` and `test-shell-integration`; existing shell/storage/ext2 regressions relevant to changed paths |
| Persistent history | New `test-usb-shell-history`; existing `test-usb-mount` and `test-usb-persistence` when storage behavior changes |
| S6 | Extend the consolidated host/integration targets; `test-ext2`, `test-ext2-write`, `test-storage`, USB persistence for append/storage changes |
| S7–S8 | Extend the consolidated host/integration targets; SMP lifecycle/VMM regressions and relevant scheduler tests discovered from Makefile |
| S9 | Extend the consolidated host/integration targets; integration corpus against actual Ring 3 executor |
| Entry/return or address-space changes | `make test-nmi`, `make test-smp-percpu`, `make test-vmm-host`, `make test-smp-vmm` as applicable |

Host tests must compile real module code under ASan/UBSan and exercise malformed
input, not reproduce the implementation in a mock. Terminal tests need screen-state
assertions: a serial substring cannot prove cursor correctness. Stream tests compare
exact bytes without using ANSI-formatted logs as the oracle.

QEMU coverage: BIOS and paired-OVMF UEFI; PS/2, UART and no-UART framebuffer paths;
1/4/8 CPUs for shared-state and lifecycle pieces; narrow/wide viewport where possible.
Use bounded runners and retain exact command/log/firmware/fixture metadata. Track
baseline allocation sets, open objects, child records and deferred reaping after
success, injected failure, cancellation and repeated shell restarts.

USB history/config tests must use disposable copies, no NVMe data fixture, and
validate the final QEMU arguments on every boot under AGENTS §7.5. Existing NVMe
storage tests retain their separate scope. Hardware writes remain restricted to the
explicitly selected authorized USB partition. Never silently repair or format it.

Manual Dell gates after S2, S4 and S8: actual US/AZERTY editing, Ctrl/AltGr operators,
long-line/prompt repaint, history after clean reboot, foreground interrupt/suspend,
and recovery to a usable prompt. Record physical results separately from QEMU.

For each piece: update implementation status and a matching `docs/roadmap/` evidence
entry only after its actual checks pass. Keep this proposal separate from completion
claims. Implementation evidence is recorded separately in `docs/roadmap/shell-s0-s2.md`.

## 6. Principal risks and review boundaries

| Risk | Required mitigation before implementation |
| --- | --- |
| Input/child records still use CPU-local assumptions | Trace callers/affinity; preserve enforced scope or review a complete synchronization change |
| New pipe/terminal locks invert existing ranks | Define lock graph and lifetime/publication protocol; no sleep under ext2 or any spinlock |
| Cursor controls break boot/NMI diagnostics | Separate terminal stream handling from early/raw output; retain no-UART tests |
| A longer line overflows the small user stack or spawn ABI | BSS arenas, stack budget and distinct input/argv limits |
| History silently destroys its only valid copy | Validated snapshot/replacement contract, retained previous copy, bounded load and honest durability claims |
| A malformed command partially executes | Parse complete logical unit first; defer expansion of skipped branches |
| A pipe hangs because the shell kept a writer open | Explicit per-stage fd ownership, EOF/close tests and cancellation cleanup |
| Job interruption frees active CPU resources | Safe-point termination through existing deferred lifetime/reaper rules |
| Useful shell features become an OS rewrite | Ship S0–S4 first; gate later process/terminal work in separate pieces |

Any intentional change to the protected synchronization, syscall transition,
address-space or test-isolation contracts requires discussion before implementation
under [`PROTECTED.md`](../../PROTECTED.md). This proposal identifies those boundaries;
it does not weaken them. Most early shell logic changes can preserve them.

## 7. Coverage of the supplied feature list

| Supplied item | Planned home |
| --- | --- |
| 1 History, persistence, `!!`/`!N` | S2/S4; expansion optional and off by default |
| 2 Cursor, Home/End, Ctrl+A/E | S1/S2 |
| 3 Delete | S1/S2 |
| 4 Ctrl+W/U/K/L | S2; add Ctrl+Y |
| 5 Ctrl+R | S2 |
| 6 Tab completion | S4 |
| 7 Long/multiline commands | S2 bounded editor; S3 continuation; later visual multiline |
| 8 Prompt customization | S4 |
| 9 Redirection | S6 |
| 10 Pipes | S7 |
| 11 Command substitution | S9c; not assumed to be trivial |
| 12 Background jobs | S8 |
| 13 Environment | S5 |
| 14 Aliases | S5 |
| 15 Status, `;`, `!`, `$?` | S3/S5; pipeline status S7 |
| 16 Globs and braces | S5; brace expansion optional |
| 17 Quoting | S3, quote-aware expansion S5 |
| 18 Scripting and functions | S9 |
| 19 Job-control signals | S8; correct Ctrl+Z to SIGTSTP |
| 20 ANSI terminal control | S1 subset, S10 extensions |
| 21 Central builtins, cwd, directory stack, source | S0/S3/S5/S9/S10 |
| 22 `set -e`, `set -x`, traps | S9d with explicit semantics |

## 8. Agreed direction and later review boundaries

1. Approved usability-first sequence: S0–S2, hardware review, then S3–S4.
2. Use bounded 4 KiB editing and a horizontal viewport initially; retain larger
   logical commands for editing even when a particular executable hits argv limits.
3. Keep a small specified shell language rather than promising Bash compatibility.
4. Keep spawn-based execution; add process cwd, uniform descriptors and structured
   child setup before attempting redirection and pipes.
5. Make history persistence optional at `/mnt/.fortress/history`, with explicit
   storage eligibility and no automatic execution of USB startup scripts.
6. Treat job control and scripting as later independently reviewable milestones.

S0–S2 implementation is authorized by the subsequent user feedback. Review the Dell
acceptance results before authorizing the next pieces; S3–S10 remain future work.
