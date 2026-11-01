# Package session acquisition: filesystem exclusion

2026-11-01. The user confirmed physical rot13 1.0 → 2.0 upgrade and reboot
persistence PASS. Their terminal photograph also shows a first acquisition
failure (`installation backend unavailable or session denied`) followed by a
successful second attempt. That generic message discarded the syscall error,
so the exact first laptop error cannot be recovered from the photograph.

## Finding and change

`PKG_ACQUIRE` calls `pb_control`, which obtains `ext4_pkg_status` before creating
the retained writer session. The package service's execution credit serializes
package calls, but ordinary VFS operations do not take that credit. The status
callback used `px_lookup`, `e4_inode`, `e4_scan`, extent mappings and record reads
without the existing `e4_lock`. Those helpers share scratch/sector buffers,
mapping state and metadata cache with ordinary reads/writes. They also issue
storage requests through the same filesystem-owned transport. Another file
operation can therefore overlap package status and produce a failed read or
corrupt shared in-memory I/O state.

The callback now allocates its private snapshot workspace before taking the
existing filesystem lock, holds the lock across lookup/mapping/record capture,
then releases it before decoding and freeing the workspace. The decoded record
also moves off the kernel stack (compiled status frame: 256 bytes).
Adjacent raw reads in database existence/UUID checks, installation resource
capture and `px_prepare`'s initial record read receive the same exclusion.
Read errors propagate; database creation does not treat a failed lookup as an
absent database, and failed UUID reads do not publish an all-zero identity.

This restores the existing filesystem contract. Lock ranks, IRQ/scheduling,
driver/DMA policy, session ownership, credential checks and syscall ABI stay as
implemented. Process authorization and package-owner locks are released before
filesystem work. Allocations and credential callbacks are not wrapped in the
filesystem lock. A failed mutation is never automatically retried.

The CLI preserves the acquisition syscall error and distinguishes busy, denied,
unsupported backend, invalid/unreadable state and memory failure. It prints
the signed kernel error number and performs one acquisition attempt. Explicit
source install, cached install and upgrade all use these diagnostics.

## Evidence and qualifications

- `build/pkg-status/host-caa9pg5z/`: deterministic before-fix regression,
  `EXPECTED_FAILURE`, `PKG_STATUS_UNPROTECTED_READ`. The actual callback reaches
  the host disk-read oracle without filesystem exclusion.
- `build/pkg-status/qemu-sv9qzwfd/`: old normal kernel, live QEMU failure during
  initial package setup, before the deliberate writer probe. It reports
  `NVMe I/O CQ CID mismatch`, then quiesces the controller. The run's original
  `FAIL` is retained; no filesystem-recovery acceptance is inferred.
- `build/pkg-status/qemu-fz1uisq7/`: intermediate fix, BIOS acquisition stress
  passes; UEFI installation preparation returns EIO. This exposed the remaining
  unlocked `px_prepare` record read. Its original `FAIL` remains.
- `build/pkg-status/qemu-id810t3y/`: final normal kernel, BIOS/UEFI SMP=4 PASS.
  Each boot installs through the real CLI, then performs 600 acquire/release
  pairs concurrently with 160 history-style writes by a separate process.
  Both report `failures=0 first=0`, return valid status and shut down cleanly.
  Linux `e2fsck -fn` passes. Only private regular-file NVMe disks are attached.
- `build/pkg-status/host-zz4_8_4h/`: final host ASan/UBSan PASS, including
  database creation under the read-lock oracle, 600 concurrent status queries
  against 150 file writes, I/O/OOM cleanup and Linux filesystem audit.
- `.codex-remote-attachments/pkg-ordinary-z_i69zq7/`: actual ordinary installation,
  verified reinstall/no-op, signature/artifact rejection and I/O failure PASS.
- `build/pkg-upgrade-qemu/gate-w78qd6jy/`: final kernel/CLI BIOS/UEFI SMP=4
  upgrade, no-op and reboot PASS with byte-exact executable/package-record,
  credential/configuration preservation and Linux filesystem oracles.
- `make test-pkg-stage2-host`: PASS 21/21, including 15 injected acquisition-error
  cases across cached install, explicit install and upgrade. Each checks one
  acquisition, the exact signed error/reason, no ingestion/download, and no
  descriptor/session leak. This syscall model is frontend evidence, not disk
  or hardware acceptance.
- `.codex-remote-attachments/pkg-system-host-20261101-113810-193029/`: transport,
  retained-owner, copied-source and frontend stack checks PASS; recorded input
  preservation PASS. Compiled acquisition diagnostic frame: 112 bytes.

Final kernel SHA-256:
`c637702bd7537f581aba2950421317c7c4fd5737204a4a0bb70b9066992df6d3`.
Normal kernel/initramfs/CLI build PASS. The private status probe is absent from
the normal initramfs. Runtime package ownership and credential checks remain
covered by the transport suite; no automatic mutation retry was introduced.

The missing filesystem exclusion is a confirmed defect and a plausible cause
of the first laptop failure. The photograph alone cannot establish whether
that attempt returned EIO, EAGAIN, or a different acquisition error. The earlier physical
package-upgrade acceptance applies to the previous shipped image. The corrected
kernel/CLI are also included in the later two-slot baseline installed on the
laptop; the user confirmed its baseline and offline kernel-update/reboot checks.
The user subsequently confirmed package acquisition/rot13 upgrade and reboot
persistence on that updated laptop PASS. This is user-reported physical acceptance;
no new terminal trace or original acquisition-error code was supplied. The existing
flashable installer receipt is preserved separately from this later laptop result.

Reproduce with `make test-pkg-status-host` and `make test-pkg-status-qemu` inside
the Linux build environment. The latter requires KVM and OVMF. Private probes
and temporary signing keys are excluded from normal media. Rerun ordinary
installation and upgrade/reboot acceptance after kernel changes.
