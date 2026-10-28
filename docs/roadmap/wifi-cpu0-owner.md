# SYS_WIFI CPU-0 ownership — issue #76

Date: 2026-10-28. Implementation and mocked ownership verification complete;
Intel radio acceptance remains pending on Dell. No commit or push performed.

## Boundary and operation audit

`syscall_dispatch` now calls `net_wifi_syscall`. Every command, including BSP
calls, uses the existing generic handoff pool and registered CPU-0 service.
There is no caller-side WLAN fast path, alternate worker, IRQ networking or
production process-affinity change. Boot initialization remains on the BSP,
before worker publication. A device-less stack can start the same owner;
`net=none/off`, raw ring selftests and worker-creation failure still prevent
normal management admission rather than falling back to direct hardware work.

| Operation | Caller | Registered owner |
| --- | --- | --- |
| SCAN start/collect | Validate writable 776-byte ABI; copy only bounded capacity, clamp to 16 | Lazy initialization, scan start/probe TX or bounded RX collection; zero-initialized copied result |
| CONNECT | Validate/copy fixed request, preserve existing termination/truncation rules | Validate SSID/PSK, BSSID selection, PMK cache/derivation, context setup and initial authentication; wipe request credentials |
| DISCONNECT | Preserve existing ignored argument/size semantics | Management deauth TX, session invalidation and hardware teardown |
| STATUS | Validate output and copy completed result | Pure lock-protected snapshot: no initialization, RX drain, scan-completion hook or state mutation |
| LOAD_FW | Validate 1..4 MiB, acquire exclusive upload credit, allocate/copy stable kernel bytes | Existing Intel loader/parser/upload with its original hardware workarounds and DMA quarantine; release copied bytes only after completion/abandonment |

Authorization was audited, not delegated to GUI checks. The original kernel
SYS_WIFI handler has **no capability or UID gate for any command**; this patch
preserves that public access policy. Actual guest tests run as unprivileged
operator and cover all commands. Root display authorization and PKG admission
are unchanged. Adding a Wi-Fi administrative capability policy is separate work.
No credential, passphrase or firmware contents are logged or put in test argv.

## Copied values, cancellation and locks

Four static records consume 3,305 writable bytes including the values lock and
firmware-credit flag. They contain fixed ABI values and generation-tagged generic
tokens. At most one heap allocation of at most 4 MiB holds an upload. Saturation
returns EAGAIN; allocation failure returns ENOMEM; absent/stopped service returns
EIO. Existing invalid command/size/range precedence is preserved.

Admission uses the caller TCB's existing handoff registration. After admission
waits, ranges are revalidated; preparation and copying contain no signal/wait
boundary. No user pointer, user page table or caller stack crosses to the owner.
Outputs are revalidated again after owner completion. Generic retained records
cannot recycle until owner cleanup wipes the values, frees copied firmware and
calls `handoff_cleaned`. Generation matching fences reuse. Firmware input is
copied into the existing driver's DMA staging; this allocation is not loaned
directly to DMA and does not replace its quarantine policy.

Signals abandon interest through the established generic transport. Existing
thread-exit/reaper hooks use the same registration, including death while waiting
for admission or a result. Cancellation observed before execution prevents the
action. Once the owner starts a bounded global action, it may complete even if
the caller exits; EINTR is not a promise to roll back an accepted association or
firmware upload. Caller death never invokes hardware teardown on an AP. A
successfully initiated connection progresses independently of caller lifetime.
The manager's existing active/cached-key policies are retained; new transaction
copies are wiped immediately after connect execution or on abandoned cleanup.

`wifi_handoff_values` is ordinary rank 1. It never nests with handoff, WLAN,
process, scheduler, filesystem or other rank-1 locks. Drop it before all WLAN,
driver, heap, user-copy, wait and wake operations. Cleanup does heap work outside
exclusion. Existing WLAN rank-1 to driver rank-2 raw TX paths remain; no lock
rank or switch/sleep contract changes. Host lock adapters check rank ordering;
QEMU uses the actual kernel lock checks and scheduler.

## Approved hardware-timing change

The code contained a synchronous 600,000-iteration RX pump restored for Intel's
session-protection on-channel window, despite the documented nonblocking API.
The user explicitly approved replacing that workaround with bounded owner turns
before implementation. Connect now returns after setup/initial TX. During
authentication, association and WPA2 exchange, the CPU-0 worker stays runnable,
processes at most 64 raw frames per WLAN service turn and yields between turns.
Retries and the six-second fail-closed timeout remain autonomous. Status does
not progress the connection. Scan completion moved into the owner service turn.

This preserves the protected sole-owner architecture. It does **not** establish
that cooperative scheduling meets the real Intel on-channel timing window.
Firmware upload and PBKDF2/setup still occupy bounded synchronous owner time;
the worker is not a fully asynchronous firmware loader. Existing roaming,
Ethernet preference and cable handover policy are unchanged.

## Verification performed

All paths below are relative to the repository and are ignored test artifacts.

- `make test-wifi-owner-host`: ASan/UBSan and separate TSan PASS. Actual
  SYS_WIFI adapter, generic handoff/control dispatch, WLAN manager, MAC and
  crypto; mocked CPU/scheduler/VM/heap/radio adapters. 4,800 concurrent calls,
  four-record exhaustion, firmware exclusion/allocation failure, pure status,
  scan/connect/disconnect, cancellation/death cleanup, output revalidation and
  wiped firmware before free. Radio hooks require CPU 0 and the registered TID.
  Final evidence: `build/wifi-owner/host-4o7apzpk/manifest.json`.
- `python3 scripts/test_wifi_owner.py`: **6/6 PASS**, BIOS/UEFI × SMP=1/4/8.
  A private copied workspace contains explicitly mocked radio hooks, an inert
  pending gate and a private observer; ordinary production sources/media are
  not instrumented. Real Ring 3 SYS_WIFI calls run on every configured CPU.
  All five commands, scan/status during setup, autonomous timeout, STOP/INT/KILL
  (including a copied connect request), four pending records/fifth-call EAGAIN,
  concurrent producers and exact generic cleanup are checked. Radio CPU mask
  must be 1, command mask 31 and caller mask `(1 << cpu_count) - 1`. Ping/DNS
  remain usable. ISO-only guests, private OVMF variables, no data disks.
  Evidence: `build/wifi-owner/qemu-j5kaa4xm/manifest.json` and serial logs.
- Wi-Fi/MAC/crypto/WPA2/fault/stack gates PASS: `make test-wifi-host`,
  `test-wifi-fault-host`, `test-wlan-mac-host`, `test-wlan-crypto-host`,
  `test-wlan-wpa2-host`, `test-wifi-stack`. Fault tests now prove nonblocking
  connect and owner-driven RX instead of requiring the removed synchronous pump.
  New owner functions fit 1,024 bytes; the existing WPA2 exception remains.
- Owner-safe wired/control regression evidence: `test-network-smp-phase2a-host`
  ASan/UBSan/TSan PASS (9,600 concurrent calls); `test-net-ifconfig-host`,
  `test-net-eth-host`, `test-net-socket-host` and `test-poll-host` PASS. Ethernet
  host adapters now supply polymorphic link callbacks and expect the intentional
  device-less control owner. A missing stable time-channel adapter was added to
  the shared network host shim; no production timer change was made.
- Ordinary production ISO wired smoke: BIOS/UEFI × SMP=1/4 **4/4 PASS**,
  ping, DNS and deterministic HTTP download with byte-identity SHA-256 check.
  Evidence/command: `python3 build/wifi-owner/wired_smoke.py`,
  `build/wifi-owner/wired-smoke/manifest.json` and serial logs.
- Desktop: `make test-compositor-host` **31/31 PASS**; actual GUI acceptance
  **4/4 PASS**, `test_desktop_apps.main()` using the ordinary private snapshot
  through its existing Guest adapter (BIOS/UEFI × SMP=1/4). Updated no-adapter
  behavior, settings/restart persistence, timezone and retained rendering checks.
  GUI logs/images: ordinary snapshot's `build/desktop-apps-*.log` and PPMs.
- `python3 scripts/verify_network_smp_phase3_release.py`: ordinary strict
  kernel/initramfs/ISO/image build, test-hook exclusion and ledger **PASS**.
  Ledger now includes Wi-Fi: **560,543 / 589,824 bytes**. Shared ISO/image hashes
  unchanged. Evidence and ordinary Dell candidate image:
  `build/network-smp-phase3/release-kcb9p6z9/manifest.json` and `fortress.img`.
  Final current production executable sections match that verified snapshot
  for all five changed objects; the later net.c host-only adapter update and
  documentation changes do not change those sections. Reconciliation:
  `build/wifi-owner/production-text-reconciliation.json`.

## Retained failures and remaining acceptance

**Follow-up:** the listener regression described below was subsequently fixed
and its host ASan/UBSan/TSan and BIOS/UEFI × SMP=1/4 BSP/AP gates pass. See the
[listener link-loss fix and exact evidence](tcp-listener-link-loss.md).
The original failures below remain historical evidence, not current blockers.

At this Wi-Fi checkpoint the legacy TCP/listener host regression was **not green**. Initially it cannot
link without `time_get_sleep_channel`; with the stable test adapter it reaches
the old-listener-after-link-loss/replug ACCEPT case and hits its bounded wait
assertion (`tests/net_tcp_owner_host.c:76`). Both `test-net-tcp-socket-host` and
`test-network-smp-phase2d-host` retain this failure. The fixture and 18 linked
transport/test implementation files match HEAD; they do not link the changed
Wi-Fi/control implementation. Evidence:
`build/network-smp-phase2d/host-wqucupn2/asan-ubsan.log` and
`build/wifi-owner/wired-failure-baseline-identity.json`. No transport workaround
or relaxed assertion was applied. This failure is separate from the successful
Wi-Fi ownership matrix and ordinary wired TCP smoke, not waived by them.

An initial QEMU runner failure in `qemu-ra8oq19s` was an asynchronous shell
redraw mistaken for command completion during STOP. The runner now waits for
the echoed command boundary and actual exit status. Its failure artifacts remain.
The first stack check also exposed a 1,040-byte compound-literal temporary;
in-place record initialization removed it before the final gates.

Dell acceptance still must cover actual Intel scan, firmware upload, BSSID
selection, connect/WPA2 completion, repeated status, disconnect, failure/retry,
roaming and Ethernet/Wi-Fi cable handover while GUI/CLI callers run on APs.
QEMU establishes ownership, copied lifetimes and scheduling, not RF, real DMA,
firmware timing or physical connectivity. Existing key-cache and radio
cryptographic policies have not received a new security audit in this work.

## Preservation

Before editing, all 24 pre-existing uncommitted desktop files were archived in
`build/wifi-owner-preservation/before.zip`; original status is alongside it.
Sixteen remain byte-identical. Eight have only targeted follow-ups for the SMP
guard, tests/build and ownership documentation. The original desktop 0.2.0
implementation, package work, assets, version metadata and authorization remain.
`comparison.json` and `desktop-followup.diff` reconcile those edits against the
original working tree, rather than against HEAD. No reset, stash, commit or push.
