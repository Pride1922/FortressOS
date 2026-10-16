# Network SMP Phase 2D — TCP listeners and lifecycle

2026-10-16. LISTEN/ACCEPT implementation and verification are described below.
Production spawn pinning remains unchanged; Phase 3 and physical acceptance
are separate work. Nothing was committed or pushed. HEAD remains the user's
`a096e49`, preserving the intervening command and Time commits after `379a977`.
DHCP and Time implementation sources were not edited.

## Actual-path audit

Numbered socket calls enter `syscall_dispatch -> net_socket_syscall ->
net_tcp_syscall`. Socket READ/WRITE enter the stream handler from VFS dispatch.
The borrowed own-table lookup remains stable until a signal/wait; the typed
admission acquires a checked file reference before publication. Cloned and dup
descriptors own independent references to the shared file, not shared fd tables.

The former ACCEPT continuation depended on BSP IF-clear exclusion from child
peek through constructor allocation, fd insertion, irreversible queue removal,
finish-adopt and user output. Both dispatch layers rejected AP/affinity callers.
The staged close callback directly unstaged TCP, while adopted close published
TCP detach after prematurely exposing the common slot for reuse. Those are
replaced with the transactions and common-slot fence below.

The protocol audit follows `net_ipv4_input -> net_tcp_input` for listener tuple
matching and passive allocation, `net_tcp_tick -> tcp_conn_tick/tcp_pool_tick`
for half-open expiry, reset/orphan cleanup and TX prepare/commit, and the
listener's bounded pending identities for adoption. Completed and half-open
children still share the existing four-entry backlog. No ninth TCB is needed
when the eight-block transport pool is full.

Process exit abandons the value registration before `fd_close_all`, credential/
process publication and CR3 teardown; detached reaping abandons it again before
address-space destruction. No TCB, frame, user address or borrowed file reaches
the owner. STOP-capable signal checks and scheduler waits remain outside locks
and copy pins. Existing context-busy, IRQ-excluded enrollment and wake-before-
save protocols are untouched.

## Implemented ownership and ordering

BSP and AP LISTEN/ACCEPT now use the registered CPU-0 owner through the existing
retained UDP/TCP records. Both CPU/affinity guards are removed only with this
replacement. LISTEN is a bounded control request; backlog validation stays
full-width and availability is checked again at owner claim. An empty ACCEPT
is pending data work, releases its execution credit and holds no payload slab.
RX, link, timer and protocol sweeps retain their previous service order/bounds.

1. Owner RESERVE selects an existing eligible child block/generation and copies
   its peer address. The child remains in the listener queue. Other reservations
   exclude that block, and the retained listener file fences final close/reuse.
2. After STOP-capable signal handling, the caller repeats output range, capacity
   and overlap validation, then pins the current offer under the value lock.
   Owner lease/error cleanup cannot revoke a pin or overwrite its decision.
3. With local IF clear and no signal/wait boundary, the caller allocates only
   the normal file/node, inserts a provisional fd and sets CLOEXEC. The file is
   fully initialized and closable even before it has a common socket identity.
   A checked rollback reference is registered before releasing the pin.
4. Caller COMMIT/ABORT publishes one bounded owner decision. The owner stages a
   common handle without allocating a transport block, checks the exact queued
   child identity/state and transfers it only at successful COMMIT. Queue removal
   follows provisional fd insertion. Constructor capacity still precedes ENOMEM
   and EMFILE, including combined handle/OOM saturation.
5. On success, the caller copies peer outputs only after acknowledgement and
   returns the installed fd. On failure it removes/closes the provisional fd;
   no peer output is copied. An eligible child stayed queued throughout abort.
   A reset, closed/reused child generation or failed listener cannot trap or
   accidentally adopt a replacement child: owner sweeps/final detach clean the
   failed transport using the existing policy.

Before a decision, terminal owner failures still repeat the historical caller
output/network/signal precedence. Review found that collecting an EIO/EBADF
before this check could bypass output revalidation or a caught interruption.
The regression rejects a capacity changed during the wait before returning the
owner's link failure. Linking the same test against the archived pre-fix ACCEPT
handler gives the retained EXPECTED_FAILURE, not a fabricated passing baseline.

An unpinned one-second offer lease releases the reservation and yields a pending
turn before reoffering, allowing competing acceptors to obtain that child. It
creates no public ACCEPT timeout. Pins and published decisions never expire.
Acknowledgement-only signal deferral covers only already-published commit/abort/
cancel decisions, never waiting for a connection, admission or a payload credit.
STOP/KILL published while the pin is held takes effect after the bounded decision;
the fixture does not park a continuation inside the pin to manufacture a race.

The owner drops the accepted-file rollback reference only after adoption or
rollback is resolved, before terminal acknowledgement. A live fd retains its own
reference; a dead caller has already closed it, so the owner can perform the last
put safely. The listener reference remains until generic retained cleanup.
Repeated exit/reaper abandonment is idempotent. No caller return is required.

All socket final-close callbacks now only mark common-slot detach and request
polling. They neither unstage nor mutate TCP state. The owner captures the stable
kind, performs UDP cancellation or TCP unstage/detach unlocked, then releases
the common slot. TCP's separate detach fence prevents early endpoint reuse.
Accepted children own independent files/endpoints/channels and survive listener
close, rebind and the departure of other inherited descriptors.

The typed readiness scan publishes ACCEPT readiness without a slab. Reservation
decisions remain level-published through generic hint clearing. Existing lock
ranks never nest; copies, allocation, protocol steps, puts and wakes happen
outside the value lock. No new worker, NIC interrupt, MSI or scheduler primitive
was added. Client/UDP/control, wire, deadlines, SIGPIPE, SHA-256 ISNs, conditional
hardware entropy assurance and zero reboot quiet period are preserved.

## Realized state budget

The Phase 2C 168-byte typed record grows to **192 bytes**, adding child identity
and a checked accepted-file rollback reference: 64 records = **12,288 bytes**.
This is an explicit reallocation of existing ledger headroom, not a claim that
the Phase 0 <=128-byte row or Phase 2C 168-byte row remains literal.

| Ordinary writable static state | Bytes |
| --- | ---: |
| Shared UDP/TCP owner, including eight 65,536-byte slabs | 536645 |
| Control owner | 8720 |
| Generic handoff | 5729 |
| Per-task registration allowance | 2048 |
| Alignment/metadata allowance | 4096 |
| **Total** | **557238** |

The hard ceiling remains **589824 bytes (576 KiB)**. Eight shared payload credits,
64 retained records, 32 execution credits and reserved control admission remain;
ACCEPT has no independent payload or transport pool. The C aggregate assertion,
ordinary object section measurements and GDB record/array sizes are retained.

## Host verification and boundaries

Actual owner/syscall/socket/IPv4/TCP code passes ASan/UBSan and separate TSan.
Deterministic scheduler, clock, fd, allocation and controlled-peer adapters cover:
distinct competing reservations; a seven-second pinned lease; expiry handing a
child to another acceptor; abort restoration; irrevocable decision versus cancel;
pre-adoption data/FIN and subsequent listener-close independence; reset between
fd insertion and adoption; fd/common-handle/transport/heap exhaustion; every
constructor allocation failure; capacity-before-OOM; output/signal precedence;
queued LISTEN link failure without mutation; and repeated death/last close before
claim, after reserve, after provisional COMMIT and after ABORT publication.

All descriptor tables, normal allocations, retained references and handoff records
return to baseline. Existing client/stream cases also pass: 64 KiB each way,
<=4096 SEND, READ/WRITE, EOF/reset/half-close and original-writer SIGPIPE; competing
readers, copy/commit races, eight shared slabs/FIFO/control progress, timed calls
and admission expiry at 100/1000 Hz. Legacy backlog overflow/duplicate SYN,
half-open expiry and full-pool adoption remain actual-code host evidence.
TCP's deterministic TSan run alone is not concurrent TCP scheduler proof.

Separate actual-code UDP and control campaigns pass ASan/UBSan and TSan with
48 concurrent actors, 1920 exact echoed UDP datagrams and 9600 coherent control
requests respectively. Exact reclamations are in their manifests/logs. Ethernet,
UDP sockets, IFGET/IFSET/configuration, DNS codec/resolver/framing, TCP expiry
guard, reschedule-return policy and stop-metadata host regressions pass.

Network host runner includes use `-iquote src/kernel` so the new kernel `time.h`
does not shadow host `<time.h>`. The network fd adapter undefines host WNOWAIT
alongside its existing wait-macro isolation. No Time header or product code was
changed. The initial host compile failures remain retained. The pre-existing
sudo host mock failure from Phase 2C remains unrelated and is not relabeled or
claimed passing by these network targets.

## Guest fixtures and evidence boundaries

Fresh regular-file installed NVMe copies are the only data disks. Strict exact
QEMU argv audits reject alternate drives, blockdev, CDROM and USB attachments.
Paired OVMF code and output-local variable copies are used. Native-Linux QMP
socket directories in the carrier variant contain only named temporary control
sockets and are removed after bounded QEMU reap; argv, exchanges and evidence
stay archived. No shared release ISO/raw media is built or attached.

Private snapshot instrumentation proves real CS=3 LISTEN/ACCEPT and stream
syscall CPU masks separately from registered CPU-0 owner masks/assertions.
SMP=8 splits CPU0–3 and CPU4–7 across the two boots; production launch remains
pinned. The probe and private syscall/checkpoint hooks are absent from the
ordinary build. Kernel direct dispatch is not used as AP-entry evidence.

Controlled localhost peers send uniquely tagged 256-byte streams and FIN,
checking exact echoed bytes or the documented discard on KILL/final-close.
Independent capture decoding checks Ethernet padding, IPv4/TCP checksums,
SYN-relative stream bytes and the exact per-peer multiset. Four inherited
acceptors deliver children once. A shared accepted descriptor is read by parent
and child on different CPUs where available; each consumes a distinct ordered
128-byte half and closes independently. Actual fd exhaustion leaves the same
child available for a successful retry.

A stopped acceptor retains a full four-child backlog, independently observed as
four SYN/ACK tuples before CONT. IFGET, ping and DNS UDP/TCP fallback complete
before adoption resumes; all four children then drain exactly. Public STOP/CONT/
KILL/INT on an empty listener and private STOP/KILL publication at reserve,
provisional insertion, COMMIT and ABORT all have explicit exit/recovery checks.
Host fake-time evidence, rather than elapsed host seconds, proves lease durations.

After producer exit, observers require FREE generic/typed records, no pins,
reservations or retained accepted files, eight free slabs and zero common/TCP
handles, blocks or detach flags. Existing TIME_WAIT policy is preserved rather
than called a leak. Every normal two-boot case explicitly persists/reloads config,
compares bytes and root:root 0644 metadata, cleanly shuts down and passes Linux
e2fsck. Absent-NIC cases check actual Ring 3 EIO/argument precedence, shutdown and
filesystem integrity; they make no network-owner execution or config-apply claim.

## Retained attempts and limitations

No previous result is erased or relabeled. `gate-xb9dr2r4` passed both BIOS/SMP=1
boots but retains FAIL for development-time source changes. `gate-z76slu6_`
passes two BIOS/SMP=4 boots with the earlier STOP fixture. `gate-c43_6ann` passes
its first UEFI/SMP=4 boot and times out in the next inherited-listener attempt:
only three of four peer connections appear in its capture; that version lacked
per-peer failure retention, so the fourth submission's cause is not established.
Later fixtures retain every peer attempt/result rather than inferring success
from a guest prompt. `gate-868jg4cz` passes two UEFI/e1000 carrier boots but retains
FAIL for test-source changes during development. `gate-49ci4g8l` passes four
absent-NIC boots but retains FAIL for the subsequent LISTEN correction.

Initial ordinary-build measurement used a wrong object path; its successful
compile and failed measurement manifest are retained. Later ordinary verification
uses every `.bss`/`.data*` section and measures the actual 192-byte record.
Earlier sanitizer attempts retain adapter/preparation failures; none counts as
final-source acceptance. Final campaign identities/results are listed below.

Phase 2C's original full-matrix overall FAIL for concurrent unrelated command
changes, its executable reconciliation and fresh e1000/carrier qualifications
remain exactly as recorded in [Phase 2C](network-smp-phase2c.md). This phase does
not convert that old manifest to PASS. No physical, entropy/security audit,
throughput, idle-CPU or general TCP-conformance acceptance is claimed.
An unresponsive owner/CPU still requires containment; pinned storage is never
freed on a guessed timeout and no automatic owner restart is introduced.

## Final verification artifacts

Paths below are relative to the repository; every directory retains its manifest,
source/build identities, exact commands, logs and applicable captures/filesystem
audits. Final guest campaign manifests are authoritative, including their source
preservation fields; earlier FAIL manifests listed above remain unchanged.

| Campaign | Artifact directory | Result |
| --- | --- | --- |
| Listener/lifecycle, e1000e, BIOS/UEFI × SMP=1/4/8, two boots each | `build/network-smp-phase2d/gate-f7u_9_g7` | PASS 12/12 boots, clean source guard |
| Absent NIC, BIOS/UEFI SMP=4, two boots each | `build/network-smp-phase2d/gate-korg6gho` | PASS 4/4 boots, clean source guard |
| e1000 listeners and carrier loss/recovery, BIOS/UEFI SMP=4 | `build/network-smp-phase2d/gate-6gmvfzzm` | PASS 4/4 boots |
| Phase 2C client/stream regression, e1000e BIOS/UEFI SMP=4 | `build/network-smp-phase2c/gate-u2iw8zrb` | PASS 4/4 boots |
| Phase 2C carrier regression, e1000 BIOS/UEFI SMP=4 | `build/network-smp-phase2c/gate-yiv019cf` | PASS 4/4 boots |
| Phase 2B UDP/DNS regression, e1000 BIOS/UEFI SMP=4 | `build/network-smp-phase2b/gate-r09u89iy` | PASS 4/4 boots |
| Actual listener/client/socket owner ASan/UBSan and separate TSan | `build/network-smp-phase2d/host-sks45rpp` | PASS both runtimes |
| Actual UDP owner ASan/UBSan and TSan | `build/network-smp-phase2b/host-vzklmqen` | PASS both runtimes; TSan 7082 exact reclaimed records |
| Actual control owner ASan/UBSan and TSan | `build/network-smp-phase2a/host-c7q3sr0m` | PASS both runtimes; 9604 exact reclamations |
| Strict ordinary kernel/initramfs, private probe exclusion and state ledger | `build/network-smp-phase2d/release-le8zfp20` | PASS |
| Pre-fix terminal-output precedence regression | `build/network-smp-phase2d/terminal-precedence-negative` | EXPECTED_FAILURE against archived old handler; current handler passes |

The complete successful host command is retained in
`build/network-smp-phase2d/final-host-current.log`: `make
test-network-smp-phase2d-host test-network-smp-phase2b-host
test-network-smp-phase2a-host test-net-tcp-socket-host test-net-eth-host
test-net-socket-host test-net-ifconfig-host test-net-dns-host
test-resched-return-host test-s8-stops-host`.

The primary instrumented kernel SHA-256 is
`6d71c92ca46534838b029d078722e42c5e720f752fa80769b2b17e377cfefc11`;
its initramfs is
`64083c42b0c02c2736041a9f0e9b60da6b75fee55928c6ef74bdf79ee6616633`.
The isolated ordinary kernel is
`2f15ee5fbbd1abaab5e575b6a1b459322c06c20a7b8c5e268c91f5906cf07d91`;
ordinary initramfs is
`21eecbe452ec5d61e6840aa3db0e2eb1f6e617518787486507b8819fd16d679f`.
The ordinary manifest verifies private fixture hooks/probes absent and unchanged
shared `bin/fortress.iso` and `bin/fortress.img` hashes; no shared media rebuild.

The first UDP/DNS guest follow-up stopped before boot because Phase 2B's private
instrumentation anchor became ambiguous after adding ACCEPT. Its failure and the
overall `build/network-smp-phase2d/followups.json` FAIL are retained. The fixture
anchor was restricted to the UDP dispatcher; no production change was needed.


The UDP/DNS retry `gate-r09u89iy` passes all four boots, independent wire audit,
exact caller/owner/reclamation checks, configuration persistence and Linux fsck.
Its source guard reports concurrent nonproduction documentation updates only;
no executable or fixture source changed during the run. Together the six final
guest campaigns above pass **32/32 boots**. The failed earlier composite stays FAIL.

After guest verification, two fixture-only cleanups were made: the generic old AP
probe and installed-root harness now expect EINVAL for an unbound listener rather
than the removed CPU guard, with an honest validation banner; the Phase 2D runner
now exits nonzero if its DNS peer makes the final manifest FAIL. The generic probe
passes the same strict freestanding compile; positive AP listener evidence still
comes from the dedicated guest fixture. The exact parsed runner exit guard passes
both success and failed-manifest checks in `fixture-exit-negative.json`.
These changes do not alter tested production sources or guest kernel behavior.

`build/network-smp-phase2d/verification-summary.json` independently indexes the
individual final manifests and compares current production/build inputs with the
strict ordinary build. It also checks shared media hashes, unchanged HEAD,
production changes confined to `src/net`, and `git diff --check`. The historical
ordinary-build source snapshot includes the pre-cleanup fixture/document versions;
its production inputs remain byte-identical. No source guard is relabeled.
