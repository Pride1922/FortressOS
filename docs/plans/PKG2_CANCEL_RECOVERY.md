# Private cancellation recovery and real kernel lifecycle validation

2026-10-22. Extends [retained calls](PKG2_RETAINED_CALL.md). Production integration,
installation and production trust provisioning remain disabled. Changes remain
uncommitted. Host recovery and disk-free kernel lifecycle results are separate
claims; no end-to-end kernel package/filesystem cancellation result follows.

## Cancellation after durable progress

The private host fixture prepares an ordinary 70-block file and a separate tagged
package file with T=(8,1), G=(5,0), Q=(3,1). Real credentialed VFS truncate and
unlink each require five semantic commits: durable intent, three bounded
right-edge reclamation batches, then orphan-chain removal. The runner measures
the uninterrupted commit count, then cancels after each acknowledged commit
before the last. It requires EINTR at the next semantic boundary, no subsequent
device writes, exactly one retained ordinary orphan and drained call references.
It closes the image without invoking sync/recovery on the dying process.

`pc_cancel_current` publishes cancellation atomically through the current owned
binding. The copied semantic dispatcher uses it only after an actual successful
commit. This is a semantic/checkpoint-boundary model, not a cut during a sector
write, journal commit record, checkpoint or hardware flush. The host device uses
regular files, memory reads and fflush-backed barrier acknowledgements; physical
power-loss durability is not tested.

Every pending image first undergoes a wrong-generation rejection with unchanged
image hash and zero writes/flushes. Then two independent fresh processes recover
it. The constructor's actual global allocation/reference audit and JBD2 preview
precede grant/namespace/ownership validation and the account-capacity certificate.
All precede the first replay/activation/cleanup write. The private extension allows
only the one expected ordinary inode/generation in the preview orphan chain,
excluding the fixed package records and charged tagged inode. Linked shrink
intents must still be named by the known sandbox directory; deleted intents must
have zero links and no surviving name. No mount/engine trust flag, inode identity
or allocation counter is fabricated to permit recovery.

The expected ordinary origin is a **trusted fixture sidecar**, captured from the
real inode before cancellation. This is not authenticated durable enrollment or
a generic policy for distinguishing every possible account/foreign orphan after
restart. Account or charged-orphan recovery and arbitrary unknown orphan origins
remain excluded. The existing structural certificate is reused without changing
its C-A calculation, and F-G >= max(A0,R) remains required before replay.

After successful preflight, actual constructor orphan recovery drains the
unfinished ordinary operation. Normal account preparation/publication follows.
Final validation requires no orphans and unchanged package T/G/Q. Independent
Linux checks require clean e2fsck, unchanged canonical account bytes, exactly
three K blocks in the tagged file, and either a present empty ordinary file
(truncate) or no ordinary name (unlink). Repeating recovery in a fresh process
checks the completed result again. No automatic IDLE retirement is performed.

## Real kernel signal, exit and reaper validation

The disk-free QEMU fixture builds the actual kernel only in a private copied
workspace. Exact overlays add the retained binding field, a test-only syscall
200 and exit/reaper hooks. The private translation unit includes the same writer,
admission and retained-call candidates, with trap assertions instead of hosted
assertions. The source tree's production TCB, syscall dispatcher, scheduler,
process exit and reaper code are unchanged.

The private Ring 3 program runs three rounds of eleven children:

- Ordinary bound exit and queued unpinned account exit.
- Actual self SIGKILL while the atomic commit gate is held: cancellation cannot
  end that binding until the gate is released, then default termination runs.
- Caught SIGINT: the syscall unwinds its binding before real custom-handler
  delivery and signal return; the handler must run exactly once.
- External SIGKILL and SIGTERM of bound, yielding kernel continuations.
- SIGSTOP/CONT while the binding remains retained, followed by SIGTERM.
- Four simultaneous bound continuations, then SIGKILL and complete drainage.

Actual `process_signal_interrupt`, STOP parking/resume, user-return termination,
`process_exit`, `thread_exit`, context switching and `sched_reap_dead` execute.
Copied exit hooks acknowledge the owned binding before descriptor/CR3 teardown.
The actual reaper asserts a clear binding before releasing resources; a counter
advances after its real `kfree(dead)`. Every round requires arena/admission counts
zero, exact exit-cleanup/reap totals and successful subsequent reuse. Each boot
requires 33 reaped children and 30 exit-hook cleanups; the three caught-handler
children already unwound their bindings in the syscall.

This fixture has no package grant/filesystem I/O. Its held commit gate is an
atomic protocol exercise, not a journal operation. It validates real kernel
lifecycle delivery separately from the host durable-recovery campaign. It does
not validate actual package writer/lease teardown, multi-threaded user processes,
exec replacement, stalled devices or production mount admission.

Only an isolated optical fixture and paired OVMF code/vars are attached. No data
disk, network or physical target is attached. The runner builds no release ISO
or raw image: it builds a copied kernel, adds one program to a copy of the
existing initramfs template, and assembles a separately named private fixture ISO.
BIOS/UEFI x SMP=1/4 are checked. SMP=4 must observe bound execution on an AP;
the recorded CPU mask gives the actual coverage. Boot acceptance, source-copy/
repository preservation and shared-media preservation are reported separately.

Run standalone `test-pkg-cancel-recovery`, `test-pkg-cancel-recovery-tsan` (4 KiB
blocks, both sectors), and `test-pkg-call-kernel` via
`make -f tests/pkg_grant_candidate.mk`. TSan retains per-process `setarch -R`.
Exact results and retained failures are in the roadmap. Further work includes
account transaction cancellation/recovery, authenticated ordinary-orphan origin,
combined real-kernel grant/filesystem acceptance and production integration review.
