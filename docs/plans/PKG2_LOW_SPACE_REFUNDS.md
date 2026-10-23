# Private low-space account recovery and charged refunds

2026-10-23 continuation of [pending/trust admission](PKG2_MOUNT_PENDING_TRUST.md).
Production integration, installation, production trust provisioning, release
media builds, pushes and physical-device writes remain disabled. The user
subsequently authorized a local PKG checkpoint commit.

The mount workbench now derives remaining account capacity from actual linked
fixed-role allocations and remaining unlinked account-orphan allocations. With
`D=ceil(8192/block_size)` and `E=(D>4 ? 1 : 0)`, the maximum is
`C=(9*(D+E)+2 blocks, 11 inodes)`. Remaining capacity is `R=C-A`, including extent
metadata and the still-allocated orphan inode. Duplicate identities, excessive
maps or capacity underflow refuse. Existing account provenance and full EXT4
ownership/bitmap checks remain mandatory; the capacity count alone does not
authorize an orphan.

For a staged account mutation, remaining capacity changes by the observed
superblock free-counter delta. Both dimensions must retain `F-G>=R`, with G
read from the actual durable package grant. Thus account allocation consumes
its remaining capacity, freeing account allocation restores that capacity, and
an unlink without physical release does neither. Production ordinary-writer
floors and account-only allocator bypasses are unchanged. The copied account
service BEGIN headroom call uses the same complete validator, retaining actual
trusted-root, tainted, quiesced and frozen checks. The production service and
public headroom function are unchanged.

Account-orphan reclamation restores account capacity R; it does not increase
package escrow G. Package-charged reclamation instead restores G and decreases
Q in the separate charged lifecycle profile described below. Logical file
length and namespace removal alone supply no refund in either case.

Only the existing under-lock `account_allocation` or
`account_namespace_operation` context may subtract a negative free-counter
delta from remaining account capacity. An ordinary allocation keeps R intact and
must fit outside G+R. Both pressure modes run actual competing inode creation
and block growth at exact capacity, requiring ENOSPC with zero writes/flushes
and an untainted engine. This distinction was found during review of the first
passing pressure checkpoints; those checkpoints are retained and superseded.

`test_pkg_mount_admission.py --low-space` initializes active fixture grants at
exact effective capacity in both dimensions. Separate one-block-short and
one-inode-short cases change and reseal the grant/account binding coherently;
they must reject without writes or flushes. This is escrow pressure, rather
than physical exhaustion. `--physical-low-space` instead fills only disposable
regular-file filesystems with ordinary dense data and empty files until actual
free blocks and inodes equal G+R. Linux fsck checks these fixtures before use.
The two pressure modes are mutually exclusive. IDLE has zero escrow by grammar.

`pg_reclaim` accepts a charged refund only when the observed increase in actual
free resources equals the decrease in owned allocation, separately for blocks
and inodes. It preserves T, increases G and decreases Q by that exact amount.
An unchanged allocation produces zero credit; repeated observation cannot
refund again. Rejected observations preserve the caller's output. Sequence
exhaustion refuses. This helper performs arithmetic, not identity authorization.

The existing actual EXT4/JBD2 charged-orphan lifecycle workbench now uses this
helper after inspecting planned inode allocation bits, remaining `i_blocks`
and actual free counters. Creation and growth use the separate charge operation.
Ledger/table changes share the actual semantic journal commit with unlink or
physical reclamation. Tests retain open-reference pins, final close, constructor
cleanup and repeated reboot recovery. `--selected-cuts` selects first/middle/last
I/O boundaries in all eight cache/before/after profiles; default remains every
boundary. These are RAM sector-atomic host faults, not physical power failures.
The charged target is one filesystem block and one inode, with inline extents
and eight blocks of total escrow. Data release and inode release are separate
actual semantic commits. This does not establish multi-file, fragmented or
deep extent-tree package cleanup; the account tests separately use 8192-byte
fixed-role files across all three block sizes.

## Remaining integration boundary

The signed pending/trust mount profile still refuses package-charged orphans.
Its archive, next state and immutable intent remain linked verification
evidence. The charged lifecycle workbench is a separate private grant profile;
it does not establish account-generation binding or signed package admission
for package cleanup. Reclaiming the last verification copy before defining a
durable retirement capability would strand recovery or weaken authentication.
Joining these profiles, retaining sufficient retirement evidence and atomically
updating the account snapshot with package refunds remains open. No production,
Ring 3, boot, SMP, scheduler or physical acceptance follows from these host tests.
The capacity bound covers filesystem blocks and inodes, not heap availability,
cache/orphan slots, journal staging memory or device liveness. It assumes the
existing fixed-role directory-slot certificate and bounded account profile.

Exact results and retained failures are recorded below after verification.


## Verified results (2026-10-23)

Final source and artifact identities, all raw attempts and independent preservation checks: [summary manifest](../../.codex-remote-attachments/pkg-low-space-summary-20261023/manifest.json). Its SHA-256 is `490d7a243a02a192904ef175c8e7136a8a468c83f37840120e404777cb3a4160`.

| Private gate | Result |
| --- | --- |
| Exact effective capacity | PASS: 42 states, 168 account UPDATE interruption cases, 144 grant-tick I/O interruption cases, 774 pre-replay zero-I/O rejections, 300 competing block/inode zero-I/O rejections, 708 Linux audits |
| Actual block/inode exhaustion | PASS: 42 states, 168 account UPDATE interruption cases, 144 grant-tick I/O interruption cases, 774 pre-replay zero-I/O rejections, 420 competing block/inode zero-I/O rejections, 750 Linux audits |
| Charged-orphan selected boundaries | PASS: 6 geometries, 576 cuts, 1152 constructor restarts, 12 Linux audits |
| Charged-orphan exhaustive 4 KiB | PASS: 10152 cuts, 20304 constructor restarts, 4 Linux audits |
| Reclaim arithmetic/codec | PASS: 28 accepted and 2,132 rejected reclaim observations; 1,638 codec vectors, 4,101 record-set cases; 132,132 accepted and 1,117,068 rejected budget plans |

Both pressure matrices cover BIOS-independent host geometry 1/2/4 KiB blocks × 512/4096-byte sectors, seven grant/publication states per geometry. Each interruption case runs two successive fresh host-process recoveries; the second must commit nothing. Cuts 13 and 29 retain actual canonical-account and pending-intent orphans. Charged lifecycle restarts run the actual constructor twice in the same host process after teardown/reset, not fresh processes or firmware boots. Linux audits apply to the retained outputs, not every charged fault iteration.

Physical fixtures have exact initial `F-G=R`: 80, 35 and 17 blocks for 1, 2 and 4 KiB respectively, and eight inodes. Active fixtures retain `G=12 blocks/4 inodes`; IDLE retains zero escrow. The separate one-block-short and one-inode-short rejections preserve the other resource bound. Final selected logs correctly show 24 executed cuts per operation, 96 per geometry.

Final matrix manifests:

- `pkg-mount-admission-20261023-132910-540496`: `b2c0dab8d967009c260cc8173c638dc54c76ee51e473d5f310f0022c13e42e27`.
- `pkg-mount-admission-20261023-133011-450083`: `29dde4e5dbb3ccef8fdb6d8662adff274554f84d0f38ef2baeca99670b9300e0`.
- `pkg-mount-admission-20261023-133011-459182`: `150c847c3da139bccf2160fdee041b854c5dd55d5eb8a8e2b6e7fc0da4235298`.
- `pkg-mount-admission-20261023-133011-455507`: `e001a65d804d35d6a8fd45b9267d6a9d30e8f4eddbd1359f8e65bddd67ff56ac`.
- `pkg-mount-admission-20261023-133011-458579`: `972bdce44a1080b4384025c2ce0c5f7319ea67d77743c7ade4f27e57ba603e50`.
- `pkg-mount-admission-20261023-133011-504827`: `9de4370a978c95ac937a7d113d0f2ceb7235d673f52b6641ae39f25cea510b0d`.
- `pkg-grant-lifecycle-20261023-152605-034282`: `000a96820e756506a627d21e5ee6a900133ecf8a9c7c904286d824821170a253`.
- `pkg-grant-lifecycle-20261023-150500-420980`: `f35d8ef99955d2541cead6ddd869bf2f6984c6a95ff5026a309b552175fcf21a`.

Final standalone codec run: `pkg-grant-codec-20261023-153537-150909`. `make -n` dispatch checks, Python compilation and `git diff --check` pass. The unchanged pure mount validator passes its existing 1024-byte freestanding frame gate.

## Retained findings and failures

- `pkg-grant-lifecycle-20261023-150413-747409`: first helper wiring applied reclaim-only checks to creation. Runtime assertion failed; raw manifest remains INCOMPLETE. Creation/growth now use PG_CHARGE.
- `pkg-mount-admission-20261023-130337-880338`: initial 14-state runtime smoke passed; preservation FAIL retained because the grant helper C/header were edited during the run.
- `pkg-grant-codec-20261023-150709-720914`: runtime checks passed; final before/after preservation assertion failed when the host stdbool include changed during the run. Original manifest and logs retained; later stable runs pass.
- `pkg-mount-admission-20261023-131023-876004`: physically full IDLE fixture passed preview but failed account BEGIN due to its separate fixed-floor headroom call. The private call now uses the same full validator; health/frozen checks remain.
- Six first pressure PASS checkpoints (`130556`, `130816`, `130826`, `131448`, `131458`, `131508`) predate the ordinary/account context fix and are superseded. They did not test competing allocation at exact capacity. Raw PASS manifests remain unchanged.
- Isolated old-code reproduction in `pkg-low-space-summary-20261023/old-context-reproduction-typed/` proves ordinary inode creation succeeded at zero effective headroom and issued 125 private-model I/O events. The expected regression assertion failed. The corrected matrices require zero-I/O refusal for both actual inode creation and block growth.
- Six `pkg-mount-admission-20261023-132439-*` builds, and the first `old-context-reproduction/compile.log`, retain the strict pointer-sign compile error in the new one-byte competing fixture. The fixture now supplies a uint8_t. No runtime success is claimed for these builds.
- Earlier selected lifecycle logs printed the number of candidate boundaries in their per-operation lines. Their final total 96 per geometry was correct; the final selected overlay reports actual executed counts. Those raw reports remain retained.

All final compiled repository inputs and copied inputs were preserved. AGENTS.md, README.md, SSL checksum asset and all four shared kernel/initramfs/ISO/raw-image guards match their task-entry hashes. This preservation PASS does not change earlier historical media FAILs. Runtime, Linux audits and source/media preservation are distinct from boot acceptance: no boot was run here. Verification completed before the user-authorized local checkpoint commit; production integration, installation and production trust provisioning remain disabled. No release-media build, push or physical-device write was performed.


Continuation: [private retirement-ledger codec and accounting model](PKG2_RETIREMENT_LEDGER_CODEC.md) now checks the five-record candidate before mount integration. It adds no actual JBD2 executor or authenticated retirement-capability parser.


## Combined private integration acceptance (2026-10-23)

The subsequent bounded private milestone is now closed. See
[final acceptance, exact evidence and limits](PKG2_PRIVATE_RETIREMENT_ACCEPTANCE.md).
This supersedes the private-integration next-step statement above only; it does
not enable production integration, installation or trust provisioning.
