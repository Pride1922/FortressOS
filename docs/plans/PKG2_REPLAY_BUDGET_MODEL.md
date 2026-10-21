# PKG-2 remaining account budget across replay: finite model

2026-10-21, continuation from `7797f0e`. Pure model and specification only;
no account, allocator, boot, grant or installation implementation changes.
Extends [allocation bounds](PKG2_ALLOCATION_BOUNDS.md).

## Result

The finite model establishes a remaining-budget rule for its admitted layouts:
charge every allocated object once; retain deleted objects until verified orphan
reclamation; move an object on rename without allocating another copy; recompute
future demand from the recovered state rather than resetting a free-space grant.
An independently retained package grant is preserved by that rule in the model.

It also finds a **restart-specific namespace counterexample**. A layout can
have enough aggregate free storage and complete an uninterrupted pass, yet
strand recovery when later publication temporaries occupy the directory's last
suitable slots. A byte-only remaining budget cannot guarantee progress.
Production eligibility must include restart-safe namespace capacity/exclusion.

No universal guarantee or production reservation size is frozen. The model is
finite exhaustive exploration of a declared family, not actual EXT4 execution
or a machine-checked proof of the kernel's implementation.

## State and transitions

`tests/pkg_account_replay_model.py` represents existing valid pending recovery:

- Three canonical files (old or new), three complete authoritative stages, three
  publication-temporary roles (absent/created empty/complete), and pending intent.
  Objects have unique role ownership; no links/foreign aliases or corrupt input
  are admitted by the abstraction. Renaming moves the existing object.
- `/etc` directory records as ordered `(record_bytes, used_bytes, role)` slots.
  First-fit insertion, occupied-record splitting, deletion without coalescing,
  and one-block growth follow `e4_dir_edit`. Private stage directory exists and
  never grows during pending recovery. Directory extent cost uses the dense
  maximally fragmented canonical-tree formula from the allocation review.
- Used blocks/inodes include linked files, the two existing directories, and
  retired unlinked objects. Source/stage data is already allocated, not credit.
  Unrelated immutable storage is a constant baseline outside the account model.
- Publication follows actual account role order: remove prior temporary,
  reclaim eligible orphan, create empty, write complete bytes, unlink canonical,
  reclaim eligible orphan, rename temporary into canonical. Pending retirement
  and staging cleanup follow full publication.

Each create/data-write/unlink/rename is an abstract durable transition. It exposes
created-but-empty temporary files and unlinked-but-not-reclaimed objects, rather
than treating the entire backend callback as atomic. A directory-growth transition
charges the new data block and new extent tree before old tree frees.
Reclaim is modelled atomically; real partial right-edge reclamation can only
reduce this charged storage before boot account work. No live pinned objects
are reclaimed by this assumption; the model is specifically unpinned boot redo.

Restart is possible from every nonterminal state. It retains linked objects,
directory records and growth; clears only unpinned retired objects; restarts
publication at the first role if pending exists. Without pending it starts
cleanup after the assumed coherent canonical set. Replay twice frees nothing
twice. Sync/readback validation are represented by their durable before/after
states, not by a model of JBD2 sectors or I/O failure handling.

## Remaining-budget rule

For blocks and inodes independently, let U(s) be currently occupied account
resources, B the unchanged unrelated baseline, G the separate durable package
grant, and C total physical capacity. Actual free resources are:

`F(s) = C - B - U(s)`

Compute P as the least fixed point over the finite transition/restart graph:

`P(s) = max(U(s), every transient allocation peak on an outgoing edge,
            P(t) for every allowed successor t)`

Then required remaining account headroom is:

`R(s) = P(s) - U(s)`

Admit only if `F(s) - G >= R(s)` and namespace capacity can finish from every
reachable state. With initial `C >= B + G + P(initial)`, each model transition
and transient allocation peak preserves G. After actual reclaim, free resources
increase exactly by reclaimed ownership; R may increase too, without inventing
credit. Already allocated stages remain in U. A consumed source name does not
refund the moved destination's storage. Directory growth stays charged after
restart even if file data is reclaimed.

The computation uses separate componentwise block/inode maxima. Those peaks
need not occur at the same step; reserving both maxima is conservative. The
model fixes G at arbitrary positive test values (37 blocks, 11 inodes); these
are invariant witnesses, **not proposed package quotas**. It models no concurrent
grant spending, ordinary writer or grant-record provisioning/authentication.

Two negative arithmetic controls show why weaker rules fail: adding ten blocks
of already allocated stage data to free credit breaks `used + free = capacity`
and falsely admits insufficient account headroom; a zero-floor account bypass
can spend into G. These are expected analytic controls, not kernel regressions
or exhaustive mutation testing of the implementation.

## Exploration and counterexample

Executed:

```
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_replay_model.py
```

Final evidence: `.codex-remote-attachments/pkg-account-model-20261021-130646/`.
**PASS**, with 324 profiles, 1,165,689 explored state instances and 2,324,700
edges. Counts sum independent profiles; they are not globally distinct states.
Source preservation PASS for the explicit model/reference-source inventory;
source copies and JSON results/witnesses remain alongside the manifest.

Profiles combine 1/2/4 KiB blocks, compact/tight/mixed record layouts, initial
1/4/5 directory blocks, zero/one extra block of permitted growth, and all six
initial canonical-name orders. Publication order remains fixed passwd/group/
shadow, as in the actual redo engine. Immutable filler records all satisfy
minimum eight-byte/four-byte-aligned lengths. The model does not instantiate
every 1 MiB directory, every byte length or every extent topology.

216 profiles are admissible under model rules; 108 are rejected. All compact
profiles pass; tight/mixed profiles with no permitted growth reject. Rejected
profiles are successfully detected inadmissible layouts, not failed test runs.
Every admitted state has a route to completion and no blocked forward step;
completion requires interruptions eventually stop, not infinite-crash liveness.
Peak additional headroom within this finite family is 53/23/11 blocks at 1/2/4
KiB, and three inodes. These observations do not replace the wider U/R bounds
or define production A/H.

A saved mixed-layout witness (1 KiB, one directory block, no growth) has:

1. Complete passwd and group publication; create an empty shadow temporary;
   interrupt before writing it.
2. Restart, publish passwd again, create an empty group temporary; interrupt.
3. Restart at passwd. Neither later empty temporary is removed yet because
   the real protocol removes temporaries per role. No suitable publication slot
   remains for passwd; growth is disallowed. Canonical passwd/group are new,
   shadow is old, authoritative stages and pending remain valid.

The witness is recorded as program-counter steps and role states in
`blocked_witness`. "No growth" is a model namespace limit, not a modified EXT4
limit. A full real directory or protected admission growth budget could impose
such a constraint; this exact witness still needs actual EXT4 reproduction.
The counterexample does not authorize changing account redo cleanup order.

## Retained qualifications

Initial campaign `pkg-account-model-20261021-125919` completed with a raw PASS
before an attempted interrupt, but its seed generator could produce a four-byte
directory record. Its original sources/result remain unchanged and a separate
qualification.json marks it **not accepted as valid-layout evidence**. It was
not actually interrupted. Corrected `130047` passes the valid-seed checks;
`130304` repeats them and adds explicit counterexample traces; final `130646`
also explicitly checks uninterrupted completion versus restart admissibility. Numeric
counts happen to match; only corrected runs support the valid-layout claims.

This is Python modelling, so no ASan/UBSan, Linux oracle, Ring 3, firmware or
physical acceptance is claimed. The retained C files are reference inputs,
not code executed by this gate. The preceding actual-code campaigns remain
separate evidence, not a proof that this abstract transition graph refines EXT4.

## Concrete next gate

Reproduce the saved two-interruption namespace witness using the actual mounted
account backend and inspect exact allocation/slot ownership after each remount.
Then compare the actual replay states and allocation peaks with this model.
Choose and review one of: an admission profile proven to retain publication
capacity under all allowed restarts/writers, or a separately reviewed persistent
namespace/capacity mechanism. Holding more raw free blocks alone is insufficient.

A future validator must derive U and namespace shape from validated retained
objects, reject malformed/foreign aliases, and prove its remaining-step cost
matches actual planners. It cannot fabricate G from filenames or reset R from
a constant on boot. Live pins, heap/cache/journal limits, allocation groups,
sector-level replay faults, arbitrary privileged writers and concurrent account/
package operations need separate admission/refinement tests.

The dynamic R rule is now concrete for the model. Runtime grant/boot integration,
production A/H and trust provisioning remain disabled or unresolved; no kernel
contract, release media or physical device was changed.
