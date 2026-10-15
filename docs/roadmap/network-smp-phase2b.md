# Network SMP Phase 2B — UDP / DNS

2026-10-15. COMPLETE. Phase 2B routes UDP create/bind/send/receive, copied
payload reservation/commit and final detach through the registered CPU-0 network
owner from every application CPU (BSP and AP). Real Ring 3 AP execution is
verified across BIOS and UEFI at SMP=1/4/8. TCP integration (Phase 2C/2D) and
production spawn rollout (Phase 3) remain separate subsequent phases; no TCP AP
or physical acceptance is claimed.

## Ownership and transaction audit

Both BSP and AP UDP callers validate their own ABI, ranges, tuple and bind
capability. IFSET and tuple validation use a coherent configuration snapshot;
bind is checked again against owner-time configuration. The caller retains a
checked file reference before any signal/wait boundary after registration.
Requests contain copied scalars and stable endpoint identities; the owner never
uses caller page tables, user pointers, frames or TCBs. Descriptor installation,
output validation/copy and credential checks stay in the original continuation.

Constructor allocation is caller-only; endpoint activation is owner-only. A
failed allocation is still submitted for capacity precedence (ENOSPC before
ENOMEM). Existing descriptor exhaustion remains EMFILE before allocation.
Failed activation, signal/death and failed fd installation all retain the
operation reference until typed owner cleanup. File installation has a separate
checked reference. Final UDP file release only marks detach and requests polling;
the endpoint remains unavailable for reuse until the owner cancels IPv4 work,
invalidates its operations and acknowledges detach. Request and endpoint
generation exhaustion fail closed. Existing TCP guards and policy are preserved.

Configuration ordering is explicit: a SEND's slab grant only prepares copying.
Its protocol-acceptance turn after payload publication uses the current owner
configuration. IFSET may pass an unready or stopped copier, preserving control
progress. This qualifies Phase 0's single-claim wording; submission order does
not reserve an address epoch across a copy handshake. Specific bind addresses
are rechecked in the owner. Established TCP tuple behavior remains unchanged.

Generic retained records cannot recycle before unlocked typed cleanup, even
after caller interest disappears. Cleanup inspects eight rotating records per
turn; all references are dropped outside Rank-1 locks. UDP protocol calls assert
the registered owner TID, and production owner service also asserts CPU 0.
NIC polling and protocol/timer sweeps precede bounded handoff work. Pending work
releases execution credits. Submission, copy decisions, cancellation, cleanup
and final detach explicitly notify the owner; existing BSP timer wakes remain.

Eight static 1472-byte slabs stage complete UDP payloads. A caller copy pin
prevents overwrite/reclamation despite cancellation or expiry. No wait, signal
check or user scheduling occurs inside a copy pin; outputs are revalidated after
the preceding signal boundary. Receive commit consumes the entire head datagram
exactly once and wins later cancellation. Abort/lease expiry consumes nothing,
preserves ordering and keeps the original five-second receive deadline. The
legacy seven-second abandoned-operation lease is disabled for retained typed
operations. One-second reservation leases cannot extend the I/O deadline.
FIFO payload tickets prevent new claims from repeatedly overtaking eligible
pending callers; ticket exhaustion fails closed. The main generic, control,
UDP-record and payload arrays measure 35,328 bytes in the strict build, plus
small pool metadata and existing task registrations, below the 576 KiB ceiling.

SEND interruption resolves the owner's bounded cancellation acknowledgement;
accepted datagrams retain a nonnegative count, including zero-length sends.
Zero-progress cancellation returns EINTR and cancels queued ARP work. No claimed
send is retried. The acknowledgement-only scheduler entry shares the ordinary
IRQ-excluded enrollment and context-switch implementation, deferring signals
until that decision. Ordinary waits remain interruptible. This entry must never
wait on peer traffic. An unresponsive owner still requires containment: pinned
memory cannot safely be reclaimed or the worker restarted.

Blocking internal admission/payload saturation sleeps; nonblocking internal
saturation returns EAGAIN. Nonblocking receive reserves payload credit before
autobind or protocol reservation. Existing same-direction contention and finite
receive expiry retain EAGAIN. UDP VFS read/write remain unsupported. TCP's shared
receive scratch, streams, listener adoption and guards remain Phase 2C/2D work.

## Production fixes delivered in Phase 2B

1. **Receive commit beats later cancellation:** Committing a valid payload copy marks
   completion terminal and releases the reservation, winning over any concurrent or
   subsequent signal/cancellation attempt.
2. **SEND cancellation resolves owner ack via `sched_wait_until_ack`, preserving accepted positive results:**
   Interrupted SEND resolves bounded owner cancellation acknowledgement; accepted datagrams
   retain a nonnegative count (including zero-length sends), while zero-progress cancellation
   returns EINTR and cancels queued ARP work.
3. **ENOSPC beats preallocation ENOMEM:** Constructor allocation is caller-only, but failed
   allocations are still submitted for capacity precedence so internal pool exhaustion (ENOSPC)
   takes precedence over preallocation ENOMEM.
4. **Nonblocking slab admission precedes autobind:** Nonblocking receive reserves payload credit
   before attempting autobind or protocol reservation.
5. **FIFO payload tickets prevent new-claim starvation:** FIFO ticket sequencing ensures pending
   callers are granted slab credits in order, preventing new claims from repeatedly overtaking
   eligible waiters; ticket exhaustion fails closed.
6. **Clock floor advances each owner turn:** The per-call clock floor advances on each owner turn
   rather than remaining frozen at admission, correctly rejecting backward clock anomalies after progress.
7. **Unavailable EIO beats internal slab saturation for SEND and RECV:** When network service or
   NIC is unavailable, EIO takes precedence over internal slab saturation.
8. **Level predicate (`net_udp_owner_work()`) prevents typed copy/payload work from depending on poll-tick rescue after the generic hint clears:**
   A lock-free typed level predicate tracks pending copy decisions and waiting payload grants with
   free credits, checked both before sleep and after bounded service.
9. **No TCP source, syscall-entry assembly, or production spawn-pinning change:** TCP sources and
   syscall-entry assembly are untouched; production commands remain pinned until Phase 3.

## Verification ledger

The Phase 2B acceptance campaign is COMPLETE. All primary e1000e guest cases, e1000
follow-up guest cases, host sanitizer gates, and legacy regressions pass.

### Primary campaign: `build/network-smp-phase2b/gate-fhnnk8qq`

- **NIC:** e1000e, BIOS/UEFI × SMP=1/4/8, two boots per configuration: **12/12 boots PASS**
- **Source guard:** PASS (no concurrent source changes; workspace created fresh from current tree by `scripts/create_ext4_guest_workspace.py`, including the level predicate fix `net_udp_owner_work()` in `net.c`)
- **Audit summary:** `{"result": "PASS", "argv_audits": 12, "negative_exclusions": 61, "no_process_launched_by_selftest": true}`

| Configuration | Boot | Result | Caller Mask | UDP Syscall CPU Masks (sock, bind, send, recv) | Owner Steps | Reclaimed Ops | Sockets (created / detached) | Echo Datagrams | Remote Wakes |
|---|---|---|---|---|---|---|---|---|---|
| `bios-smp1` | boot0 | PASS | 0x1 | [0x1, 0x1, 0x1, 0x1] | 19,959 | 390 | 27 / 27 | 71 | 0 |
| `bios-smp1` | boot1 | PASS | 0x1 | [0x1, 0x1, 0x1, 0x1] | 28,383 | 390 | 27 / 27 | 71 | 0 |
| `bios-smp4` | boot0 | PASS | 0xF | [0xF, 0xF, 0xF, 0xF] | 36,542 | 1,110 | 93 / 93 | 188 | 6,814 |
| `bios-smp4` | boot1 | PASS | 0xF | [0xF, 0xF, 0xF, 0xF] | 31,249 | 1,110 | 93 / 93 | 188 | 6,342 |
| `bios-smp8` | boot0 | PASS | 0xFF | [0xFF, 0xFF, 0xFF, 0xFF] | 56,913 | 1,941 | 180 / 180 | 312 | 14,265 |
| `bios-smp8` | boot1 | PASS | 0xFF | [0xFF, 0xFF, 0xFF, 0xFF] | 61,973 | 1,941 | 180 / 180 | 312 | 23,690 |
| `uefi-smp1` | boot0 | PASS | 0x1 | [0x1, 0x1, 0x1, 0x1] | 20,271 | 390 | 27 / 27 | 71 | 0 |
| `uefi-smp1` | boot1 | PASS | 0x1 | [0x1, 0x1, 0x1, 0x1] | 27,968 | 390 | 27 / 27 | 71 | 0 |
| `uefi-smp4` | boot0 | PASS | 0xF | [0xF, 0xF, 0xF, 0xF] | 35,846 | 1,110 | 93 / 93 | 188 | 5,783 |
| `uefi-smp4` | boot1 | PASS | 0xF | [0xF, 0xF, 0xF, 0xF] | 31,486 | 1,110 | 93 / 93 | 188 | 6,949 |
| `uefi-smp8` | boot0 | PASS | 0xFF | [0xFF, 0xFF, 0xFF, 0xFF] | 57,877 | 1,941 | 180 / 180 | 312 | 16,834 |
| `uefi-smp8` | boot1 | PASS | 0xFF | [0xFF, 0xFF, 0xFF, 0xFF] | 61,676 | 1,941 | 180 / 180 | 312 | 24,614 |

### Follow-up campaign: `build/network-smp-phase2b/gate-1te338y2`

- **Command:** `python3 scripts/test_network_smp_phase2b.py --nic e1000 --cpus 4 --jobs 2`
- **NIC:** e1000, BIOS/UEFI SMP=4, two boots each: **4/4 boots PASS** (`result: PASS`)
- **Source guard:** PASS

| Configuration | Boot | Result | Caller Mask | UDP Syscall CPU Masks (sock, bind, send, recv) | Owner Steps | Reclaimed Ops | Sockets (created / detached) | Echo Datagrams | Remote Wakes |
|---|---|---|---|---|---|---|---|---|---|
| `bios-smp4` | boot0 | PASS | 0xF | [0xF, 0xF, 0xF, 0xF] | 36,944 | 1,110 | 93 / 93 | 188 | 8,623 |
| `bios-smp4` | boot1 | PASS | 0xF | [0xF, 0xF, 0xF, 0xF] | 31,565 | 1,110 | 93 / 93 | 188 | 8,748 |
| `uefi-smp4` | boot0 | PASS | 0xF | [0xF, 0xF, 0xF, 0xF] | 34,587 | 1,110 | 93 / 93 | 188 | 7,612 |
| `uefi-smp4` | boot1 | PASS | 0xF | [0xF, 0xF, 0xF, 0xF] | 31,849 | 1,110 | 93 / 93 | 188 | 8,983 |

### Host gates

- **ASan/UBSan + TSan:** 48 actors, 1,920 echoed datagrams each, exact 4,398 / 4,022 reclaimed records. All file references return to one, final allocation and request counts return to zero.
- `test-perm-privileges-host`: PASS (CAP_NET_BIND denial on low port, same-fd retry with capability, byte-order/port boundaries, power and signal capability enforcement).

### Legacy regressions

- **Legacy UDP:** 10/10 PASS (BIOS/UEFI × e1000/e1000e × user/socket at SMP=1, plus BIOS/UEFI e1000/user SMP=4).
- **Legacy DNS:** BIOS + UEFI pinned wire audits PASS (independent raw-wire audits, TCP fallback, hostname client).
- **Legacy link recovery:** 8/8 PASS (cold/warm link recovery across BIOS/UEFI × e1000/e1000e).

### Strict uninstrumented build

Verified fixture absence in `build/network-smp-phase2b/release-verification.json`:
- **Kernel SHA-256:** `9b2c31e5b19764c2b55a34538dea593867f9ce4d8481b5dc89727d7fa025f52e`
- **Initramfs SHA-256:** `5ebe9ab1bc424243ae3651ea961156400ab32dfde88ac27d881219e40c022660`

The host runner records compiler-discovered transitive local headers and every
linked translation unit, including TCP dependencies, compiler identity, HEAD,
commands and binary hashes. Guest snapshots preserve all tracked/untracked
inputs, instrumentation overrides, kernel/initramfs identities, exact argv,
serial/stderr, commands, pcap, filesystem audits and disposable disks.

Final host `host-xe9atux9/manifest.json` passes ASan/UBSan and TSan. Each run
contains 48 actors and 1,920 exact echoed datagrams; all file references return
to one and final allocation/request counts return to zero. Reclamations are
6,131 and 7,152 respectively, including independently counted contention retries.
Deterministic cases cover pin retention past seven seconds, original receive
deadline, ordered restoration, receive commit/cancel, claimed ARP cancellation
with zero later transmission, eight pinned slabs, nonblocking saturation, FIFO
grant ahead of new claims and accepted SEND cancellation before typed completion
with exactly one driver submission, unavailable-service precedence while all
eight slabs are pinned (RECV and SEND return EIO), and a backward clock after progress while
still above admission (EIO and recovery). The per-call clock floor advances on
each owner turn; an earlier admission-only floor missed this reversal.
These are actual-code scheduler/clock/NIC
adapters, not IRQ-frame or hardware evidence. `level-work-regressions.log`
records UDP, TCP socket/tuple/exact 16 KiB transfer, Ethernet, IFCONFIG/ifup,
DNS codec/resolver and CAP_NET_BIND/power sanitizer regression passes.
`permission-enforcement-regression.log` additionally records the actual
`test-perm-privileges-host` enforcement target: denied low-port bind without
CAP_NET_BIND, same-fd retry with the capability, byte-order/port boundaries,
signal authorization and power capability enforcement all PASS. This is distinct
from the older permissive wiring adapter and the stale sudo aggregate below.

The final isolated strict kernel/initramfs build passes fixture-absence checks in
`build/network-smp-phase2b/release-verification.json`. Kernel SHA-256:
`9b2c31e5b19764c2b55a34538dea593867f9ce4d8481b5dc89727d7fa025f52e`;
initramfs:
`5ebe9ab1bc424243ae3651ea961156400ab32dfde88ac27d881219e40c022660`.
No shared release ISO/raw image was rebuilt. The earlier pre-fairness strict
build remains separately recorded in `release-pre-fairness.json`; intermediate
clock/availability/comment checkpoints also retain their own JSON identities.

Guest recovery compares generic reclamations with a separate syscall-return
token ledger after producer exit. A second observer requires every activated
UDP endpoint to receive owner detach acknowledgement. It checks zero retained
files, active waits, pins and occupied slabs. Independent packet decoding checks
IPv4/UDP checksums, framing, zero padding and an exact echo multiset: seven ABI
datagrams per CPU plus 32 per traffic actor. Every case has two boots, actual
Ring 3 CPU-entry masks, CPU-0 owner assertions, remote wakes, persisted config
bytes/root:root 0644 metadata and Linux e2fsck checks.
Final acceptance additionally records four separate actual Ring 3 CPU masks
for SOCKET, BIND, SENDTO and RECVFROM; each must equal `(1<<SMP)-1`.
Control queries cannot supply a missing UDP provenance bit.

The current generic/scheduler sources pass the fresh Phase 1 inert-owner
BIOS/UEFI × SMP=1/4/8 campaign `gate-3968zcce`, including filesystem audits,
clean source guard, six strict argv checks and 31 negative storage exclusions.
Later edits only reconcile a UDP source comment and legacy test prompts/leases;
the tested generic/scheduler implementation is unchanged.

Legacy regressions use disposable LOGIN_TEST=1 ISO snapshots, never shared
release media. `legacy-udp-current-retry.json` records all ten UDP cases PASS:
BIOS/UEFI × e1000/e1000e × user/socket at SMP=1, plus BIOS/UEFI e1000/user SMP=4.
The old AP-negative marker now checks TCP only. Binary/ABI/fd, caught signal,
STOP/CONT/KILL, exact datagrams, corrupt checksums, port recovery and independent
capture checks pass. `legacy-dns-raw-retry.json` records BIOS/e1000/socket and
UEFI/e1000e/socket pinned DNS/TCP fallback/hostname-client regressions with
independent raw-wire audits, plus all eight cold/warm link cases PASS.
The snapshot workspace and fixture override hashes are retained in these JSONs;
this is a qualified fixture invocation, not an unconditional PASS of the old
make targets against today's normal login image.

The last owner-wakeup audit found that a consumed generic hint could leave a
copy decision or newly available slab awaiting the next normal poll tick if
the rotating budget missed its record. A lock-free typed level predicate now
tracks pending copy decisions and waiting payload grants with free credits.
The owner reads it both before sleep and after bounded service. Pending I/O or
an unpinned caller alone does not keep the worker runnable. Host forced-hint-
clearing tests retain runnable work until the decision/grant is serviced, then
verify an idle predicate after drainage. Snapshot recovery additionally requires
zero decision/waiter counters and exactly eight free slab credits. This adds no
timer hook or polling work to AP callers; normal protocol timers remain unchanged.

Phase 1/2A host gates pass ASan/UBSan and TSan with exact 14,474 and 9,604
reclamations (`host-saocz5y8` and `host-xhu789n6`). Scheduler return/stop and
CAP_NET_BIND/power adapters pass; logs are in
`build/network-smp-phase2b/prerequisite-regressions.log`.
The initial UDP/TCP/configuration/Ethernet/DNS regressions pass. The broader
`test-perm-spawn-host` invocation fails in the unchanged sudo adapter because
it does not implement sudo's existing SYS_STAT lookup. Its spawn credential
vectors pass; the failure and output remain in `regressions-initial.log`.

## Retained attempts

Earlier host `host-imruwkr4` passes ASan/UBSan and TSan, including pin retention
past seven seconds; its source manifest omits some linked TCP dependencies and
does not establish acceptance of subsequent changes. Earlier guest failures
`gate-o9x5n4fi`, `gate-7553fg48`, `gate-slo6chzo` and `gate-k4soj5u0` remain as
recorded: port-53 privilege, old argv audit, source drift and an incomplete
exploratory campaign. None is relabeled PASS.

New host attempts `host-ggq0gtw6` and `host-dlgm8nv9` exposed deterministic
fixture rendezvous mistakes: parking the owner before an earlier actor record
had drained prevented new admission. They were terminated and retain failure
manifests. `host-hfa7uaot` passed ASan/UBSan but failed a TSan saturation assertion
because the prior receive's released record had not drained. The fixture now
drains explicitly and waits for both 48 records and 24 queued data credits.

`gate-2gp2bbkl` exposed a snapshot oracle race: FREE can occur between a
background producer's syscalls, so a one-time empty-pool observation cannot
establish producer completion. The runner now waits for shell-observed producer
exit before comparing independent ledgers. `gate-9xxnofl_` completed its first
guest shutdown but correctly failed an overcounted expected packet total
(eight instead of seven ABI sends). The expectation is now derived from the
actual seven explicit send call sites. Failed campaigns retain their results.

`gate-ptzcb1ef` completed BIOS/SMP=4 boot 0 and its independent audits, then
timed out waiting to observe a later finite probe that had already completed.
The final lifecycle snapshot holds an actual admitted UDP/probe request in
PENDING until the signal test releases its gate; protocol sweeps and original
deadlines continue. Cancellation bypasses this gate. This is a deterministic
fixture rendezvous, not production behavior. The earlier runner also shadowed
the numeric operation-count variable with expected configuration bytes, causing
JSON serialization to fail while saving that campaign. Its initial manifest and
saved boot-0 packet/filesystem audits remain; no composite PASS is claimed.

Host attempts during fixture extension remain in their separate directories.
Some passed both runtime sanitizers but correctly failed source preservation
when the test was extended during the run. `host-dz4wnyde`/`host-jbna0tfc` tried
to turn the driver-acceptance cancellation test into an additional remote-echo
check; `host-zr_oquim`/`host-vwxymk0i` then exposed its leftover synthetic reply
in the concurrent echo phase. The final driver-acceptance case disables its
synthetic echo peer, independently checks the transmitted payload and exact
submission count, and keeps the 1,920-datagram echo oracle separate.

The pre-clock-fix Phase 1 inert-owner guest regression
`build/network-smp-phase1/gate-ifwmjux6` passes all six BIOS/UEFI SMP=1/4/8 guest
cases and filesystem audits, then correctly FAILs its strict source guard for
two Markdown files plus the UDP clock-floor/test extension. Its original FAIL
is retained; a fresh final-source regression is required.

`gate-m0lwy63k` likewise passes all six inert guests, then FAILs source drift
during the final UDP availability/test extension. The subsequent clean Phase 1
campaign above replaces that run; neither FAIL is relabeled.

`gate-lz0g4q8w` is retained as incomplete exploratory evidence. Eight boots pass
packet/persistence/resource audits; the run was deliberately stopped after code
and fixture drift, with original manifest copied to `pre-stop-manifest.json`
and owned runner/guests reaped as recorded in `termination.json`. Its helper
exited on pipe closure. It does not validate the final source snapshot.

Legacy preparation failures are retained too: normal-login release ISO fixtures
waited for the old direct-shell prompt; LOGIN_TEST then exposed the new identity
prompt. The UDP injector also needed concurrent UART drainage while typing to
avoid QEMU output backpressure. A normalized DNS observer initially broke its
independent raw-serial membership check; the final predicates accept the current
prompt without changing output bytes. The old STOP-eight-seconds UDP fixture
expected the superseded seven-second abandonment EINTR; it now expects the
original finite receive's EAGAIN, independently covered by the owner host gate.
Runtime successes preceding those fixture/audit failures remain qualified,
not relabeled as complete target passes.

`gate-p6li_m8m` retains eight passing acceptance boots and a rejected BIOS/SMP=8
reboot. The control lifecycle probe explicitly allowed EAGAIN/EINTR, but then
fell through to its generic nonzero-rc exit 12; the runner correctly rejected
the resulting `Done(12)`. The fixture now returns success for those previously
allowed mode-e outcomes, while unexpected errors and failed positive probes
still fail. This changes no kernel behavior. Original manifest, serial and
termination records remain; the fresh corrected full campaign replaces it.
The runner now records a failed case immediately and cancels queued configurations
instead of waiting to expose the first failure after unrelated configurations.

`gate-4u18nac_` passes seven boots with the corrected finite-probe fixture, then
is deliberately stopped for the typed level-predicate fix above. Original FAIL,
pre-stop manifest, logs and explicit process termination records remain; it is
not final-source acceptance. The next frozen-source campaign replaces it.

## Remaining boundaries

No TCP AP call, DNS TCP fallback, NIC interrupt, production affinity rollout,
physical hardware, security audit or performance/idle-CPU acceptance follows
from this phase. Existing SHA-256 boot-secret/tuple/TSC ISNs, conditional hardware
entropy assurance and zero reboot quiet period remain unchanged.
