# Network SMP Phase 2C — TCP clients and streams

2026-10-16. Phase 2C implementation and network behavior verification complete:
12 matrix boots, 4 current-checkout e1000 boots and 4 carrier-recovery boots pass.
The full-matrix repository-preservation guard is qualified below. Do not read
this as an unqualified PASS of that campaign's overall manifest.
No physical acceptance. LISTEN/ACCEPT migration remains Phase 2D; production
spawn rollout remains Phase 3. Commit/publication are separate user-authorized
steps. Concurrent DHCP
work, including its independently created commit, was preserved.

## Actual-path audit and implementation

`syscall_dispatch` routes socket READ/WRITE to `net_tcp_syscall`; numbered calls
enter via `net_socket_syscall`. The former BSP continuation directly mutated TCP
with IF clear and used a shared 65536-byte scratch. CREATE/BIND also ran there.
LISTEN/ACCEPT still use BSP staging, fd insertion and adoption, with AP guards.

Client CREATE/BIND/CONNECT/SEND/RECV, timed variants, READ/WRITE and SHUT_WR now
use the same registered CPU-0 owner from BSP and AP. Mutating client APIs assert
registered ownership. No BSP fast path, second worker, NIC IRQ or MSI was added.
Production pinning and ISN/entropy/reboot-quiet policy are unchanged.

The existing retained UDP records, FIFO tickets, copy pins and eight slabs now
serve UDP and TCP. Slabs grow to 65536; there is no separate TCP pool. SEND
validates <=16384 and stages a short chunk <=4096, accepted once. Stalled CONNECT,
empty receive and TX-capacity waits hold no slab. Receive probes readiness before
credit allocation, stages <=65536 bytes and reserves the prefix. Competing readers
wait; owner commit consumes once, abort consumes nothing. Pinned data remains
immutable beyond the one-second offer lease. Lease expiry is not a public timeout.
EOF/errors/CONNECT remain runnable with all eight credits pinned.

Phase 0 called 65536 the existing receive limit. Actual baseline code applied
MAX_SYSCALL_WRITE_LEN (16384) to receive too, despite its 64 KiB scratch. This
implements the user-requested/planned 65536 bound; it does not claim baseline
already admitted that length.

CONNECT initiates once per claim. Handshake readiness is an offer, acknowledged
after caller signal handling. Cancellation/expiry detaches the attempt on owner.
Absolute BSP deadlines span admission/queueing/retries. Invalid/backwards clocks
return EIO. Admission expiry precedes protocol claim; route/configuration/state
are checked on owner, without an AP protocol fast path. Positive SEND acceptance
and receive commit survive later cancellation. SIGPIPE targets the original TID.

Final vfs_close publishes detach and requests polling; net_tcp_tick detaches the
transport. Checked references fence dup/inherited close and caller death. Exit,
termination and reaper cleanup abandon registration before address-space teardown.
No request borrows user addresses, frames, CR3, stacks or TCBs. Range checks repeat
after STOP-capable signal handling. Acknowledgement-only waits apply solely to
published copy/CONNECT decisions and cancel acknowledgements, never peer waits.

RX/link/protocol/timer sweeps precede bounded handoff service. Typed pending work
releases execution credits. Copy decisions, FIFO eligibility and TCP readiness
stay level-published after hints clear. Scheduler predicates acquire-read scalars.
Rank-1 locks do not nest; protocol calls, copies, wakeups and puts run outside the
value lock. LISTEN/ACCEPT are the explicit legacy owner exception until Phase 2D.

## Host evidence

`build/network-smp-phase2c/host-ai2h6zby`: ASan/UBSan and separate TSan PASS,
actual linked owner/syscall/socket/IPv4/TCP, compiler-discovered transitive source
identities and exact argv/binary hashes. TCP uses deterministic scheduler/peer
adapters; its TSan pass alone is not concurrent TCP scheduler proof.

Cases: binary 64 KiB each way, <=4096 SEND, READ/WRITE, half-close, EOF/EPIPE,
original SIGPIPE actor; accepted SEND versus late cancel; CONNECT offer/cancel
and late ack; seven-second pinned lease, competing readers, abort restoration,
commit versus cancel; timed offers, 100/1000 Hz and backwards clocks; eight pinned
slabs, ninth nonblocking EAGAIN, shared UDP/TCP FIFO, slab-free EOF readiness and
twelve exact reclaimed records with base references restored. Legacy listener
backlog/half-open/rollback/full-pool adoption, child independence, FIN/data order,
repeated RX readiness and link recovery also pass.

Review forced AP copy completion between owner snapshot and terminal link cleanup.
`host-i2jv6mwc` retained the failure: cleanup lost newly unpinned COMMIT/credit.
The corrected owner defers active pins AND newly published decisions. The passing
suite proves four positive bytes survive, consume once and release their credit.

Admission review also covered owner FIFO promotion while the live caller remains
stopped in its publication wait. Resumed CONNECT expiry/interruption now publishes
cancel and waits solely for its owner acknowledgement before returning. The host
case fills 24 data credits, forces promotion and a transmitted SYN without another
caller publish, then advances the original deadline; it checks owner cancellation
and all 25 reclaimed records. Initial host-q282a6i7/host-w5kdjfis attempts retain a
fixture failure: the previous CREATE's cleanup fence had not drained, so the
forced wait was registration rather than publication. No SYN was fabricated to
weaken that assertion; the corrected fixture drains CREATE before saturation.
`admission-before-fix/manifest.json` additionally records the EXPECTED failure
when this regression is linked against the archived ordinary pre-fix owner:
CONNECT returns before cancellation resolves, failing the ENOTCONN assertion.
The current owner passes that same test. An initial instrumented-source attempt
failed linking private serial helpers; its build log is retained too.

`build/network-smp-phase2b/host-bnavm1bb` passes ASan/UBSan and TSan: 48 actual
concurrent actors, 1920 exact echoed UDP datagrams, saturated data credits and
retained references/control progress. `final-host-regressions-current.log` passes
TCP owner/listener/runtime expiry guard, Ethernet, UDP sockets, IFGET/IFSET/config
and DNS codec/resolver/fixture gates. The broader permissions target
passed 32768 credential/stack vectors but failed in the unchanged sudo mock:
sudo now uses SYS_STAT to probe /sbin and exports PATH=/bin:/sbin; its older mock
handles neither. No unrelated sudo source or mock was changed for this task.
The separate actual-control-owner `build/network-smp-phase2a/host-dwr1y7mi`
passes ASan/UBSan and TSan with 48 producers, 9600 requests and 9604 exact
reclamations: coherent configuration/probes, saturation, cancellation, deadlines
and clock reset.

## Guest fixture and supporting evidence

Only disposable installed-root NVMe copies are attached. Exact argv allowlists
admit one target, named NIC, capture and paired output-local OVMF variables;
alternate storage/physical attachments are rejected. Baseline hashes, sources,
build hashes, logs, pcaps, config bytes/mode/ownership and e2fsck are retained.
Each case boots twice and reloads persisted configuration.

Private snapshot instrumentation stages AP launch and records CS=3 syscall CPU
masks separately from owner CPU/TID assertions and mask. Up to four concurrent
SEND callers are gated pending together. After producer exit, checks require
FREE generic/typed records, no pins/reservations, eight free slabs, zero common
handles/TCP blocks/endpoints/detaches. Existing TIME_WAIT policy remains; SMP=8
splits CPU0–3 and CPU4–7 across two boots to respect its bound.

Local peers supply binary echo, hostname/DNS UDP truncation plus TCP fallback,
eighteen buffered bytes then RST, HTTP 64 KiB, and advertised zero window. Only
QEMU preloads an archived shim limiting its one loopback zero-peer socket buffer;
no system setting/kernel transport change. Independent audits check framing,
checksums/padding, SYN-relative sequence assembly, exact bytes/RST/zero window,
unanswered ARP, HTTP request/body and wget SHA-256.

Guest cases cover inherited/shared fd close during AP receive; STOP/CONT/KILL/INT;
IFGET/ping/another transfer during stalled CONNECT/empty RECV; IFGET/ping/forced
AP wget during zero-window SEND; AP listener guards and exact cleanup. STOP lasts
22 HOST seconds, not proof of 20 BSP seconds under TCG. Immutable deadlines/late
copies are asserted with controlled host BSP clocks; no stronger guest time claim.

`gate-qpjs1oka` passed 12/12 e1000e BIOS/UEFI × SMP=1/4/8 boots, capture/fsck and
source-preservation checks BEFORE the final cleanup race fix. It supports the
implementation but is not final-source acceptance. `gate-urymqat5` completed
four e1000 boots using that old snapshot, then retained FAIL for the subsequent
source correction.
Final primary, e1000 and pending-operation link results follow.

`gate-j61ttqg_`: all 12 e1000e BIOS/UEFI × SMP=1/4/8 boots PASS, including
independent stream/HTTP/reset/zero-window audits, AP/owner provenance, resource
ledgers, two-boot configuration bytes/metadata and e2fsck. Baseline hash unchanged.
The OVERALL manifest remains FAIL: final preservation checking found concurrent
Makefile, user/ps.c, user/shell/builtin_exec.c, user/tools/common.h and
user/tools/disk.c changes. They add which/env, ps columns, disk PARTUUID lookup
and shell builtin options. They were preserved, not reverted or silently ignored.
The saved-argv audit passes 12 allowlists and 61 hostile-storage exclusions.

`build/network-smp-phase2c/reconciliation/manifest.json` PASS qualifies that
failure without relabeling it: all 217 original kernel source identities match
the current checkout. Fresh/current ordinary and instrumented kernel .text/.data
sections are byte-identical to the tested builds. Kernel .rodata outside the
embedded init ELF byte blob also matches; embedded init's executable,
constant and initialized-data sections themselves match. The Ring 3 probe and
wget/ping/ifup/ifconfig/nslookup/sha256sum .text/.rodata/.data match too. Extracted
sections and hashes are retained with the reproducible comparison driver. The
changed shell is covered by the fresh current-checkout e1000 campaign, rather than
being silently described as the earlier shell binary. This is a qualified source-
preservation result, not an unqualified PASS of the original repository guard.

`gate-jo87fzup` PASS: fresh e1000 BIOS/UEFI SMP=4 two-boot integration, 4/4 boots,
including the concurrent new commands and shell. Executable/fixture source guard
passes; only this report changed during execution. All capture/provenance/resource/
configuration/filesystem checks and unchanged baseline hash pass. Its strict
snapshot build also compiles the newly added user commands. No source-guard failure
was hidden or reclassified in either campaign.

`gate-31nryodv` was explicitly interrupted after its two BIOS SMP=1 boots passed,
to include the admission correction above. Its KeyboardInterrupt log and partial
artifacts are retained; it does not count toward final acceptance.

`gate-9jnr6i6v` PASS, 4/4 e1000 BIOS/UEFI SMP=4 carrier-loss boots. Three AP
callers launch unanswered CONNECT, empty RECV and zero-window SEND. IFGET observes
carrier down; each actor returns EIO, then carrier-up ping and fresh exact 16384-
byte echo succeed. CPU masks separately cover AP1/2/3 entries and owner CPU0.
Every boot checks all resource ledgers, config persistence/metadata and e2fsck.
The archived probe override extends these three deadlines to 50 BSP seconds and
requires EIO; source/build argv/hash and effective driver are retained. Snapshot
kernel instrumentation matches the primary campaign; only the external test ELF
and QMP launch options differ. No executable/fixture source drift; subsequent
contract documentation changes are explicitly listed as nonproduction changes.

QMP uses one named native-Linux temporary AF_UNIX control socket because DrvFS
cannot bind one (`gate-t6jm3zw3` retained that launch failure). Actual argv records
only the named NIC id and that exact additional QMP socket; normalization passes
the ordinary allowlist and 16 hostile-storage exclusions. No physical storage
is attached. QMP exchanges remain archived; native sockets are removed after reap.
`gate-fwj3b2r_` completed BIOS application/recovery/shutdown checks but retained
FAIL when its audit compared different QEMU-pcap/Python clock epochs. The final
audit instead archives the complete capture-record boundary at QMP link-up and
requires the exact fresh transfer's SYN to occur after that boundary.

## Isolated ordinary build

`build/network-smp-phase2c/release-current-verification.json`: strict ordinary
kernel/initramfs build in a fresh source snapshot PASS. Private fixture symbols,
strings and net-cpu-probe archive entry absent; shared ISO/raw media not rebuilt.
All .bss/.data* included: owner535109 + control8720 + generic5729 + registration
allowance2048 + alignment/metadata4096 =555702 bytes, below589824 (576 KiB).
GDB on that ordinary ELF measures udp_request_t=168 and its 64-record array=10752.
The Phase 0 per-row <=128-byte planning estimate is therefore not met literally;
the realized ledger reallocates existing headroom without changing the hard total
ceiling or adding payload credits. This distinction is explicit in the contract.

## Retained failures and limits

No failure relabeled. Retained attempts include host compile/adapter mistakes;
concurrent overwritten adapters/Makefile reapplied without reverting DHCP;
phase-snapshot ack and copy-revocation races; long-command echo timeout; SLIRP
buffering before shim; reset half-close producing FIN; TIME_WAIT exhaustion before
SMP=8 splitting; resource checks before producer exit; jobs/prompt observation
race prematurely releasing zero peer. Focused gate-uf23fujv and gate-f8hghro2
completed application checks but retain FAIL for source drift, not acceptance.

No physical, security/entropy audit, throughput or idle-CPU measurement claim.
Listener migration is Phase 2D, unpinned commands Phase 3, hardware acceptance
separate. Manifests, failures, captures and bounded process cleanup are retained.

## Final verification index

`build/network-smp-phase2c/verification-summary.json` records
VERIFIED_WITH_PRESERVATION_QUALIFICATION: 20 passing guest boots, three sanitizer
campaigns, independent saved-argv exclusions/capture checks, unchanged disposable
baseline hashes, strict ordinary-build identity/absence checks and executable-byte
reconciliation. The original full-matrix FAIL and unrelated sudo-mock failure
remain explicit. The final audit verifies current kernel/user build inputs still
match the ordinary snapshot; verification did not rebuild shared release media.

Ordinary kernel SHA-256:
`0e1dca93381d89603a29a02d2abcc5fd42e8630d0ef989f598a9d949cc45630a`.
Ordinary initramfs SHA-256:
`c951b1d78ccec016286407b25932f584a529e3c34cc6c10977710e0b0d413a38`.
These are isolated verification artifacts, not published release media.
`git diff --check` passes excluding the concurrently edited COMMANDS.md; its
Markdown hard-break trailing spaces were preserved as unrelated user work.
