# Private account cancellation and combined kernel grant/filesystem validation

Continuation: [mid-callback account-orphan recovery](PKG2_ACCOUNT_ORPHAN_RECOVERY.md)
adds a private trusted-origin admission workbench. The results and exclusions
below describe the earlier completed-callback campaign.

2026-10-22. Extends [cancellation recovery](PKG2_CANCEL_RECOVERY.md). All hooks
remain in copied sources or private tests. Production integration, installation
and production trust provisioning remain disabled.

## Account transaction cancellation

The account fixture runs actual `accounts_prepare`, BEGIN, UPDATE and FINISH,
including the real account parser, redo engine and EXT4 account backend. It
retains a separate RESERVED package grant with T=(8,1), G=(5,0), Q=(3,1).
After every completed mutating account callback that performed device writes,
the retained call can publish cancellation. Subsequent mutating callbacks and
sync refuse EINTR before more device writes. The failed UPDATE leaves service
authentication disabled and CHECK returns EAGAIN while its writer remains.
Owner unwind releases all volatile binding/admission references.

Each callback finishes its actual bounded inode/orphan work before cancellation
is injected. This covers redo staging, intent publication, canonical removal and
publication, pending retirement and staging cleanup. It does not cover an
account orphan stranded inside a callback, sector/flush cuts or uncertain I/O.
The existing constructor continues to refuse such unsupported account orphans;
no account identity or capacity counters are fabricated to admit them.

Every interrupted image is first rejected using a mismatching trusted fixture
marker hash, with zero writes/flushes and unchanged whole-image hash. Two fresh
processes then perform the actual constructor audit, grant/capacity preview
before replay and account-service recovery. Before durable pending publication,
recovery retains old canonical accounts and cleans staging. After publication,
it rolls forward from retained staging and hash-checked intent; interruption
after pending retirement accepts the complete new canonical set. Linux fsck and
exact-content checks independently verify the outcome and absent temporary roles.
Durable package records and actual tagged allocation remain separately audited.

Trusted expected package identities come from the existing private sidecar,
not authenticated production enrollment. The account text is coherent root-only
service input, not the maximum-byte synthetic capacity profile. Only the passwd
GECOS field changes; group/shadow bytes are unchanged although all three roles
are restaged, removed and published. This does not cover simultaneous content
changes in all databases or broader user/group layouts. A separate focused
`--account-all-files --block 4096` variant adds explicit root group membership
and changes the locked shadow marker to `*`, making all three databases distinct.
It checks cuts 3/7/8/10/11/13/14/15/18 with 512-byte sectors: pre-intent staging,
each canonical remove/publication, pending retirement and final staging cleanup.
It is not a second full geometry matrix or a broader user/group-layout test.
The host backend
uses regular files and fflush acknowledgements; no physical durability claim.

## Combined kernel grant/filesystem fixture

The private kernel loads a 32 MiB EXT4 fixture from immutable TarFS into a
kernel-owned, loader-mapped BSS RAM block device. It never registers a hardware block device or
attaches a data disk to QEMU. The actual detached constructor, ownership audit,
JBD2 preview and private grant/capacity certificate precede replay/activation.
The mount remains detached; the running live root is not replaced.
This combined kernel fixture covers 4 KiB filesystem blocks and 512-byte sectors
only; the host account campaign covers its separately recorded geometries.

A privileged Ring 3 coordinator obtains writer ownership from the actual process
registry, proves durable IDLE, and reserves a real grant through the existing
private planner. Retained package calls create the charged inode and append via
actual EXT4/JBD2 semantic commits. Ledger and ownership images join each commit.
The constructor performs the global ownership audit while unpublished and
exclusively owned. Serialized reservation rechecks actual role/inode/map state
without repeating that quadratic audit under IRQ-disabled filesystem exclusion.
After every child is reaped and all bindings drain, the coordinator freezes the
backing and opens a fresh, naturally unpublished audit engine under retained
exclusive device ownership. The full audit runs there without a filesystem lock,
with zero additional writes/flushes. Mounted-engine ownership flags stay intact.
This is a finite trusted fixture phase protocol, not a general live-mount audit API.
The private RAM sector/flush callbacks service the existing lockless polled TLB
mailbox protocol. The long constructor is entered with syscall IF masked; a
memory-copy-only adapter would starve a remote shootdown despite holding no lock.
No timeout is increased and no IRQ is enabled under filesystem exclusion. The
number of polled acknowledgements is retained separately from I/O counts.
Children use actual kernel current-task delivery and retained package admission.
The trusted fixture dispatcher supplies the coordinator's retained writer values
to these child operations; it is not a user lease ABI or proof of per-caller
package authorization, descriptor nontransferability or automatic owner-death
session retirement. Those remain separate integration work.
SIGKILL, SIGTERM, STOP/CONT followed by TERM, caught SIGINT and four simultaneous
writers interrupt their yielding kernel continuations after acknowledged writes.
Each cancellation must refuse the next real planned write with EINTR and zero
additional writes by that operation. The actual exit/reaper hooks drain owned
bindings; the caught-handler path unwinds before signal delivery.

The final reservation intentionally remains RESERVED: eight charged blocks and
one inode, no unused escrow. Closing/detaching volatile writer ownership does
not release its durable grant. Freeze/checkpoint completes before QEMU exports
the RAM device through a read-only virtual-memory snapshot. Independent Linux
fsck, exact bytes, record hashes and actual inode generation/mode/owner/link/
allocation checks audit the exported image without modifying it.

RAM barriers are deterministic fixture acknowledgements. They do not test NVMe,
USB, controller ordering, persistence across QEMU restart or physical power loss.
The combined fixture does not run account-service updates inside the kernel,
charged-orphan recovery, exec replacement, arbitrary namespace mutations or
production package dispatch. Account recovery and kernel package I/O are distinct
acceptance campaigns. Boot/runtime acceptance and source/media preservation are
recorded independently. Exact results and retained failures belong in the roadmap.

Standalone targets in `tests/pkg_grant_candidate.mk`: `test-pkg-account-cancel`,
`test-pkg-account-cancel-tsan`, `test-pkg-account-cancel-all-files`,
`test-pkg-kernel-fs` (BIOS/UEFI x SMP=1/4).

The subsequent host-only [durable account-service/redo layer](PKG2_ACCOUNT_SERVICE_DURABLE.md)
binds the actual account service to paired generation/provenance records. Its
acceptance is separate from this kernel campaign; production integration and
installation remain disabled.
