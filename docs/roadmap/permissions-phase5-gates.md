# Permissions Phase 5 hardening and automated acceptance

2026-10-10. Continued locally from `700ba89`. Automated work complete;
**Dell Latitude 5590 physical acceptance PENDING USER RESULTS**.
At the automated acceptance checkpoint there were no commits, GitHub changes,
pushes, physical device access or flashing. The user subsequently requested a
local commit of all remaining changes; physical acceptance remains pending.
The two initially untracked Phase 2 logs are byte-identical, with baseline copies,
and are included unchanged in that local commit.
Prior bin artifacts, including the intentionally old raw image, are retained.

Final source hashes, the complete local patch/status, preservation checks and
campaign artifact hashes are recorded in
`build/permissions-phase5/artifact-index.json` and `preservation.json`.
The index includes failed campaigns and retained guest fixtures; it excludes
itself and unrelated historical campaigns. Individual manifests remain the
authority for exact argv, outcomes and verification scope.

## Delivered policy and audit

All 71 dispatcher cases match the syscall ABI and have explicit authority
classifications in `build/permissions-phase5/syscall-audit.json`.
`scripts/audit_permissions_phase5.py` checks inventory drift. The existing VFS
and trusted-call inventory retains 173 classified sites, zero unclassified.
Reviewed indirect paths include authoritative filesystem callbacks, inherited
descriptors, runfs executable snapshots, private child construction and unwind,
process/group/staged/terminal signals, child/session control, low-port UDP/TCP
bind, protocol worker/manager calls, power freeze/sync and global configuration.

Demonstrated gaps fixed:

- SYS_DMESG now requires **effective UID zero**, including zero-length probes,
  before output validation/read or on-demand profile/log publication. Root with
  dropped capabilities remains eligible; non-root capabilities do not replace
  root identity. Denial leaves user bytes and log/profile counters unchanged.
- NETCTL_IFSET now requires **CAP_SYS_ADMIN** before input validation and
  global configuration changes. Effective UID zero alone does not restore a
  dropped capability. The existing BSP/affinity fence still precedes the
  subcommand handler; unpinned/AP callers retain EOPNOTSUPP. IFGET, finite
  ping/trace and ordinary high-port networking remain public.
- TarFS checks the entire immutable USTAR archive before namespace publication.
  Checksums, strict magic/version/numeric fields, bounded paths/components,
  parent traversal, entry types, directory payload, payload bounds, complete
  terminators and zero tail are validated. Malformed later headers cannot
  publish an earlier privileged image. Population allocation failure still
  fails boot; no transactional VFS boot allocator is added.

Existing signal checks use current actor/target credentials and publish under G;
staged/group/signal-0, partial group permissions and trusted kernel signals keep
their contracts. Power requires CAP_SYS_BOOT before effects; low-port bind
requires CAP_NET_BIND before socket mutation. Process listing intentionally
shows all processes. Self operations, child/session control, public system and
geometry queries, sync, single-console layout, aggregate lock metrics and
self-owned profiling have explicit classifications; no blanket root check is
invented for these interfaces. Device DAC/CAP_SYS_RAWIO remains additional to
raw-write, durability, GPT, DMA and internal-NVMe exclusions.

The protected documentation still mentions TCP's old 120-second quiet period,
while base history `056a12d` replaced it with cryptographic ISNs. This pre-existing
documentation/code discrepancy was observed; Phase 5 changes no TCP protocol,
timer, scheduler, boot assembly, storage driver, filesystem journal or lock ranks.

## Final host evidence

`python3 scripts/run_permissions_phase5_host.py` PASS. Exact argv, sanitizer
environment, source hashes before/after, unchanged-source assertion, binary
hashes and log hash: `build/permissions-phase5/host-manifest.json`.
Output: `final-host-verified.log`; prior aggregate logs remain retained.

Executed: `make test-host test-perm-phase5-host test-perm-db-host
test-perm-spawn-host test-perm-creds-host test-perm-syscalls-host
test-perm-runfs-host test-perm-privileges-host test-perm-matrix-host
test-perm-fs-host test-net-ifconfig-host test-usb-mount-host test-xhci-bot-host`,
then runner tests and both source audits. All return zero.

- 30,000 deterministic USTAR header/length mutations (with and without repaired
  checksums), including 20,908 rejected inputs before publication in the first
  focused run; final exact output is retained. Explicit long path/component,
  parent traversal and malformed-later-header vectors reject with no VFS effects.
- 10,000 mutations/truncations each for passwd, group and shadow; rejected
  parsing leaves the complete database unchanged. Existing duplicates, bounds,
  NUL/control, aging restrictions, hash-vector and host libcrypt gates remain.
- 10,000 malformed spawn options targeting /bin/sudo reject before construction;
  injected failure at action-path/argv/env allocation cleans up all heap objects.
  A construction error cleans copied arguments; actor bytes remain unchanged.
  The construction boundary is an explicitly labelled host adapter. Actual
  credential policy retains 32,768 set-ID/nosuid/root/drop combinations,
  full-width IDs, secure fd destinations and the verbatim stack/AT_SECURE builder.
- Actual dmesg and IFSET handlers check denial invariance and positive admission,
  non-root capability versus root-with-dropped-capability distinctions and
  invalid output/input ranges. Configuration/profile/log callbacks are mocks;
  no physical network, IRQ or scheduler proof follows.
- Existing independent DAC/creation/chmod oracles, credential publication,
  runfs interleavings, signal/power/bind, sudo/password/no-echo/wiping, login,
  secure descriptors, shell/pipe/EXT2/stream/disk regressions pass.

`make test-perm-filesystems-host
PERM_FIXTURE_DIR=build/permissions-phase0/inodes-l920f8r4` PASS;
`filesystems.log` and `build/permissions-phase2/enforcement-6bs2q5dv` retain
actual EXT2 128/256-byte inode and journaled EXT4 denial/interleaving tests,
complete-image/allocation/staging invariance, Linux fsck/stat checks and
4,088 metadata/content atomic crash cuts. Chmod/chown/write/truncate have
77/77/86/271 events across four persistence models. All sources were hashed
before compilation and checked afterward. No new sector-tear/physical claim.

## Disposable BIOS/UEFI evidence

| Campaign | Final evidence | Result |
| --- | --- | --- |
| Expanded `make test-sudo` | `qemu-sudo-attempt3.log`; `build/permissions-phase4/sudo-mq_d1iri/manifest.json` | 10/10 password SMP=1/4, non-wheel, passwordless and dropped-cap controls; secure descriptors/actions/staging/environment and RO USB nosuid; complete image unchanged |
| `make test-login` | `login-regression.log`; `build/permissions-phase3/login-ql1ed0qm/manifest.json` | 9/9 password, passwordless, test bypass and normal-kernel login=0 rejection |
| `python3 scripts/test_perm_phase2_regression.py` | `phase2-regression.log`; `phase2-7m4ph1az`; snapshot `guest-workspace-h0ii7sld/build/permissions-phase2/guest-_vd2qosu` | 4/4 BIOS/UEFI × SMP=1/4 Ring 3 DAC/metadata/capabilities/root tools on disposable journaled USB |
| Exact normal raw image | `image-attempt4.log`; `image-d8a008c6/manifest.json` | 8/8 boots: two boots × BIOS/UEFI × SMP=1/4; ownership/modes/bytes persist; independent Linux fsck/debugfs/dump after each boot |

The expanded Ring 3 set-ID probe performs 256 malformed option/action attempts
per admitted operator case, comparing heap/table counts and preserving caps.
It checks dmesg buffer invariance/zero-capacity denial, power denial, public
process listing and unchanged network fence. A secure root child with dropped
caps can still read dmesg. Sudo dmesg succeeds. Unpinned NETCTL rejection is
not claimed as an admitted IFSET capability proof; that handler has host gates.

The exact-image campaign uses normal login with no test hooks, one disposable
raw USB file per firmware/CPU case, two orderly shutdowns and independent Linux
fsck, journal information, exact dumps, mode and full-width owner audits after
each boot. It retains UID/GID UINT32_MAX/UINT32_MAX-1 through debugfs inode
inspection; this is on-disk full-range evidence, not a Linux mounted-UID claim.
The physical checklist uses valid high IDs 12345678:87654321 for `ls -ln`.
The real root-owned 4755 USB copy of sudo fails privileged installation checks,
and the operator parent remains unprivileged. Only disposable image files and
paired OVMF code/disposable vars are attached; exact argv preflight rejects
extra storage/blockdev/snapshot arguments. Every QEMU PID is terminated/reaped.

The Phase 2 runner now retains its ISO/OVMF/data disks and permits login bypass
only with its explicit regression environment plus LOGIN_TEST=1 test build.
PERMISSIONS_TEST=1 is an explicit build switch for the fixed self-drop syscall.
Both gates are absent in the delivered normal kernel.

## Fresh image and provenance

Normal build: `make -j4` in isolated `guest-workspace-hb74hjxa`;
`production-build.log`, its workspace manifest and retained production inputs.
`scripts/prepare_permissions_delivery.py` verifies primary/backup GPT CRCs,
embedded kernel/initramfs hashes, normalized sudo/database metadata, locked root,
empty operator hash, clean Linux EXT4 fsck, absent credential test syscall/login
escape and 293 current runtime source hashes against the production snapshot.
The snapshot's Makefile predates the added unused PERMISSIONS_TEST switch;
its exact build input is retained. Normal flags and all runtime bytes match.

Fresh image: `bin/fortress.img`, also retained as
`build/permissions-phase5/delivery/fortress-permissions-phase5.img`.
Size 136,314,880 bytes. SHA-256:

```
7c6a70d1a50252efed9b5f07e44c5cb210ae94f5ecd7fdcea20a60951c03b53f
```

Kernel SHA-256:
`d62fc1edb414078d3c36abddabd13114bb2d7ef3b2e8eb3e28e77f8d1dd81738`.
Initramfs SHA-256:
`e8579b26ed5a9507b380ddc59023c82e42c4ff9649217b3ecbfe02b56a24591f`.
Data PARTUUID: `705536e3-a337-4b30-a0cf-0f316ebda29c`.
Delivery manifests retain prior and current artifacts, embedded config, symbols,
Linux audit and SHA256SUMS. The old raw image
`d130bec6d0b396a968135fb2132b9543a8774c8889b24fce464f88a10cd68c28`
is preserved as `baseline/prior-fortress.img`; it is not the current image.
The verified shipped image is byte-identical to the source booted in the
exact-image campaign; the shipped copy was never attached to QEMU.

Locked root, passwordless operator/login warning, temporary HOME, optional
explicit build-time operator hashes, passwordless sudo/warning and no caching
remain unchanged. No persistent root/home/database mutation is added.

## Retained failures and limits

Focused compile attempts encountered a misleading-indentation warning in the
new database loop and a host adapter return-type/string-function declaration
mismatch. Corrected before final gates; `host-attempt1.log` retains the latter,
and the initial database compiler output is in the chat tool transcript.
`host-attempt2.log` is the first passing focused campaign.
Sudo attempt 1 failed compilation due to a missing power constant include;
attempt 2 expected EPERM from an unpinned process before accounting for the
BSP fence. Both source snapshots, build/UART logs and manifests remain retained.
The runner now waits for a complete diagnostic line before aborting a probe.

Exact-image attempts 1 and 2 waited for an incomplete locked-root login or
expected an external-process status from the shell's dmesg builtin; they were
interrupted with SIGINT, invoking the runner's finally cleanup. Attempt 3
used the child wrapper for forbidden mkdir and failed with status 2. Attempt 4
uses the actual elevated shell for mkdir/shutdown and passes. Failed disks,
argv/OVMF/UART/stderr/manifests are retained and excluded from acceptance.
No tested policy or hardware workaround was weakened to accommodate a failure.

Finite deterministic fuzzing is not an exhaustive parser/privilege proof.
Host adapters do not prove IRQ/scheduling behavior; SMP=4 cases do not certify
arbitrary AP Ring 3 credential races or cross-core network ownership. TarFS boot
population remains fatal on allocation failure. Existing bounded runfs/parser,
nonjournaled EXT2, ordinary mutable nosuid executable-read, shared-offset and
shared-user-thread limitations remain. The shell history path is still
/mnt/.fortress/history and may warn about auto-save under operator DAC on a
fresh root-owned USB; HOME remains temporary and is not silently redirected.

The [Dell checklist](permissions-phase5-dell.md) covers boot/login, sudo, USB
nosuid, clean reboot ownership/bytes and independent Linux ownership/integrity
audits on explicit disposable media. Physical acceptance remains pending until
the user supplies results; diagnoses and closeout must cite those observations.
