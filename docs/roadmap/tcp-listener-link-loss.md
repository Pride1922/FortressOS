# Persistent TCP listener failure after carrier loss

2026-10-28: fixed and verified. This closes the wired host regression retained
during the [SYS_WIFI owner work](wifi-cpu0-owner.md). No commit or push made.

## Cause and fix

This was a kernel bug, not a weakened or obsolete host assertion.
`net_tcp_tick` latched `SYSCALL_EIO` for started connections only when
`!e->listening`. A listener therefore failed a blocked ACCEPT while the common
socket service reported offline, but lost that failure when the link recovered.
The next blocking ACCEPT on the same descriptor could wait indefinitely.

Remove that listener exclusion in the existing CPU-0 endpoint sweep. The same
error field and existing event publication now apply to listeners too. The
resulting behavior matches the established link-recovery contract:

- The old listener stays failed after replug, including `POLLERR` and ACCEPT
  `EIO`. New SYNs cannot enter its queue.
- Repeated offline ticks do not republish the same error indefinitely.
- Generation, queue and retained child references remain intact until normal
  close/transaction cleanup. No forced pin revocation or early child reuse.
- A caller-held ACCEPT pin remains valid until its decision; provisional
  adoption after link loss resolves to `EIO` and rolls back safely, including
  when the link returns before the owner processes that decision.
- Close the failed listener and open/bind/listen again to recover. Fresh
  listeners can reuse the port and accept a new child normally.

No new lock, state allocation, timeout, syscall ABI, owner, affinity policy,
NIC programming, IRQ path, timer hook or DMA behavior. Existing error precedence
and descriptor/reference cleanup are preserved. The protected CPU-0 and retained
ACCEPT contracts are unchanged.

## Actual-code host verification

- Before: `make test-net-tcp-socket-host` reproduces the bounded owner-wait
  assertion. Retained output: `build/listener-link-loss/before-host.log`.
- After: `make test-net-tcp-socket-host` PASS, including its runtime transaction
  guard, persistent error/poll wake checks, rejection of replug SYNs and fresh
  port reuse. The error assertion was strengthened, not removed.
- `make test-network-smp-phase2d-host`: ASan/UBSan and separate TSan PASS.
  Existing listener/client tests plus four new cuts: before owner claim, after
  an offered child, while caller-pinned, and after commit publication. Tests
  require pinned-decision acknowledgement, provisional rollback, restored file
  reference counts, empty generic handoff state and fresh port/child reuse.
  Evidence: `build/network-smp-phase2d/host-jy78g4or/manifest.json` and logs.
- `make test-network-smp-phase2c-host`: ASan/UBSan and TSan PASS,
  `build/network-smp-phase2c/host-5tbrhixd/manifest.json`.
- `make test-wifi-owner-host`: ASan/UBSan and TSan PASS, 4,800 concurrent calls;
  evidence `build/wifi-owner/host-rh09qgd7/manifest.json`.
- `make test-net-socket-host`: actual UDP/socket ASan/UBSan regression PASS.

An initial new test expected an unpinned copied offer to complete without a
caller decision or lease expiry. It was corrected to resume the caller and
publish the normal adoption decision. No production pin/lease rules changed.

## QEMU and production-build verification

`python3 scripts/test_net_listener_link.py --workspace
/mnt/c/Sources/FortressOS/.codex-remote-attachments/ext4-phase9/guest-workspace-kxr51xvp`
passed **4/4 BIOS/UEFI × SMP=1/4 boots**, with ten complete carrier-loss/replug
cycles across those boots. The default command creates a fresh private snapshot;
the workspace option reuses its already instrumented snapshot.

The probe uses actual Ring 3 socket, poll, NETCTL and descriptor calls on every
configured CPU. Test-only CPU/registered-owner observers exist in the copied
snapshot only. QMP toggles the emulated e1000e carrier. The same old descriptor
must fail blocking ACCEPT again after recovery, then a fresh listener must
accept and echo **exactly** `REPLUG-FRESH\n`. The owner mask must remain 1.
Every boot validates its final argv: ISO and private OVMF variables only, no
data disks. Tests assert successful probe exit and clean guest shutdown.

Evidence: `build/listener-link-loss/qemu-lcjrlt0l/manifest.json`, final argv,
serial logs and QMP carrier-event records. The first runner attempt
`qemu-gx6iw_ja` exposed a test-probe ABI error: its three-argument inline syscall
helper did not clear R10, which ACCEPT interprets as flags. The helper now sets
all unused syscall arguments to zero; failure messages include source lines.
The corrected BSP check passed in `qemu-o7hz4bp3` before the full matrix.

`python3 scripts/verify_network_smp_phase3_release.py` PASS: ordinary strict
kernel/initramfs/ISO/image build, private listener/Wi-Fi hook exclusion, static
ledger **560,543 / 589,824 bytes**, and unchanged shared ISO/image hashes.
Evidence and updated ordinary image:
`build/network-smp-phase3/release-nuaxyzo9/manifest.json` and `fortress.img`.

This is emulated carrier and scheduling evidence, not a new physical Intel PHY,
DMA or cable acceptance claim. The existing physical/network findings remain
separate. There are no remaining failures in the targeted host and guest gates.

## Preservation

The 43 pre-existing modified/untracked files from the desktop and Wi-Fi work
were archived before editing under `build/listener-link-loss-preservation/`.
The transport fix and new tests are separate changes. Existing implementation
files remain intact; shared build/doc files receive only this follow-up's
target, exclusion checks and evidence links. The archive and comparison report
preserve the exact starting working tree. Nothing was reset, stashed, committed
or pushed.
