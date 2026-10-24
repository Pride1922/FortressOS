# Private Gate 1 kernel backend

2026-10-24. Continuation of the [host candidate and source trace](PKG2_GATE1_PRIVATE_BACKEND.md).
The untracked [production integration plan](PKG2_PRODUCTION_INTEGRATION.md) is
preserved. Installation, production trust provisioning, public syscall allocation,
release-media builds, pushes and physical-device writes remain disabled.

## Implemented private boundary

`scripts/pkg_gate1_kernel_overlay.py` constructs a separate copied kernel. It
closes function dependencies over the accepted validation/recovery source bodies,
emits `pkg_gate1_engine_generated.inc`, and records every selected function's
source/hash and each port edit. No host translation unit, host main, signer,
fixture constructor, disk-fault model, printf transport or host filesystem API
is compiled into the kernel. The emitted module includes native EXT4 internals
and the actual bounded record/catalog/artifact/enrollment/crypto validators.

The remaining private configuration lives in `pkg_gate1_engine_config.inc` and
`pkg_gate1_storage.inc`. The fixed single-demo profile, paths and record schema
remain explicit private-test bounds. Policy, public test keys, descriptor and
certificates are runtime inputs from the separately pinned immutable boot module;
they are never selected from unauthenticated mutable disk data. Nothing is placed
in production `src/` or the default dispatcher in the working checkout.

Recorded port edits replace fixed-path snprintf with bounded memcpy, remove
host-only diagnostics, refuse unreachable fixture-bootstrap/legacy commit modes,
and add an interrupt check at unlocked recovery boundaries. The 11 durable
retirement transitions and record construction remain the accepted engine.

The private mount is selected before login in the copied `kmain`. Its namespace
stays detached and is retained for the complete boot; there is no unmount/reuse
API. Read-only JBD2 projection runs `ad_validate` before replay/dirty marking and
generic orphan recovery. Reopened state is checked again. Actual filesystem UUID,
native inode identities/maps/ownership, account lineage, fixed-role capacity,
grant records and signed completion evidence are checked. Only successful
construction publishes the private admitted flag. This flag grants no ordinary
VFS access, account authentication or root namespace publication.

Without the private QEMU fw_cfg NVMe selector, backing is retained RAM. With that
selector, the private build additionally requires a unique 512-byte namespace of
exactly 32 MiB before using the native NVMe read/write/flush driver. The runner
admits only its own newly copied regular file and validates the final QEMU disk
argv on every boot. The physical NVMe exclusion and raw-pattern test gates in
production are unchanged. The ISO uses explicit `boot_mode=live` so generic GPT
diagnostics cannot run against the raw enrolled filesystem fixture.

Missing/foreign/incomplete or unreadable selected enrollment refuses private
admission. Constructor failure leaves the backend unavailable, and never selects
a legacy writable package mount. The ordinary live shell remains available to
test that refusal; it has no path to the detached package filesystem. This is not
a production boot-selector, GPT/PARTUUID enrollment or legacy-root policy.

## Owner, operation and mount lifetime

`pkg_gate1_kernel.inc` implements private dispatch 200 for the existing 32-byte
request/80-byte result candidate. It checks exact request byte count and both
user ranges, requesting writable output, before copying the input once into a
kernel value. No raw user pointer is retained. Output is copied only after the
backend returns; a failed copyout never reruns storage.

The current TCB lazily receives a private owner slot/generation at first use.
New TCBs are zero-initialized rather than inheriting that key. The kernel supplies
the key, immutable mount operations and boot-admitted mount identity; users
provide only a session value. Numeric PID or a copied session never constitutes
owner authority. Existing spawn/descriptor/exec behavior is unchanged. There is
no in-place exec syscall; a future such path must explicitly cancel first.

The private owner arena retains birth, session and operation references. Each
acquire and mutation rechecks current effective UID zero plus CAP_SYS_ADMIN;
cleanup remains allowed after credential loss. The session retains only the
kernel mount identity, not callback addresses, user pointers, selectors, keys or
maps. Mount operations are fixed by the trusted kernel embedding on each call.
One nonblocking atomic execution credit serializes storage without a spinlock
held across I/O or yield. Concurrent calls return EAGAIN while that credit is
occupied; a competing owner cannot acquire the writer.

The copied `process_exit` and direct `thread_exit` paths cancel outside scheduler
and process locks. Calls are synchronous: recovery checks pending signals between
storage steps, an admitted durable step finishes, and `pw_complete` releases its
operation reference before control can reach termination. Exit detaches the owner,
cancels its session and asserts zero remaining references. Repeated exit hooks
are idempotent. The real reaper, after detaching the dead list and releasing its
scheduler lock, asserts that no owner key remains before TCB destruction. No
storage drain or process-registry lookup runs under filesystem/scheduler locks.

The mount itself is boot-retained and never freed while sessions or calls exist.
This proves a bounded private lifetime; it is not a general refcounted production
mount/unmount interface. Releasing a session changes no durable record or grant.

## Ring 3 scenarios and independent oracle

The private probe covers valid read-only status; request size/reserved/operation
rejection; null, unmapped/page-crossing, read-only and kernel user buffers;
competing owner and child session misuse; capability loss and effective-UID loss;
normal exit with an unreleased session; SIGKILL after durable progress, SIGTERM,
STOP/CONT and caught SIGINT at unlocked boundaries; real reaping and fresh-owner
reuse; stale sessions; output failure after committed recovery; IDLE status and
repeat recovery without extra retirement credit. Seven children per primary
boot must be reaped. Application callers remain ordinarily unpinned; SMP=4
requires observation of a non-BSP caller.

Fault injection is private test control 201. It cannot alter policy, maps or
request fields. Its post-freeze latch test sets only the retained engine's
in-memory taint flag: status, recovery and new acquire must then return EIO
without another filesystem write, while session cleanup remains possible. This
tests a latched engine state, not an actual uncertain controller completion.
The injected copyout failure happens after storage returns;
the original session remains usable, and status must observe IDLE sequence 12.
The filesystem is frozen before audit. On RAM runs a read-only QMP memory export
is audited; on NVMe runs the frozen regular-file backing is copied for audit.
Linux `e2fsck -fn` must return zero. `debugfs` extracts the five package/account
terminal records and executable, all of which must match the previously
independently generated and signed host terminal vector byte for byte.

Two fresh boots from the preceding frozen NVMe output must each recover IDLE
sequence 12 with zero retirement commits. Boot lifecycle dirty/clean journal
transactions still write; zero retirement commits is not zero device I/O.

The negative fixture cases corrupt the completion signature, remove an enrolled
marker, substitute marker identity, or inject a selected-root superblock read
error. Refusal must precede every package adapter write/flush; Ring 3 status and
acquire must return EOPNOTSUPP, and the regular-file data image must remain
byte-identical to its preboot input. An injected read error is not a physical
device failure or evidence about DMA uncertainty.

## Retained development failures

- `pkg-gate1-kernel-20261024-074656-291270`: first strict build failed on two
  misleading-indentation warnings and missing crypto declaration. Repair logs
  1–4 retain missing crypto linkage, missing upstream include path and missing
  verifier helper failures before the development kernel linked. Repairs were
  applied only to the copied workspace; this is not an immutable acceptance run.
- `pkg-gate1-kernel-20261024-074955-335306`: development reuse of that repaired
  workspace passed BIOS/SMP=1 RAM-backed runtime, Linux fsck and record oracle.
  Its `development_reuse` field prevents treating it as a clean source build.
- `pkg-gate1-kernel-20261024-075453-359603`: first NVMe attempt entered generic
  GPT diagnostics before the private hook and refused the raw filesystem image.
  The runner's QEMU was explicitly terminated; no package backend ran. Boot
  failed and source/media preservation passed. Explicit live mode corrected the
  fixture configuration, without enabling any raw-write gate.
- `pkg-gate1-kernel-20261024-075800-029924`: BIOS/SMP=1 plus both fresh repeat
  boots and Linux oracles passed. BIOS/SMP=4 failed the probe's fixed-yield-count
  readiness wait. The original FAIL and all outputs remain. The test now uses a
  bounded sleep-based wait so another CPU can complete admission/storage work.
- `pkg-gate1-kernel-20261024-080129-061537`: all 16 boots and all Linux/record
  oracles passed with source/media preservation PASS. Superseded by the final
  build's removal of unused crypto routines and scheduler-wait correction.
- `pkg-gate1-kernel-20261024-080837-530667`: the build's linked-frame audit and
  six BIOS boots/oracles passed. UEFI/SMP=1 stalled in the private fault-control
  pause. Source tracing identified IRQ-masked yield polling starving a sleeping
  controller's timer when no other runnable user task remained; its private QEMU
  was explicitly terminated and the FAIL is retained. The hook now uses existing
  `sched_wait_until` with a lock-free atomic predicate and IRQ-excluded enrollment.
  Signals wake that continuation through the real scheduler; there is no new
  wait primitive, lock rank or lock across a context switch.

The private link now removes unused crypto sections, including Argon2, signing
entry points and entropy adapters. An nm/stack-usage audit checks only retained
functions: the existing verifier exception is 1280 bytes and every other added
linked function must be at most 1024 bytes. This is a per-frame audit, not a
proof of a worst-case transitive call-stack bound. Existing kernel objects are
not given new general stack-budget or hardware-entropy claims.

## Final acceptance and evidence

**Private kernel status/recover milestone PASS**, limited to the accepted finite
profile. This completes the private Ring 3/lifetime/mount-admission deliverable;
it does not activate the production integration gates or default `/bin/pkg`.

All commands ran in WSL `Ubuntu-24.04`, `/mnt/c/Sources/FortressOS`:

| Exact command | Final result / directory under `.codex-remote-attachments/` |
| --- | --- |
| `make -f tests/pkg_grant_candidate.mk test-pkg-gate1-kernel` | PASS, `pkg-gate1-kernel-20261024-081601-383016` |
| `make -f tests/pkg_grant_candidate.mk test-pkg-gate1-backend-host` | Strict GCC + ASan/UBSan, 128 actual-registry/mock-storage lifetime cycles and freestanding compile PASS, `pkg-backend-20261024-080845-299982` |
| `python3 scripts/test_pkg_cli_transaction.py --kernel-backend` | Six-geometry actual-engine host regression PASS, `pkg-kernel-backend-host-20261024-080855-564770` |
| `git diff --check` | PASS; Git reports working-copy newline conversion information |

The kernel command was also run for the retained `080837` FAIL above. Earlier
development invocations used `python3 scripts/test_pkg_gate1_kernel.py --smoke`,
`--smoke --nvme --negatives`, and `--nvme --negatives`; their exact argv and
copied sources remain in the corresponding manifests. The development reuse
runner is explicitly recorded as reuse rather than a clean source build.

| Firmware / CPUs | Primary Ring 3 + Linux oracle | Two fresh boots | Observed primary CPU mask |
| --- | --- | --- | --- |
| BIOS / 1 | PASS | PASS, zero retirement commits each | 1 |
| BIOS / 4 | PASS | PASS, zero retirement commits each | 15 |
| UEFI / 1 | PASS | PASS, zero retirement commits each | 1 |
| UEFI / 4 | PASS | PASS, zero retirement commits each | 15 |

Totals: **16 boots**: four primary, eight fresh-repeat, four negative BIOS/SMP=1.
The primary boots reap **28 children**, exercise **12 terminating/caught signal
scenarios** plus STOP/CONT, and finish at IDLE sequence 12 after exactly 11
retirement commits each. All 12 positive/repeat Linux audits pass; all **72
record/executable comparisons** are byte-exact. This is 4 KiB filesystem blocks
and 512-byte sectors on 32 MiB raw enrolled NVMe fixtures, not a GPT installed
system image or a new full geometry kernel matrix.

Each primary output is exactly the prior independent host terminal image,
SHA-256 `615e39b638f652a1c78d408271650b1e4f2d636f19d59bae91674c4c0e808082`.
Each fresh repeat makes zero retirement commits, with 112 sector-write callbacks
and 12 flush callbacks for boot/freeze journal lifecycle. Primary totals are
2384 sector-write callbacks and 67 flush callbacks, including boot/freeze.
These counters are not physical media measurements.

All four negative cases refuse before any package-adapter write or flush and
retain their input image hash after guest shutdown. Signature/marker changes are
deliberately damaged fixtures; they are not claimed to pass fsck. The read-error
case injects a failed read of the selected superblock, not an actual NVMe device
fault. No fallback package mount is published.

The generated kernel core has **53 selected functions**, with every port delta
recorded. The final linked-frame audit measures **1152 bytes** for
`mono_crypto_eddsa_check_equation`, within the existing 1280-byte verifier
exception, and at most **816 bytes** for every other added linked function.
Unused Argon2, signing entry points and entropy adapters are absent from the
linked private kernel. No transitive stack, entropy-quality or independent
cryptographic audit claim follows from this measurement.

The final host regression adds **66 CLI scenario groups, 144 selected
interruptions, 288 fresh-process recoveries and 24 Linux audits** to the native
boot evidence. Host stdout and lifetime transport remain mocked; this count must
not be presented as native Ring 3 interruption/power-cut coverage. Later private
kernel-only edits did not change those host compiled inputs, which are
independently rechecked against current source.

The [aggregate reconciliation](../../.codex-remote-attachments/pkg-gate1-kernel-summary-20261024/manifest.json)
records exact manifest/ISO/generated-core hashes, per-boot post-shutdown disk
checks and stack measurements. It independently rechecks all final compiled
copies, source inputs, fixture inputs and guards. Each audited NVMe image must
still equal its actual backing file after shutdown, not merely at freeze time.

**Source/media preservation PASS**, independently from **boot/runtime PASS**:
production `src/`, default Makefile/CLI/dispatcher, AGENTS.md, PROTECTED.md,
SSL checksum, shared media and the original production plan remain unchanged.
The original plan's SHA-256 remains
`f1e8a769becaac55509a82a1633fda9c6057167588becab8c9cba91521fb1017`.
All changes remain uncommitted; retained failures and their original manifests
have not been relabeled or removed.

## Production boundary and next work

The actual status/recover engine, bounded transport, owner lifetime and selected
private enrollment now have native acceptance. Default dispatch remains disabled.
The next integration work is the test-key offline enrollment producer/reader and
independent production selector design: GPT/PARTUUID and installation binding,
absence versus I/O/partial enrollment classification, rollback floors and
exclusion of older incompatible kernels. Production key selection/provisioning
remains a later explicit decision. The boot-retained private mount is not a
general production mount-retention/unmount API.

No physical power-loss, SSH-disconnect campaign, NVMe uncertain-completion
injection, general installation, account UPDATE, arbitrary package, upgrade/removal,
production trust or production root-publication acceptance is claimed. The
private in-memory taint-latch test requires fresh-boot recovery after an error;
it does not simulate all controller persistence states. Installation, production
provisioning, release builds, pushes and physical writes remain disabled.
