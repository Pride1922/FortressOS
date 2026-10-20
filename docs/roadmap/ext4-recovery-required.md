# EH-3 follow-up: explicit recovery-required state

2026-10-20. Adds `MOUNT_FLAGS_RECOVERY_REQUIRED=32` without changing syscall
numbers, mount-info/stat layouts, mutation ordering, barriers or lock ranks.
`disk info` prints "recovery required (stop mutation retries)" independently of
nominal RW policy.

The snapshot exposes the actual recoverable mutation fence, permanent taint or
uncertain replacement namespace under existing filesystem exclusion. Healthy
pinned orphans alone do not set it. A real failed checkpoint returns EIO/taint;
the recoverable EAGAIN case is a failed close cleanup followed by a new mutation.
Close itself has no cleanup return channel. A later mount query is a snapshot,
not an atomic explanation of a historical error. Another thread may drain or
fail between calls; absence of the flag does not authorize an unbounded retry.

The [caller contract](../plans/EXT4_DURABILITY_HEALTH_DESIGN.md#recovery-required-flag-and-caller-rules-follow-up)
requires abort on permanent failure, at most one explicit drain for a recoverable
fence, and no automatic package resume after that drain. A fresh package attempt
must revalidate its intent, objects and namespace. Non-recovery EAGAIN permits
at most one retry, only for a documented pre-I/O denial with a known progress
condition. The retry wait budget does not bound a synchronous device call.

The separate [descriptor-sync/latency review](../plans/EXT4_SYNC_LATENCY_REVIEW.md)
records why an fd-scoped API does not imply inode-only device flushing, why the
current repeated synchronous staging-write barriers matter, and what must be
measured/reviewed before changing exclusion or writeback. No SYS_FSYNC, asynchronous
writeback or latency optimization is implemented by this follow-up.

## Focused verification

- `make test-ext4-health-host`: **6/6 PASS, 42 independent Linux fsck audits**,
  ASan/UBSan, 1/2/4 KiB blocks × 512/4096-byte sectors. Actual read-fault close
  cleanup failure sets the flag; another failed drain keeps it set; a successful
  drain clears it while acknowledging the saved error once. Mutation resumes
  only after the fence clears. Healthy pins and an open replacement source
  returning EAGAIN do not set it. Actual failed flush and replacement source
  barrier failures set permanent health bits and RECOVERY_REQUIRED, with no
  metadata query I/O on the failed device. Reboot recovery clears them. The
  earlier concurrency, short-write, exhaustion and account-floor cases remain
  in this campaign. Logs/manifest: `build/ext4-health-recovery/`.
- `python3 scripts/test_disk_host.py`: **20/20 PASS**, including the new human
  diagnostic with a mocked mount-info snapshot. This CLI test is not a device
  failure test; actual mounted failures are exercised by the host suite above.
- `make test-ext4-health`: **4/4 PASS**, BIOS/UEFI × SMP=1/4, disposable
  NVMe /mnt fixtures. Ring 3 observes a healthy pinned orphan without the recovery
  bit, then the injected non-tainted final-close failure, EAGAIN on the next
  mutation, the recovery-required snapshot, one drain returning saved ENOMEM,
  a cleared snapshot and successful fresh admission. Independent Linux fsck and
  exact 64 KiB/three-byte payload audits pass for all four final filesystems.
  Manifest: `.codex-remote-attachments/ext4-health-20261020-181019/manifest.json`.
  Recorded source hashes match the final inputs. **Whole-checkout output
  preservation is qualified**: shared `bin/fortress.elf`, `bin/fortress.iso` and
  `bin/initramfs.tar` changed during concurrent work. The manifest retains that
  list; the functional PASS does not assert unchanged shared artifacts. This
  task's build commands target only copied-workspace test outputs. Unrelated
  Watchtower/TLS/build inputs are frozen at
  committed checkpoint `9a56be3` only inside that workspace.
- Native freestanding `build/fs/ext4.o` compiles with strict flags both with and
  without the fixture macro in the copied workspace. The private filename
  marker is present only in the enabled object. No test ISO/kernel was relinked
  during this exclusion check. Proof: `build/ext4-health-recovery/production-exclusion.json`.

Firmware-only pre-I/O failure injection is compiled into `build/fs/ext4.o` only
when the isolated health fixture uses `HEALTH_TEST_PROBE=1`. The private
`FORTRESS_EXT4_HEALTH_TEST` branch denies one cleanup of the probe's named victim
with ENOMEM. It does not add a syscall, device fault, physical-write admission or
production filename behavior. Host failures above use the actual read/flush
adapters rather than that injection. Firmware coverage must be read with this
distinction; it is not physical I/O or sector-tear evidence.

An initial copied-workspace firmware build failed because concurrent Watchtower
editing temporarily defined `wt_sleep_ms` twice. The runner now freezes unrelated
Watchtower compilation inputs at its recorded committed checkpoint, alongside
the existing unrelated TLS/build freeze, only in the private copy. Shared edits
are untouched. No failure of the ext4 code or a boot is attributed to that build.

The full 12,784-cut replacement campaign and broad account regression were run
for the preceding EH-3 change; their results and concurrent-artifact qualification
remain [historical evidence](ext4-durability-health.md), not fresh executions of
this flag-only follow-up. No release build, physical write, commit or push is
performed here. EH-1B, package reservations and end-to-end best-effort caller
policy remain separately scoped.
