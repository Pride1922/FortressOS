# Network SMP Phase 3 — ordinary unpinned commands

2026-10-16. Phase 3 rollout and verification are COMPLETE. Matrix acceptance,
carrier recovery, absent-NIC, NMI, sudo, CPU accounting, host ASan/UBSan/TSan,
mounted EXT4/VFS, source reconciliation and strict isolated release builds have
passed. Phase 4 physical acceptance and Phase 5 throughput measurement remain
separate. Shared release media and physical devices are not written.

## Actual creation, ownership and lifecycle audit

All numbered spawn paths reach `process_spawn_from_vfs_group`, including the
shell executor, sudo, ordinary background commands and staged pipelines. ELF,
credentials, fd actions, CLOEXEC/secure sweeps, stack, signal attachment and VMM
scheduler reference are prepared before metadata/runqueue publication. Ordinary
children start on the caller CPU with `cpu_affinity=-1`, `cpus_allowed=CPU_MASK_ALL`
and initialized `current_cpu`. Explicit benchmark `--worker` arguments retain
the selected CPU and one-bit mask, including SMP=1. No public affinity API is
added. Initial trusted boot supervision retains its existing BSP restrictions.

The old global staged list relied on BSP IF exclusion, and staged release
ignored worker placement. A disjoint ordinary rank-1 `staged_lock` now protects
list insertion, lookup, validation and detachment. Whole-group ownership failure
still changes nothing. Release and cancellation detach before any process-table,
fd/VMM/heap cleanup or scheduler operation. Release queues each child on its
initialized target and sends work hints; unbound local publication hints peers.
After queue publication, no staged TCB is dereferenced. Likewise ordinary spawn
returns its reserved PID value rather than reading a child that a remote CPU
could already have exited and reaped. No scheduler switch, CR3, context_busy,
rank ordering or IRQ-excluded enrollment protocol is replaced.

`syscall_entry_stub` uses per-CPU GS scratch and RSP0; user return preserves its
canonical-address/flags checks, signal handling and reschedule policy. Blocking
operations enroll through `sched_wait_until`; wakes scan all scheduler CPUs,
publish READY under their queue locks and send remote reschedule hints. A woken
outgoing continuation stays unstealable until post-switch context publication.
Exit abandons handoff registration before descriptors/address-space teardown;
the detached reaper repeats abandonment before freeing. Staged tasks never ran
and have no handoff token. Process/group/session authority remains in the process
registry; its lock never overlaps staged or scheduler exclusion.

Own fd tables remain private. Clone/dup descriptors retain shared `file_t`
references; streams dispatch to pipe/socket callbacks before regular offsets.
Pipe data/space predicates use atomic publication, transfers use the pipe lock,
and close retains the endpoint through wake completion. Socket continuations
retain checked references before waits, revalidate outputs in caller CR3, and
use the existing registered CPU-0 owner for every operation including final
detach. No network CPU guard was removed in Phase 3: Phase 2D already replaced
them with retained ownership/lifecycle transactions.

The terminal's former BSP-only header was stale: reads already accepted APs, but
their predicates read shared fields without atomic publication. New published
readiness/session/group/tick/event snapshots are atomic-only under scheduler
locks. Authoritative reads recheck terminal exclusion; another CPU consuming the
indication causes retry rather than false EOF. UART error loss publication now
uses the same lock. Hardware ingress, signal worker and input tick ownership
remain BSP; input timer service is not multiplied by the CPU count.

Keyboard-layout syscalls now publish an atomic layout value; each decode uses
one acquire snapshot while decoder state remains BSP-owned. Ordinary AP exits
use atomic stores for the existing global legacy exit-observation values. The
isolated supervisor recovery contract and transition/frame assembly are unchanged;
those diagnostics do not become per-process accounting or a concurrent test API.

Shared regular-file reads formerly advanced `file_t.offset` after filesystem
unlock. New `read_offset` callbacks in ext2, ext4 and runfs read and advance under
their existing filesystem locks, together with writes. No new nested lock or
sleeping file lock is introduced. Remaining immutable boot-byte/read-only
partition reads use optimistic position CAS and retry a pure read on conflict.
Shared readdir indices similarly advance only on successful CAS. Device reads
still route through ext4/ext2 exclusion; ordinary commands cannot bypass storage
ownership. Boot namespace publication remains sealed and immutable. Existing
device/boot-only restrictions stay in place.

Network polling, registered CPU-0 protocol ownership, eight shared 64 KiB credits,
bounded service, reserve/commit/abort, retained generations/copy pins, original
absolute deadlines, SIGPIPE targeting and ISN/entropy/reboot policy are unchanged.
No DHCP, Time or command implementation is modified. CPU accounting hooks are
audited as per-TCB transitions across entry/return/switch/exit; their algorithm
is unchanged and regression evidence is recorded separately.

## Verification design and evidence boundary

`scripts/test_network_smp_phase3.py` builds an isolated strict snapshot, adds
observation only, and installs a private observer in disposable installed roots.
No launch placement patch is applied. Production commands retain ordinary argv.
CS=3 dispatcher CPU masks are recorded per executable/syscall independently of
CPU-0 owner/TID assertions. Benchmark workers are separately observed against
their explicit affinity/mask. Successful output or eligibility alone is not
AP-entry evidence. Private snapshots record pending operations and resource
counts; retained TIME_WAIT entries are reported separately from live blocks.

The runner uses controlled loopback HTTP, DNS UDP/TCP, UDP echo and TCP peers,
exact hashes and independent Ethernet/IP/TCP/UDP capture audits. QEMU alone may
preload the archived proxy-buffer shim for the zero-window peer. Six ordinary
empty listeners exercise contention while unrelated IFGET/ping/DNS progress;
host campaigns establish full credit/slab saturation. Ordinary unanswered
CONNECT, zero-window pipeline SEND and STOP/CONT/KILL retain bounded cleanup.
Only fresh installed NVMe regular-file copies are attached. Saved argv allowlists
reject alternate storage. Baselines are hashed; config bytes, root:root 0644
metadata and Linux e2fsck are checked after clean boots. Carrier cases use a named
native-Linux QMP socket removed after bounded QEMU reap. Absent-NIC cases make no
owner-execution or successful network-configuration claim.

Actual staged functions are extracted with only privileged IRQ instructions
adapted for pthreads. ASan/UBSan and separate TSan exercise eight parents and
32,002 exact releases/cancellations. Actual terminal code exercises four
consumers and 16,384 exact bytes. Actual VFS/runfs/immutable-byte checks exercise
four readers over 512 exactly-once records each. Mounted EXT4/VFS host checks use
512/4096-byte sectors and four shared readers, plus existing root/exec/cache,
permission, rollback, reclamation and filesystem checks. These are host adapters,
not IRQ/real scheduler/physical evidence.

## Retained attempts and preservation

Earlier Phase 2C preservation qualifications, the stale sudo mock, and the
unexplained earlier Phase 2D listener timeout remain historical as originally
recorded. Nothing in this phase relabels those manifests.

Phase 3 retains the initial missing terminal credential-link dependency, the
Python HTTP-server name collision, long terminal-echo observation timeout,
hostfwd targeting the pre-ifup guest address, interleaved UART substring checks,
an inspection-time DrvFS sharing failure, an over-specific hexadecimal observer
assertion, and development-source guard failures. Two six-listener attempts hit
the unchanged TIME_WAIT reservation limit after earlier completed connections;
the fixture now runs contention earlier and reports retained TIME_WAIT slots.
Host preparation failures retain missing-header/host-WNOWAIT adapter diagnostics.
Original logs/manifests are preserved under `build/network-smp-phase3`.

During the follow-up campaigns, three operational failures occurred and are retained:
1. Stuck `--link` run (`gate-_sm95vhn`): created 14:28, no boots completed, process
   killed with SIGKILL (-9) after encountering host port exhaustion/conflicts.
   Diagnosed: port 53 was held by a stale background DNS peer (PID 32318) and port
   19000 was held by a leftover QEMU process from an interrupted run.
2. Stale DNS peer (PID 32318) holding `127.0.0.1:53` was reaped.
3. Leftover QEMU process holding `127.0.0.1:19000` causing QEMU launch error
   `Could not set up host forwarding rule` was cleaned via `wsl --shutdown`.
Following this cleanup, fresh carrier recovery ran to full completion in `gate-03audutx`.

The source started at `7aeb0d2`. The unrelated credential-review documentation
commit `cab18b6` arrived concurrently and is preserved. Snapshots contain exact
hashes; HEAD alone is not a build identity. Source-preservation reconciliation
(`build/network-smp-phase3/reconciliation/manifest.json`) confirms that kernel
sources are byte-for-byte identical across all campaigns (`gate-qy268izh`,
`gate-jlsdsoi7`, `gate-lf19d306`, `gate-k8oqd7oe`, `gate-porp7eyz`, `gate-03audutx`)
and match the working tree with zero drift.

## Final evidence index

| Campaign / Gate | Scope | Result | Details |
| --- | --- | --- | --- |
| `gate-qy268izh` | e1000e matrix (12 boots) | **PASS** | BIOS/UEFI × SMP=1/4/8, boots 0/1; AP entry confirmed for production commands; exact HTTP/echo/listener/UDP streams; CPU0 owner observed; e2fsck clean. |
| `gate-jlsdsoi7` | e1000 matrix part 1 (3 boots) | **PASS** | BIOS SMP=1 boots 0/1, BIOS SMP=4 boot 0; AP entry confirmed; e2fsck clean. |
| `gate-lf19d306` | e1000 matrix part 2 (8 boots) | **PASS** | BIOS SMP=4 boot 1, BIOS SMP=8 boots 0/1, UEFI SMP=4/8 boots 0/1; full AP entry masks; e2fsck clean. |
| `gate-k8oqd7oe` | e1000 matrix part 3 (2 boots) | **PASS** | UEFI SMP=1 boots 0/1; clean completion; e2fsck clean. Complete e1000 matrix 12/12 PASS. |
| `gate-porp7eyz` | Absent NIC (4 boots) | **PASS** | BIOS/UEFI × SMP=4 boots 0/1; Ring 3 EIO on all network tools; zero owner execution (`cpus=0x0 steps=0x0`); clean shutdown; e2fsck clean. |
| `gate-03audutx` | Carrier recovery (4 boots) | **PASS** | BIOS/UEFI × SMP=4 boots 0/1; link drop (`set_link down`), carrier-down EIO, link restore (`set_link up`), ping + exact 9th transfer pass, AP caller coverage, e2fsck clean. |
| `gate-_sm95vhn` | Retained failure (0 boots) | **FAIL** | Stuck link attempt killed at 14:30; port conflict on 53 and 19000; retained historical failure. |
| `nmi-diagnostic.json` | NMI regression | **PASS** | BIOS & UEFI, 104 exact-boundary NMIs, 16 alias round trips, both recovery checks. |
| `sudo-guest.log` | Sudo regression | **PASS** | 10/10 sudo permissions and execution policies pass under SMP. |
| `time-regression.json` | CPU accounting regression | **PASS** | 4 guest regressions (BIOS/UEFI × SMP=1/4); per-process utime/stime accounting isolated. |
| `host-mzr2kwzl` / `vfs-tsan` / `terminal-tsan` | Host sanitizers | **PASS** | Staged list (8 parents, 32,002 children), terminal (4 consumers, 16,384 bytes), shared offset (4 readers, 512 records) under ASan/UBSan and TSan. |
| `tests/ext4_mount_host.c` | Mounted EXT4/VFS | **PASS** | 512 and 4096-byte sectors, shared readers, e2fsck verification. |
| `release-vtj0euw0` | Strict isolated release | **PASS** | Clean kernel/initramfs snapshot; private symbols/strings absent; static ledger 557,238 / 589,824 bytes (< 576 KiB); release media unchanged. |
| `reconciliation` | Source reconciliation | **PASS** | Zero kernel source differences across all test campaigns and current tree (`manifest.json`). |

## Scope and Limits

- **No physical hardware acceptance:** Bare-metal testing on the Dell Latitude 5590
  is deferred to Phase 4.
- **No security/entropy audit:** Hardware entropy assurance and network ISN quiet-period
  properties remain unchanged and conditional.
- **No throughput measurement:** Performance benchmarking, throughput bounds and idle-CPU
  efficiency are deferred to Phase 5.
