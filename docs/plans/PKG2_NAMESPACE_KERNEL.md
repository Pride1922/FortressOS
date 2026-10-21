# PKG-2 opt-in EXT4 namespace admission implementation

`ext4_root_account_namespace_admit()` implements the structural admission
candidate and a mount-lifetime writer fence. It has **no production caller**.
There is no package mount path, persistent grant/marker, automatic provisioning,
directory relayout, package mutation ABI or installation enablement in this change.

## API and lifecycle

The trusted kernel caller supplies an exclusively prepared, detached journaled
root and a report pointer. Under the existing `e4_lock`, admission validates
the root and the fixed ancestor chain, scans both account directories, checks
fixed-role inode metadata and geometry, and only then publishes both fences
together. The scanner allocates its bounded 4 KiB block workspace on the heap;
allocation occurs before acquiring the filesystem lock. No device write, sync,
directory creation or relayout is performed by admission.

Failure clears the report and never arms a fresh fence. Failure when rechecking
an already fenced mount never removes its existing fence. No unfence API exists;
the fence belongs to the mount rather than a process or account lease, so owner
exit cannot release it. Discard ends ownership. A new mount starts unfenced and
must explicitly re-admit before writable publication in any future package
integration. This is **not a persistent on-disk grant or automatic boot policy**.

Frozen, quiesced, installer-owned, untrusted or published roots are refused;
tainted/uncertain and pending planner/recovery states fail closed. Unchanged
roots without an explicit call keep the previous behavior. No boot ordering,
account service initialization or installer path calls this API.

The report contains the two physical-slot counts and a mask of present fixed
roles. It conveys structural capacity, not authentication or installation
authority. The caller must separately validate the canonical database or valid
pending/staging hashes and account coherence before authentication/publication,
and separately admit blocks/inodes, heap/cache, journal/orphan resources and
package grant ownership. Missing canonical entries alone never authorize
recovery; their legitimacy depends on valid protocol evidence.

## Admitted profile

`/`, `/etc`, `/var`, `/var/lib` and `/var/lib/fortress` must be root-owned mode
0755 directories; the private `accounts` directory must be root-owned 0700.
Existing trusted-mount ownership admission remains a prerequisite; it excludes
unsupported hard links, aliased mappings and malformed trees.

The scanner reads the actual directory records, validates complete directory
checksums/names through the existing scanner, and checks dot/dotdot identities.
It accepts free 12/16/24-byte records. Fixed-role entries can occupy a 24-byte
record, or a 16-byte record only when that name uses 16 bytes. Other `/etc`
entries must have less than 16 bytes of slack. Other private entries are refused.
Dot entries also cannot expose 16 bytes of slack. Larger free records reject;
the scanner never converts their bytes into guessed virtual slots.

Fixed files must be regular, root-owned, have the exact role mode (0644 for
public passwd/group roles and 0600 otherwise), one link, no duplicate role/name
or cross-role inode alias, and size at most 8192. Existing canonical files must
be nonempty; pending has the exact 160-byte record length. Empty staging/temp
files remain structurally admissible at allowed interrupted prefixes. Their
contents and protocol legitimacy belong to the separate redo/account validator.

The `/etc` threshold is six physical 24-byte records, as in the
[candidate proof](PKG2_NAMESPACE_ADMISSION.md). The private parent requires five,
for passwd.new/group.new/shadow.new/intent.tmp/pending. All five names use at
most 24 and at least 16 aligned bytes. Before any insertion its target name is
absent, so at most four of these five names exist, including the rename source.
The fifth physical record remains available. A 20-byte name in a 24-byte record
leaves four bytes of unusable slack; pending in such a record leaves eight.
Insertion cannot split an occupied record under this profile. Deletion preserves
its length, so the eligible records remain conserved across restart. Free
16-byte records can hold pending but never consume a 24-byte record by splitting.

## Enforcement

The actual directory editor refuses ordinary mutations in either protected
parent, and refuses removal/rename of the ancestor anchors. The replacement
editor has the corresponding fence. Mounted unlink/rename reject ancestor
relocation before planning, and metadata changes to the protected root/anchors
are refused. Ordinary write/truncate/metadata callbacks also reject cached
children of the protected parents, including retained references. Existing
cached-child lifetime rules keep those parent identities alive.

Fixed-role account namespace calls carry a separate scoped namespace-operation
flag under `e4_lock`. That flag is independent of `account_allocation`; unlink
does not gain a new allocation-floor bypass. Existing 256 KiB/16-inode floors
and allocator policy remain unchanged. Ordinary callers, including root, cannot
set the namespace-operation flag. Ancestor protection does not freeze unrelated
entries under `/var`; normal creation/removal there remains available.

This intentionally restricts ordinary configuration writes in the protected
parents on explicitly admitted mounts. Future integration must review that
compatibility and the durable package marker/failure route before enabling it.
Trusted kernel raw-device writers and workbench APIs are not an adversarial
security boundary; the mount owner must continue excluding them. No protection
against external offline directory repacking is inferred: remount must validate
the actual records again, not trust a prior count.

## Test commands and evidence boundary

```sh
python3 scripts/test_pkg_namespace_kernel.py .codex-remote-attachments/pkg-namespace-admission-ext4-20261021-134539
python3 scripts/test_pkg_namespace_kernel_regression.py .codex-remote-attachments/pkg-namespace-kernel-YYYYMMDD-HHMMSS
```

These gates copy and hash sources, compile actual EXT4/JBD2/redo with ASan/UBSan
and strict warnings, and operate on private regular-file images only. The
primary matrix uses the retained full 1 MiB `/etc` seeds and separately prepares
a five-record private parent. Admission/rejection write counters start after
mount preparation; mount/replay/freeze writes are separate. Image hashes are
not expected to remain equal across a mounted lifecycle.

Host locks, the memory read mirror and device callbacks remain adapters. The
byte-limit redo validator is synthetic; no production authentication or Ring 3
execution is claimed. The primary host adapter publishes the namespace to test
published-root refusal without starting services. That is not acceptance of
future production validation/boot ordering. The corrupt-intent supplemental
adapter leaves the root detached, then tests that actual redo rejects the bad
record despite successful structural admission.

The regression gate runs the actual existing account service without opt-in,
the actual redo unit campaign, and a freestanding compile of the EXT4 object.
The latter compiles one object only; it does not link/build release media.
The supplemental adapter substitutes a foreign `/etc` inode for the retained
reference denial, avoiding reliance on existing canonical immutability as proof
of the new child-write fence. Original and derived source hashes are retained.

## Results, 2026-10-21

Primary evidence: `.codex-remote-attachments/pkg-namespace-kernel-20261021-140820/`.
**72/72 terminal states PASS** across 1/2/4 KiB blocks x 512/4096-byte sectors:

- 24 admitted redo states: two durable empty-create interruptions, successful
  recovery and a fresh repeated recovery in each pair. Both fences are armed
  before actual account redo. Existing account policy pins are installed too.
- 42 fresh API refusals: workspace OOM, read error, absent private path,
  five-slot `/etc`, four-slot private parent, wrong private-directory mode and
  wrong public role mode. Each clears the report, leaves the fence off and
  asserts zero admission device writes. No setup callback repairs these inputs.
- Six no-opt-in controls: ordinary `/etc` create/remove remains permitted.

Each admitted process checks idempotent readmission, a tainted failed recheck
that preserves the fence, and published-root refusal. Root create/unlink,
rename-in/out, replacement, ancestor relocation/unlink, chmod and ordinary
write/truncate denials assert no device writes; unrelated `/var` creation and
removal succeed. The first admission and successful rechecks also assert zero
writes. Linux e2fsck checks every terminal image; admitted redo stages/canonicals,
metadata and sentinel bytes are read back independently. Completed states have
no publication temporaries or pending record. Host scanner frame maximum is
**640 bytes** under the retained GCC O1 sanitizer build, not a total call-chain
or universal stack bound.

Supplemental evidence:
`.codex-remote-attachments/pkg-namespace-regression-20261021-142535/`.
**6/6 profiles PASS**: actual legacy account service at both sector sizes,
corrupt pending/retained foreign-handle/concurrency probes at both sector sizes,
and wrong role/private-parent owner refusals at 4 KiB blocks/512-byte sectors.
The actual legacy service covers snapshot secrecy, contention and writer death,
stale decisions, allocation failure and canonical immutability without enabling
the new API. Its successful 215-flush update is not a repeated mounted flush-cut
campaign in this continuation.

The concurrency adapter uses pthread exclusion and atomic host counters: an
ordinary root creator starts while admission holds the filesystem mutex, observes
that mutex busy, then finishes with EROFS and zero writes. It does not establish
hardware IRQ exclusion or real kernel SMP scheduling. Retained foreign inode
writes/metadata/truncation are refused after admission. A structurally valid
but digest-corrupt pending record is accepted only by the structural scanner,
then actual redo rejects it with EINVAL and zero recovery writes. The root stays
detached for that semantic rejection; no authentication is started.

Actual redo unit regression: **2586 failure/restart/corruption cases PASS**, 49
operation boundaries. Freestanding EXT4 object compilation with the normal
no-SIMD/no-red-zone flags and strict warnings passes; measured new scanner frame
maximum is **272 bytes** in that build. No full kernel/media link or boot occurs.

Private-parent model evidence:
`.codex-remote-attachments/pkg-private-namespace-20261021-141816/`:
**6141 shapes and 58017 edges PASS**, covering all role placements in five large
records with zero/one/two extra small records, insert/delete and insert-before-
source-delete renames. Every successor preserves geometry; four full records
without another eligible slot fail the negative control. This is Python geometry
modelling, not C execution, arbitrary record layouts or ownership validation.

Retained preliminary attempts:

- `pkg-namespace-kernel-20261021-140235` was stopped during early profiles after
  review found namespace authorization reused the allocator bypass on unlink.
  It is not accepted behavior-neutral evidence. The final implementation uses
  a separate namespace-operation flag and preserves prior allocator policy.
- `140456` fails the bad-private-mode profile in the old harness's setup call,
  before reaching the scanner. Its FAIL/logs remain unchanged. The final harness
  skips setup for negative cases and explicitly tests a missing private path.

Primary inventoried C/header/include/Python source comparison has an empty change
list. Supplemental inputs are verified against that snapshot before intentional
test-adapter derivation; its runner and modified sources remain retained. The
new private model has its own input hashes. This does not claim whole repository
or shared-media byte equality. Concurrent USB work remains outside the commit.

`git diff --check` and all three opt-in target dry-runs pass. There is no new
Ring 3, BIOS/UEFI/SMP, physical, installer or production-trust acceptance. Existing
stack-matrix boot results and their separate preservation checks are unchanged.
Next work is durable package marker/grant/resource ownership and mount failure
routing before a production caller can re-establish this fence on every remount.
The new scanner is not exercised at every mounted journal write/flush cut,
allocation failure site, extent topology or cache/orphan saturation point here.
Selected durable empty-create prefixes are not torn-sector or physical power-loss
tests. Earlier mounted cut campaigns remain separate evidence.
