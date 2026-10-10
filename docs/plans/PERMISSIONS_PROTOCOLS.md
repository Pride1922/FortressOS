# Permissions Phase 0 protocol review

Status: Phases 0–4 complete locally, 2026-10-10. Production DAC and login are enabled.
[Phase 2 evidence](../roadmap/permissions-phase2-gates.md) records authoritative
admission, metadata transactions, syscall/tool acceptance and verification limits.
Issue #5. No GitHub state changes; physical evidence is user-supplied.
The user approved global registry consolidation, completion of Phase 0 and
EXT2 lifetime repair. Credential bindings and filesystem metadata now exist;
authorization and metadata/query syscalls are implemented. General credential
transition syscalls, login, set-ID execution and sudo are delivered. Phase 5
automated hardening is complete locally; user Dell functional and Linux audits
PASS with limits retained in the physical checklist closeout.
See [Phase 5 gates](../roadmap/permissions-phase5-gates.md).
Read alongside PERMISSIONS_PLAN.md and the evidence checklist.

## Phase 5 admission and archive hardening (2026-10-10)

SYS_DMESG snapshots its published actor under G, releases G and requires
effective UID zero before zero-length probes, output validation or on-demand
network profile publication. UID-zero callers with dropped capabilities remain
eligible under this explicit root-only policy. Non-root capabilities do not
substitute for effective UID zero. NETCTL_IFSET uses CAP_SYS_ADMIN before input
validation/configuration mutation, within the unchanged BSP/affinity fence.
An ordinary unpinned process still receives EOPNOTSUPP at that fence; no AP
network ownership or socket support is added.

TarFS checks the complete immutable USTAR module before VFS initialization or
node publication: checksum, strict USTAR version, numeric metadata/size, bounded
full paths and components, no parent traversal, supported types, payload bounds
and two zero blocks with an all-zero tail. This prevents malformed later headers
from publishing an earlier set-ID image. Allocation failure during subsequent
boot population remains a fatal initialization error; this adds no transactional
VFS allocator or recovery from a failed boot-module construction.

All syscall cases have an explicit reviewed authority classification, including
public process/geometry/system queries, parent/session controls, self operations,
already-admitted shared descriptors and trusted kernel terminal/CHLD/SIGPIPE
paths. Inventory drift is checked against the ABI; textual classification alone
does not establish arbitrary execution safety. Finite sanitizer and Ring 3
evidence, exact-image hashes and physical limits are in the Phase 5 report.

## Phase 3 credential syscall publication (2026-10-10)

Syscalls 66–70 add setresuid/setresgid/setgroups/capset/capget. Setres calls
use full uint32 IDs and explicit keep flags; setgroups copies a validated
bounded array before registry entry. Each writer snapshots old credentials,
derives a complete canonical proposal through the actual value helpers, then
uses expected-old publication under G. The whole-value comparison makes an
outside-G authorization decision conditional on the exact old value still
being current at commit; stale state returns EAGAIN without effects. No user
copy, VFS call, scheduling or target pointer escape occurs under G. Unknown
bits, overflow, excess groups and denied transitions leave credentials intact.
Dropping all zero UIDs clears capabilities; capset never grants them.

The boot launcher starts privileged login, which reads immutable root-owned
TarFS databases, authenticates, validates/creates the runtime home and drops
groups/GIDs/UIDs before spawning its shell. Login and shell share their
foreground group; the waiting login ignores INT/TSTP after authentication.
Shell entry consumes the existing loader envp register without changing the
entry assembly/stack/interrupt-frame contracts. Static user parser scratch
is restricted to the present single-user-thread runtime. Set-ID execution
and shared user-thread credentials remain separate prerequisites/work.
[Phase 3 gates](../roadmap/permissions-phase3-gates.md) records finite host
and BIOS/UEFI acceptance; no physical, cross-core socket or calendar-aging
claim follows.

## Current field and operation ownership (after approved consolidation)

`G` is the ordinary rank-1 global process lock. Every registry lookup and
record transition shares G; no PID shard remains in this module.

| Field / operation | Authority and lifetime |
| --- | --- |
| Record used/PID/parent/session/publication/exit/collection/teardown | G: reservation, lookup, commit, abort, exit, wait, forget and reuse. Lookup returns values only. Monotonic PIDs distinguish reused slots. |
| Name, ticks, stopped state, child status/events | G: construction names, merges, final accounting, STOP/CONT and enumeration. Final ticks cannot be overwritten by a late merge. Enumeration across separate calls remains best effort. |
| Group identity/generation/membership | G: join/leave, selection and signal publication. Atomic reference cloning requires an already-owned reference; it does not pin a TCB. |
| Signal binding/actions/frames and compound decisions | G: attachment, inheritance, single/group/self operations, CHLD inspection and detachment. Atomic pending/mask mailbox access by scheduler-owned TCBs remains separate. Wake captures PIDs and runs after G release. |
| Credential binding and complete value | G: bind once before publication; snapshot/publish only published live records; abort/exit/forget detach before TCB free. Validation and expected-old comparison precede complete replacement. No credential or TCB pointer escapes. |
| Private TCB construction / execution / destruction | Scheduler and owner continuation: initialize private credentials before binding; attach/commit before separate enqueue. Staged cancellation aborts before free; staged release commits before enqueue. Exit detaches bindings before reaper destruction. G never spans scheduler acquisition, filesystem I/O or a context switch. |

Self-signal no longer relies on an unlocked registry shortcut. Current execution
keeps its own TCB alive, but does not protect registry scans, group membership
or target credentials; self and group operations now use G like other callers.
The generic PROCESS-kind lock-checker exception is unchanged; registry G uses
the ordinary kind and cannot nest with any other rank-1 lock.

Current trusted APIs are `process_record_bind_creds`, `process_record_creds`
and `process_record_publish_creds`. Publication validates canonical bounded
values and compares all bytes to the expected snapshot under G; stale, invalid,
unpublished or exited targets fail without changing credentials. This is a
kernel publication primitive, not user authorization. User transition syscalls
must later derive a permitted next value and handle stale publication explicitly.
Private storage must remain exclusively owned until binding, then all access
must follow the registry protocol.
Signal authorization inspects the target's bound value inside its
existing G critical section; it cannot recursively call the public snapshot API.
Public snapshots exclude unpublished records, while existing signal matching
includes attached staged children. Preserve that eligibility explicitly: staged
credentials are initialized before signal attachment and remain valid until
abort/exit detachment. Spawn captures inheritance during construction; staged
release does not recapture credentials or propagate later parent changes. Kernel threads initialize root values;
ordinary user spawn copies one published caller snapshot, including dropped
capabilities. UID zero does not replenish them. Set-ID construction now follows the Phase 4 protocol below.

## Filesystem foundation and authorization boundary

| Filesystem | Metadata and node lifetime authority |
| --- | --- |
| Journaled EXT4 | e4_lock owns references, namespace, authoritative inode metadata and complete create attributes in the existing JBD2 transaction. Owned references protect addresses; metadata rereads under filesystem exclusion. |
| EXT2 | ext2_lock owns lookup/create pins, open counts, namespace, retirement and metadata reads. Removed nodes reject callbacks. Active-open unlink/replacement returns EOPNOTSUPP before writes because disk reclamation is immediate. Legacy raw pointers retain bounded boot-lifetime tombstones; 1024-node bound includes them. No journal/crash-atomicity claim. |
| runfs | Ordinary rank-1 run_lock owns the 64-node pool, namespace, reference retirement, metadata and 4096-byte file contents. Three reserved nodes are /run, /run/user and /tmp. Owned open files retain bytes across unlink. Legacy tombstones consume capacity. |
| TarFS/devfs | Metadata and nodes are immutable after boot publication. TarFS strictly parses USTAR modes/IDs; archive generation normalizes declared boot-image modes and root IDs. Device nodes are boot-lifetime and raw partition access is read-only, USB-only; internal NVMe remains omitted. |

Current `vfs_metadata` returns coherent values through filesystem callbacks;
`vfs_create_attrs_ref` supplies already-derived mode/UID/GID to authoritative
creation. User creation derives actor ownership, umask and parent-setgid under
the filesystem lock before mutation allocation. Actor lookup/open/namespace/
metadata callbacks perform fresh authoritative decisions under the same lock.
Generic VFS paths are not a globally serialized namespace, and owned references
alone do not prove authorization or complete path identity stability.
The write-offset callback contract covers write publication only. Generic
`vfs_read` still captures/advances `file_t.offset` outside filesystem exclusion;
shared read/write offsets and generic file_t reference accounting are not
certified by the append tests. A focused future barrier test can pause a read
after offset capture, run a shared-file write, then compare bytes/offset against
both serial orders. This is an existing VFS synchronization boundary, not repaired
by credential or node lifetime changes. EXT2 owner decoding is tested on Linux
osd2 images; foreign creator layouts are now rejected before allocation, writes
or publication in both mount modes (EXT4 already requires Linux creator OS).

The proposed authorization protocol below remains the Phase 1/2 contract:
copy actor credentials under G, release G, resolve owned filesystem references,
then revalidate identities and authorization under the authoritative filesystem
mutation lock immediately before any write/allocation. Never nest G with a
filesystem lock. Future explicit trusted `_kernel` entry points and permissive
actor plumbing are not yet current APIs. Cross-filesystem rename, denied-mutation
proofs, set-ID bit-clearing rules and mutable mount policy need their listed gates.

Current ABI: legacy SYS_STAT retains its 16-byte buffer and now reports full
mode. SYS_STAT_EXT (57) requires exact size 40 and version 1 and returns size,
type, full mode, full-width UID/GID, mount flags and zero reserved fields.
Invalid size/version is rejected before pointers; output is validated before
lookup and copied after releasing the owned reference. Existing syscall error
numbers remain; EACCES adds -32 and VFS ENOTDIR maps to existing syscall -8.
This compatible extension does not imply chmod/chown or credential syscalls.

## Source audit

The initial audit and G/S ownership table below describe the pre-consolidation
checkpoint. On 2026-10-10 the user explicitly approved global-lock
consolidation, which is now implemented: every registry lookup and record
field operation uses G, including self signals, names, ticks, signal actions
and frames. Abort clears the signal binding. STOP consumption and child-event
publication stay in one G section; wake uses PID values after unlock.
Scheduler-owned TCB lifetime and atomic scheduler mailbox reads remain separate.
The original audit below is retained as historical evidence. The current
ownership map follows; enforcement remains a later gate.

- `thread.h`: one user TCB owns descriptors/cwd today; no credentials exist.
  The scheduler owns placement, context handoff and TCB destruction.
- `process_table.h`: public metadata APIs return values, not TCB pointers;
  its header describes a private global rank-1 process lock.
- `process_table.c`: implementation also has PID-shard rank-1 locks. Name,
  tick and self-signal paths use shards, whereas reservations, enumeration,
  commit/forget and group signal paths use the global lock. Self-signal paths
  perform some lookup before shard acquisition. This is a contract discrepancy,
  not proof here of a new race. Existing lifetime assumptions must be traced
  before credentials depend on them. A shard is not a TCB lifetime pin.
- `thread.c`: private child construction precedes signal attachment and record
  commit; scheduler enqueue follows under a separate lock. Staged children
  remain unpublished until release. Reaping forgets metadata before freeing TCB.
- `vfs.h`: owned references protect node addresses, not coherent mode/owner
  fields or namespace identity. Current callbacks do not accept credentials.
- Journaled EXT4 callbacks use filesystem exclusion (`e4_lock`, rank 1).
  Its current reference/namespace machinery is reusable, not permission-aware.
- `SYS_STAT` has an existing fixed layout; SYS_MEMINFO is 56. No permission
  syscall numbers or stat ABI extension are allocated by this design.

## Credential authority and publication

### Registry ownership gate audit (2026-10-10)

This is source-traced behavior, not runtime acceptance. `G` below means the
ordinary rank-1 `g_process_lock`; `S(pid)` means the independent rank-1 PID
shard. Neither lock excludes holders of the other. `find()` scans all 64
records using plain `used`/`pid` reads; it has no pin or atomic lookup protocol.

| Field / storage | Current writers and exclusion | Readers / qualification |
| --- | --- | --- |
| Record `used`, `pid`, `parent`, `sid` | Whole-record reservation in `begin`: G; abort/collection/forget retire `used`: G | G lookups/enumeration; every shard lookup also scans these fields. Monotonic PID allocation prevents numeric reuse in normal operation, not slot reuse or concurrent scan access. |
| `published`, `exited`, `uncollected`, `teardown_complete` | commit, exit, wait/parent discard, forget: G | G enumeration; name/tick/signal shard paths also read publication/exit state. No common lock for those reads. |
| `pgid`, `group`, group `members` | begin/join, setpgid, exit/abort/forget/leave: G | G group operations; selector-0 self fast path reads these before any lock. Membership is not immutable while a leader runs. |
| `stopped` | take-action drops S, calls `stopped_locked` under G; resume under G | G snapshots/orphan logic; self `signal_publish` reads it under S. |
| `name[16]` | reservation zeros under G; set-name writes under S before publication | G snapshots. Construction-before-commit explains intended name visibility; it does not synchronize unrelated slots scanned by `find`. |
| `cpu_ticks` | reservation zeros and exit finalizes under G; live merge advances under S | G snapshots and exit read the same scalar. A sampled PID is a value, not a lifetime pin. |
| `signals` binding | attach under G after private TCB construction; exit/forget clear under G; abort makes record unused | G publishers/inheritance and S self APIs dereference it. Abort leaves the pointer in an unused slot until overwrite; it is logically unreachable through G lookup, not physically cleared. |
| Child record: identity, parent/group, status/event, sequences, used/done | begin, setpgid, stop/resume/exit, wait/abort/parent discard: G | wait/child publication: G. Global event `sequence` is atomic release/acquire; it is a wake predicate, not a registry lifetime pin. |
| Group identity `pgid/sid/generation`, `members` | G; generation retained across slot reuse | G, plus selector-0 unlocked membership read. Owned external refs prevent group slot reuse, not TCB lifetime. |
| Group `refs` | atomic CAS retain/acquire; atomic decrement under G | atomic; lock-free retain requires an already-owned reference valid throughout. |
| Signal pending/blocked/ignored masks, continue request | atomic mailbox updates; private attach initializes; S action/mask/take and G single/group/orphan/CHLD/resume paths | scheduler readiness uses acquire loads on scheduler-owned TCBs. Some registry reads are plain loads; independent G/S exclusion does not make these coherent compound snapshots. |
| Signal actions and active-frame stack | private attach under G; action/frame operations under S | take/handler under S; attach inheritance and CHLD handler inspection under G. Frames have current-continuation ownership; action inheritance/CHLD do not share S with action updates. |

Operation and lifetime trace:

- Reservation (`process_table.c:286-318`) allocates record, optional child
  status and group membership under G. It does not allocate or pin a TCB.
- `thread.c:1749-1758` names and attaches signals only after all fallible
  construction, including the VMM scheduler reference, succeeds. Commit under
  G precedes enqueue under a separate scheduler lock. Construction failure
  frees the private TCB before outer abort, but no signal binding exists yet.
- Staged children are BSP-owned with local IRQ exclusion, attached but not
  published. `thread.c:2085-2134` cancellation aborts before freeing; release
  commits before enqueue. Signal matching currently does **not** require
  `published`: attached staged children can receive pending signals.
- Owner exit (`thread.c:2271-2310`) excludes local IRQs, cancels staged children,
  samples final ticks under the scheduler lock, releases it, then exits under
  G. Exit clears signals and leaves group membership. Child status may outlive
  the TCB; wait/parent discard clears `uncollected`, and slot retirement waits
  for forget when needed. Reaper detaches dead tasks under scheduler exclusion,
  drops it, destroys address space/stack, calls forget, then frees TCB
  (`thread.c:424-476`). Preserve context handoff and deferred VMM reclamation.
- G-only cross-process signal/group/CHLD/orphan readers cannot race a G detach
  while dereferencing a bound signal pointer. Wake uses only captured PIDs
  after unlock; group refs retain group identity, never a TCB.
- Current syscall/delivery callers use their own `thread_current()->tid`
  (`syscall.c:1410-1432`, `thread.c:2499,2655,2677`). SIGPIPE callers and terminal
  background-access signaling also use the running task (`syscall.c:197`,
  `net_tcp_syscall.c:157`, `input.c:201`). A running continuation cannot be
  concurrently reaped: remote signals publish requests; owner exit performs
  teardown. This supports the self TCB lifetime, including `take_action`'s
  local pointer across S -> unlock -> G -> unlock -> S. It does not protect
  other slots scanned by `find`, group membership, or arbitrary PID callers
  of the public self APIs. No general shard-backed pointer lifetime guarantee
  is established.
- Enumeration returns values under G and is best-effort across calls. Tick
  refresh copies values under each scheduler lock and merges after releasing
  it (`thread.c:1092-1118`); no TCB escapes, but merge and finalization lack
  shared exclusion.

**Concrete existing defect, initially source-level interleaving:**
`process_table.c:80-87` can evaluate the live/tick predicate with cached ticks
10 and sample 20, then pause before storing. `exit_accounted` at lines 340-348
can finalize ticks to 30 and mark exited under G. Resuming the shard writer
stores 20 into the exited record. Refreshes for the same PID serialize with
each other under S; the conflict here is with G finalization. This contradicts the
header's finalized-accounting promise; no TCB UAF is needed for the example.
Use an actual-code host barrier immediately between merge predicate and store,
run exit on another pthread, then snapshot the retained zombie and require 30.
A second barrier test should retire/reuse the record before the delayed store
and require the new record untouched. These require narrowly gated test hooks;
ordinary stress or ASan/UBSan alone cannot prove absence of a data race.
Discuss the synchronization-contract correction before implementing it.
Subsequent actual-code host audit reproduced both finalized-tick overwrite
and stale-slot overwrite on 2026-10-10. Exact commands, boundaries and the
proposed correction are in
[the Phase 0 checkpoint](../roadmap/permissions-phase0-values.md).

Other unresolved hazards are mixed-lock registry scans, action/CHLD reads and
the unlocked singleton-group shortcut. A singleton observation can cease to
be true as another CPU joins the group; establish a reachable controlled
join/send ordering before claiming a reproduced signal omission. Current-task
lifetime alone is insufficient to discharge these gates.

There is an additional contract discrepancy: `spinlock.c:56-60` permits ordered
PROCESS-kind shard pairs, whereas PROTECTED/AGENTS allow only scheduler pairs.
This registry currently acquires one shard at a time; the exception does not
provide G/S exclusion. Do not use or extend it for credentials.

### Historical revised candidate and first implementation decision

The G-only credential proposal below is **conditional**, not validated by the
signal pointer precedent. G can protect a new binding if **all** credential
attach/read/write/detach paths use G, no shard touches credentials, and detach
precedes every bound TCB destruction. Exit must detach credentials, not merely
forget; zombies have no credential pointer. Abort must explicitly clear a new
binding, including staged cancellation. Private initialization is allowed only
before binding; after binding even the owner publishes through G. Failed
construction must never free an attached credential value before detach.

No existing signal fast path may gain a credential read under S. Future user
signal authorization must use G for target credential lookup plus pending
publication, including self/selector-0, and wake after unlock. Keep trusted
kernel group publication separate. This requires accepted signal-contract work
before enforcement; it is not a Phase 0 silent fast-path repair. Define whether
attached unpublished targets are eligible rather than accidentally changing
their current signal behavior. Copy actor credentials under G, release G, then
enter filesystem exclusion; user input/output, scheduler and descriptor work
remain outside G. No G/S or process/filesystem nesting is proposed.

Recommended first small Phase 0 implementation after review: pure bounded
credential value foundation (`creds.h`, layout assertions, initialization,
validation and complete-value transition helpers) with actual-code host tests.
No TCB field/binding, syscall allocation, or enforcement in that first change.
Before the subsequent binding change, explicitly choose a registry policy:
prefer one G ownership domain for registry fields as the simplest correctness
baseline, or specify atomic lookup/pinning and field ownership for retained
shards. Consolidation changes synchronization behavior and needs discussion;
performance work remains paused. Do not infer acceptance from this recommendation.

Focused outstanding gates: positive merge/exit and slot-reuse regressions;
concurrent action update versus CHLD/attach; controlled singleton-group join;
current-only API preconditions; bind/abort/exit/forget ordering and staged
eligibility. Then disposable BIOS/UEFI SMP fixtures must exercise actual owner
exit, context handoff, signaling and cleanup. The initial source audit ran no
tests; the later host reproduction is recorded separately above. Host mutex
adapters cannot certify IRQ/scheduler correctness.

Retain the planned bounded `creds_t` in the TCB, but prohibit direct published
field access. Candidate: process-table APIs mediate every published snapshot
and update under the same global process lock. Bind a credential pointer to
the record only while TCB lifetime is protected, following the existing signal
pointer discipline; never return this pointer. No duplicate mutable mirror.
This is a proposed contract extension, not an existing API or a lock policy change.

1. Construction exclusively initializes the full child credential value before
   binding/publication. Kernel threads use explicit trusted initialization;
   normal user spawn takes one parent snapshot, including groups/umask/caps.
   Root effective UID alone never replenishes capabilities.
2. Snapshot lookup, record-state validation and bounded value copy share one
   critical section. Self reads use the same protocol as cross-process reads.
   Return failure for missing/unpublished/dead targets according to the API;
   process enumeration may retain value-only final zombie identity if specified.
3. Writers copy user input before locking, then validate transitions against the
   current old credentials under the lock and publish the entire valid value.
   Unknown caps, excess groups and invalid reserved fields fail without changes.
   Clear unused group entries/padding; never publish partially changed identity.
4. A syscall uses one actor snapshot for its operation. Release the process lock
   before VFS, network, scheduler, user copies, descriptor work or blocking.
   Blocking does not refresh authority midway through the same operation.
5. Cross-process signal authorization and pending publication occur together
   under the process lock against each target's current credentials. Wake by
   PID only after unlock. Do not authorize a target, unlock, then signal a new
   identity found later. Kernel terminal/orphan signals use explicit trusted
   entry points, not fabricated root credentials or user authorization bypass.
6. For group signals, bound traversal by PROCESS_CAPACITY and check each live
   target. Proposed result: success if at least one target is authorized (also
   for signal 0); EPERM if eligible targets exist but none is authorized, ESRCH
   if none exist. Retained group generations preserve existing identity rules.
7. Abort/forget detaches credential binding under the same lock before TCB
   destruction; stale PIDs never resurrect it. No pointer survives unlock.
8. Staged credentials are final before record commit and runnable publication.
   Cancellation detaches them. Set-ID changes are deferred to Phase 4 and must
   complete before child publication, using the admitted executable identity.

Original gate: audit every global/shard record lifecycle reader/writer, including
reservation reuse, attach, abort, forget, self/group signals and staged release.
Establish that the global lock really protects lookup plus bound-pointer
lifetime. Shards must never independently modify credentials or bindings.
If the current registry cannot provide this guarantee, stop for a separate
protected-contract discussion; do not nest process/shard/scheduler locks or
introduce an unsynchronized credential mirror as a workaround.

Post-consolidation gate: G now excludes attach/abort/exit/forget, lookup and
bounded value operations; no shards remain in the registry. This provides
the proposed binding's required exclusion domain, but its new detach paths,
private-construction ordering and actual credential publication still require
their own review/implementation/tests. No credential pointer exists today.

Shared-address-space user threads (#12) remain out of scope. Their prerequisite
is moving credential authority to a shared process object, not copying TCB creds.

## VFS authorization and mutation

Pass a const actor snapshot through explicit user-facing APIs. Trusted kernel
entry points are distinct. `vfs_permission` becomes a pure decision over a
coherent metadata value; it must not perform I/O, allocation or locking itself.
Current `can_write` callbacks are not assumed safe to invoke recursively under
filesystem exclusion: provide existing-lock helpers at the filesystem boundary.

- Walk: hold the parent reference until acquiring the child reference. Under
  filesystem exclusion, check parent search permission and resolve/pin the
  named child. Released exclusion does not mean the full multi-component path
  is an atomic snapshot. Later mutation revalidates the relevant name/inode.
- Open: under authoritative filesystem exclusion, verify identity/type, mount
  policy and access, acquire the open/reference, and perform any authorized
  truncate. Missing-name creation checks parent search/write and derives mode,
  uid/gid in the same transaction. Do not treat EIO/ENOMEM as ENOENT.
- Create/unlink/rename: filesystem callbacks check current parents, victims,
  sticky bits and destination identity inside the same exclusion used to plan
  and commit mutation. Both rename parents share the existing filesystem lock;
  cross-filesystem rename remains unsupported. Revalidate before staging.
- chmod/chown/fchmod: authorize authoritative inode metadata, then stage owner,
  mode and required set-ID clearing in one journal transaction. File handles
  pin identity; raw cached node fields are not authorization authority.
- Existing descriptors: access admitted at open follows the handle, not fresh
  path DAC on every read/write. Continue checking requested access mode and
  dynamic EROFS/EIO/taint. Mode changes do not revoke existing open rights.
  Specify write/truncate set-ID clearing separately before Phase 2.
- stat/readdir/chdir: coherent metadata snapshots/search checks occur under
  filesystem exclusion; copy values to user memory only after unlocking.
- Executable loading: pin/open the executable and authorize that identity,
  never relookup its pathname for set-ID metadata. Phase 4 additionally needs
  a validated policy for concurrent executable writes and metadata changes;
  a lifetime pin alone does not freeze executable bytes or permission fields.

EXT2, TarFS and memory-backed namespaces need explicit authorization adapters.
Their current lifetime/metadata foundation does not provide authorization.
Runfs exclusion and boot-immutable devfs now exist; no new all-VFS lock is assumed.
Denied operations must precede disk staging, allocation and namespace mutation.
Normal EXT4 uses JBD2; ext2 retains its separate non-journaled guarantee.

Lock sequence: take/release process exclusion for the actor value, then take
filesystem rank-1 exclusion. Never hold both. Existing filesystem -> heap(2)
-> VMM(3) -> PMM(4) calls retain their contracts. No rank-1 nesting, permission
helper recursion, process lock over I/O, or lock across scheduling/user copy.

## Acceptance and handoff

### Initial value-only checkpoint (historical, 2026-10-10)

`src/include/creds.h` and `src/kernel/creds.c` now provide the first isolated
Phase 0 step: a 104-byte bounded value, layout assertions, explicit root
initialization, canonical validation, ordinary inheritance and complete-value
setresuid/setresgid/setgroups/capset/umask helpers. This is an internal pure
value API, not a syscall ABI or a credential publication API. Result constants
are positive internal errors; a future adapter must map them explicitly.

ID changes use explicit keep flags, preserving the full uint32_t ID range.
Validation rejects nonzero reserved/unused group storage, excess groups,
unknown capabilities and noncanonical umask. Group replacement clears unused
entries; duplicates remain permitted. capset intersects the requested defined
bits with current capabilities, as specified by this plan. UID transitions
clear all effective capabilities when no UID remains zero; they do not grant
capabilities when a UID becomes zero. These are FortressOS candidate policy
rules, not a claim of complete Linux credential semantics. Invalid input or
denied transitions leave output untouched; in-place transitions are supported.

Executed in WSL Ubuntu-24.04: `make test-perm-creds-host` PASS under ASan/UBSan
(70,496 matrix cases plus focused boundary/rollback checks), and
`make build/kernel/creds.o` PASS with production freestanding flags.
No process bindings, locks, syscall allocations, enforcement or image changes
were made. No QEMU/IRQ/scheduler/lifetime acceptance is implied. The registry
ownership decision and existing defect reproduction remain outstanding before
the binding change.

The following was the initial gate list. Current execution is recorded in
the ownership map and roadmap; signal authorization and denied mutation remain
future work:

- Actual-code host ASan/UBSan: alternate two distinct credential values under
  concurrent snapshots; reject mixed groups/IDs/caps, failed partial publication,
  overflow and stale PID access through exit/forget/abort. Test lifecycle reuse.
- Spawn/staging: ordinary root spawn cannot restore dropped caps; child receives
  one consistent parent value; cancelled children leave no binding or resources.
- Signals: target changes credentials/exits concurrently with single/group
  sends; authorize and publish atomically; verify signal-0/error precedence,
  partial group permission and trusted kernel signal behavior.
- Filesystem barriers: race lookup/check with rename/unlink/chmod/chown; ensure
  mutation uses authoritative identity. Denials produce zero metadata/allocation
  events. Journal cuts recover old-or-new ownership/mode, never mixed values.
- ABI (#7): preserve existing callers; define a separate size/version-checked
  stat interface before Phase 0 metadata reporting. Test short/invalid buffers,
  reserved fields and current syscall-number allocation. No silent struct growth.
- QEMU: disposable BIOS/UEFI fixtures; actual Ring 3 checks and cross-core
  publication/lifetime tests with explicit gates, recovery and exact cleanup.
  Host adapters do not certify IRQ, scheduler or hardware correctness.

Order: resolve registry ownership gate; approve protocol; implement credential
foundation without enforcement; implement metadata/compatible ABI; then bounded
devfs/runfs in separate verified changes. Phase 1 wiring and Phase 2 enforcement
follow their own gates. No login/sudo, allocator tuning, cross-core sockets or
Dell testing belongs in Phase 0.

### Current focused gates and first Phase 1 step

The registry gate is resolved by global exclusion plus explicit binding detach,
not by an assumption that the old global header covered shard operations.
Host publication/lifecycle tests and gated AP snapshot/owner-exit execution pass.
The complete inode mode/owner matrix and 1000 new attribute-creation atomic
cuts pass; each recovered inode is absent or has the complete intended values,
never mixed ownership/mode. Representative old/new images pass Linux fsck/stat.
The full existing mounted regression and 66-case guest crash campaign pass;
independent campaign review reports 66 cases and zero errors. The campaign used
an isolated source snapshot before the final devfs raw-read adapter correction;
its filesystem mutation/recovery paths are unchanged by that adapter. See
permissions-phase0-values.md for exact runs, the approved GPT scratch repair
and the passing final BIOS/UEFI × SMP=1/4 raw USB peer gate. Phase 0 is complete
locally; actor-aware wiring and enforcement remain separate open phases.

Devfs metadata and nodes are immutable after boot publication. Its bounded
64 KiB raw reads join the boot-selected /mnt filesystem exclusion per sector:
EXT4 when mounted, otherwise EXT2. An EXT4 I/O failure is never retried through
EXT2. This preserves existing shared USB BOT serialization without nesting
rank-1 locks or changing the transport. Mount selection is boot-only; hot mount
changes would require a separate ownership protocol. Actual-filesystem host
tests check exclusion and failure routing; they do not prove IRQ correctness.

The first Phase 1 step, now delivered and expanded below, was to introduce one
explicit actor-aware lookup/open path with a permissive authorization stub and
an explicit trusted kernel counterpart. Capture one real current credential
value before VFS entry, then pass it by value/reference to owned local storage;
keep G released throughout VFS work. Audit all callers of that pair before
expanding to create/rename/unlink. Do not add a fake-root bypass or enforce DAC
in this first wiring change. Filesystem mutation authorization remains a separate
adapter gate under filesystem exclusion, with denied-mutation byte/allocation
checks before Phase 2. Shared user threads must move credential authority first.

## Historical Phase 1 actor wiring (2026-10-10)

The current APIs are `vfs_lookup_creds`, `vfs_open_creds`,
`vfs_open_mode_creds`, `vfs_open_exec_creds`, and actor-aware mkdir, unlink,
rename and readdir. Public actor entries reject a missing actor. Permission
and deletion decisions are deliberately permissive; this is not DAC support.
A syscall captures the current published credential value under G, releases G,
then borrows its local value throughout VFS work. Owned node references retain
identities across walk/open; permission hooks do not read mutable metadata.

Canonical parent walks request EXEC; open requests READ/WRITE as appropriate,
truncate adds WRITE, executable loading requests EXEC, readdir requests READ,
and namespace changes request parent WRITE|EXEC. Creation callbacks carry the
actual euid/egid and requested mode into the existing authoritative filesystem
mutation. Default OPEN mode is 0644; spawn actions retain their explicit mode.
Umask, setgid inheritance and stripping remain Phase 2 decisions.

Spawn captures one actor for executable admission, explicit cwd search,
FD actions and child inheritance. SYS_SPAWN_EXT resolves an explicit cwd using
current CHDIR path normalization; the loader validates directory type and
requests EXEC before constructing the child. Missing/non-directory explicit
cwd now fails before FD actions. Executable metadata must identify a regular
file; a raw block device is not an executable. No set-ID transition is enabled.

`process_signal_send_creds` decides against the current actor and each bound
eligible target under G immediately before pending publication. Signal 0,
group and staged-target paths share that exclusion. Values never escape G;
wakes occur after unlock. Trusted terminal/job-control/CHLD/SIGPIPE publication
retains its explicit kernel entry. CAP_SYS_BOOT and CAP_NET_BIND hooks borrow
actual caller snapshots before power/flush or network-lock entry. Low ports
are interpreted in host byte order; port zero and ports >=1024 need no hook.
Both capability decisions remain permissive, including zero-capability actors.

Production boot, mount, kernel tests, initial terminal descriptors and private
kernel process construction use explicit `_kernel` path APIs. Unsuffixed APIs
remain trusted compatibility interfaces for existing fixtures; they are not
actor-aware APIs. Handle read/write/dup/close operate on previously admitted
handles, not a new pathname lookup. Fixed /mnt metrics lookup is trusted.
The finite source inventory and indirect callback/descriptor classification
are recorded in permissions-phase1-wiring.md. A textual inventory is not proof
that every possible execution passed through a trace hook.

### Historical gates before Phase 2 enforcement (resolved below)

- Implement filesystem-owned value decisions and identity revalidation under
  each authoritative mutation exclusion. Replacing the permissive VFS stub
  alone is unsafe: it is outside that exclusion and must not inspect mutable
  inode fields or borrow unprotected parent pointers.
- Resolve lexical `.`/`..` normalization: current syscalls check the canonical
  walk, so they can omit search checks on components removed before VFS entry.
  Define and test full traversal semantics before enabling DAC.
- Finish no-op/error precedence under filesystem exclusion. Phase 2 now
  resolves same-path rename and validates trailing slashes before success;
  authoritative locked identity/sticky/read-only decisions remain required.
- Define umask/setgid and descriptor-right admission, denied-operation byte,
  allocation, journal and namespace oracles with issue #6. Coordinate new
  metadata interfaces with #7; existing stat ABIs remain unchanged here.
- Shared user threads require a shared credential owner before execution.
  No cross-core sockets, login, sudo or physical acceptance is claimed.

Phase 2 now also routes SYS_READDIR through the admitted descriptor's READ
right, retaining filesystem errors and success-only offset publication; it
does not repeat pathname DAC after open. O_RDONLY|O_TRUNC is rejected before
effects. Pure DAC/create/metadata proposal rules have independent host-oracle
coverage, but are not connected to permissive production admission. Full
original dot-component traversal and filesystem-owned transaction integration
remain blockers; see the Phase 2 checkpoint.

Host adapters validate actual call wiring and value identity; they do not
certify IRQ, scheduling or real power transitions. Isolated trace kernels log
only access mask and actor values, not unsynchronized mutable node metadata.
Normal kernels contain no trace output. Raw UART logs remain available even
when application-output matching removes trace records in the test harness.

## Phase 2 authoritative admission (2026-10-10)

The historical gates above are resolved. Production CFLAGS enable DAC;
filesystem-owned actor callbacks decide and mutate under ext2_lock, e4_lock
or run_lock. Process snapshots release G before filesystem entry; signal
authorization instead stays inside the existing G critical section. Original
path components are retained through admission, and only successful CHDIR/
spawn cwd walks are canonicalized. Existing admitted descriptor rights persist.
Journaled EXT4 setters and content set-ID stripping share the corresponding
inode transaction; EXT2 retains its existing nonjournaled failure/taint limits.

Metadata/query ABI numbers 58–64 and numeric tools are implemented. Test-only
syscall 65 permits a fixed self-drop to UID/GID 1001 with zero capabilities;
it is absent from the production kernel. General credential transitions,
shared user-thread credential ownership, login and set-ID exec remain later
work. The final [Phase 2 evidence](../roadmap/permissions-phase2-gates.md)
records finite denial/interleaving coverage, 4,088 crash cuts, independent
Linux audits and BIOS/UEFI × SMP=1/4 Ring 3 acceptance. No physical or GitHub
changes are included.

## Phase 4 admitted-image and secure construction (2026-10-10)

Set-ID decisions use the retained executable identity, never a fresh pathname.
TarFS metadata and bytes are immutable boot-module values. Runfs reauthorizes
EXEC and copies complete bytes/metadata under run_lock; allocation follows the
existing rank-1 -> heap rank-2 order. Copy ownership remains with the spawn
continuation until loader consumption/cleanup. Unsupported mutable suid-capable
adapters fail closed. Production EXT2/EXT4 always return nosuid/nodev; ordinary
mutable disk executable reads do not gain a freeze/atomic-byte claim.

The actor value authorizes explicit cwd and OPEN actions. A separate private
child value receives admitted setuid/setgid changes before registry binding,
staging or publication. Only a changed effective UID equal to zero replenishes
capabilities. An already-root caller's dropped mask remains dropped. No process
lock spans filesystem entry, user copy or scheduling. Staged release never
recomputes credentials; failure uses the existing private-resource unwind.

Secure descriptor admission tracks final explicitly mapped destinations after
ordered actions. Above-2 inherited descriptors without that admission close,
then CLOEXEC closes normally. Parent descriptors and caller OPEN authority are
preserved. Vector stacks carry AT_SECURE=23 before AT_NULL, within the original
alignment/floor budget; shell/builtin entry ignores inherited environment when
secure. Sudo resolves real-user wheel membership in the immutable database,
authenticates via controlling tty, then installs root credentials and a minimal
target environment. The approved empty operator hash prints an explicit warning;
locked root login and temporary /run/user/1000 remain unchanged.

[Phase 4 gates](../roadmap/permissions-phase4-gates.md) records host and 10/10
BIOS/UEFI acceptance plus the 9/9 login regression. No physical, shared-thread,
general concurrent disk executable-write or cross-core socket claim follows.
