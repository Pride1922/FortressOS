# PKG-2 account recovery compatibility candidate

2026-10-21, HEAD `a6cdffa0af271d5547f4321dfd0c68eb484d73e3`. Source audit and
isolated protocol experiment; no account, allocator or boot behavior changes.
Extends [recovery-space review](PKG2_RECOVERY_SPACE_REVIEW.md) and
[transaction review](PKG2_TRANSACTION_REVIEW.md). Installation remains disabled.

## Proposed compatibility rule

Accounts have priority over **new package admission and subsequent package
work**, while an already durable package grant must remain protected. These
requirements coexist only with independently admitted account headroom. Never
borrow the account floor for a package, lend package credit to accounts, or
silently downgrade a durable grant to a best-effort promise.

An account request cannot retain exactly the old success domain while making
free blocks inaccessible in a new package grant: finite space cannot be promised
twice. Compatibility means preserving fixed-role authority, authentication
failure handling and priority, with an explicit new effective-headroom rule on
package-enabled roots. That rule needs approval and actual EXT4 tests before
implementation. Roots without package grants retain current behavior.

Recommended guarantee is **bounded boot account-redo recovery alongside package
space protection**, plus safe live account failures under existing resource
limits. Do not promise unlimited live updates with retained old generations or
progress through full orphan/cache pools. Package roll-forward remains manual
after early grant admission; account redo remains before installed login.

## Existing account protocol and allocation ownership

`src/include/account_redo.h` fixes 11 roles, three input files each <=8,192 bytes
and a 160-byte intent. `src/lib/account_redo.c` implements:

1. Validate input; refuse earlier pending; remove stale stages/temporaries.
2. Write three stages and sync; write/sync temporary intent; rename/sync pending.
3. For each canonical role, remove old publication temporary, write/sync a fresh
   complete temporary, unlink canonical, rename temporary to canonical and sync.
4. Read/hash/validate canonical files, sync, remove/sync pending, clean stages.

Recovery with pending validates all stages then repeats step 3, even if every
canonical byte already matches. Recovery without pending validates canonical
files then only cleans staging. A partial publication or pending-retirement
failure therefore leaves real future allocation work. No allocation-free redo
claim follows from having durable stages.

`src/fs/ext4_accounts.inc` resolves only fixed kernel paths. Its create, write
and rename paths select scoped account bypass under `e4_lock`; `e4_account` in
`ext4_mutate.inc` exempts that bypass from the ordinary 256 KiB/16-inode floor.
`ext4_root_account_headroom` checks raw free totals. `accounts_control` checks it
at ACCOUNT_BEGIN; that check is not a reservation and is not repeated as complete
allocation admission at every UPDATE. A writer can issue multiple updates with
its evolving generation/token before FINISH. Boot `accounts_prepare` does not
use ACCOUNT_BEGIN's preflight. It creates missing account directories through
`ext4_root_account_io`, then performs redo on the detached root.

Account serialization is the non-waiting operation gate in `accounts.c`; there
is no filesystem spinlock held for an entire redo. Ordinary `/etc` configuration
creation can interleave between callbacks. A package session cannot substitute
for account serialization or protect directory slack during those gaps.

Canonical unlinks use the actual orphan protocol. Open old files retain storage;
`e4_orphan_add` has 64 entries and mounted cached nodes have a 1,024-node bound.
Repeated live updates can retain generations. These pins disappear on reboot;
do not carry a live-pin accumulation model across a reboot or imply unlimited
growth past actual pool limits. Boot orphan recovery precedes account redo.

## Protocol experiment, exact results and limits

Executed:

```
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_budget_host.py
git diff --check
```

Successful evidence: `.codex-remote-attachments/pkg-account-budget-20261021-115220/`.
ASan/UBSan PASS for three block-size models (1/2/4 KiB), each with a complete
maximum-byte update, pending retries/final recovery/no-pending replay, and 20
updates retaining old canonical objects. Test runs actual `account_redo.c` and
digest code in a copied source workspace. Model callbacks implement synchronous
whole-file effects and a synthetic length-only validation callback. The 8 KiB
buffers are not asserted to be userdb-valid databases. Copies/ELF/log/source
hashes are retained; inventoried test sources are unchanged during the gate.

| Model result | 1 KiB | 2 KiB | 4 KiB |
| --- | ---: | ---: | ---: |
| Complete update write callbacks | 7 | 7 | 7 |
| Peak named objects, no retained generations | 8 | 8 | 8 |
| Peak data blocks, no retained generations | 57 | 29 | 15 |
| Peak payload bytes, no retained generations | 57,504 | 57,504 | 57,504 |
| Publication writes on each pending recovery attempt | 3 | 3 | 3 |
| Writes on final no-pending recovery | 0 | 0 | 0 |
| Retained old objects after 20 updates | 60 | 60 | 60 |
| Peak objects including retained generations | 67 | 67 | 67 |
| Peak data blocks including retained generations | 529 | 265 | 133 |

The no-pin peak is seven 8 KiB files plus one intent block, including baseline
canonical files: it is not an incremental grant. Recovery is stopped before
pending removal three times, then allowed to finish on attempt four. Those
whole-operation failures model durable pending states, not real device EIO
followed by continued writes on a tainted mount. EXT4 requires restart recovery
after uncertain I/O. The pin model retains each removed canonical object's data;
it does not enforce EXT4's orphan capacity, cache rules or userdb limits.

This experiment excludes directories, inode tables, allocation bitmaps, extent
nodes, journal credits, sector tearing, heap/cache limits and real descriptors.
It proves protocol callback behavior in this adapter, **not** a physical or EXT4
space bound. In particular, 57 data blocks is not proof that the 256 KiB floor
is sufficient for every accepted account layout or recovery state.

Retained failure: `pkg-account-budget-20261021-115200/` compiled successfully but
aborted on the test's incorrect pin-peak assertion (65 objects). It omitted
generations retired during the three-file publication; the correct model peak
is 67 objects (66 full-size data objects plus intent). Fixed that expectation;
no protocol, resource limit or kernel behavior was relaxed. Original log and
copied fixture remain retained; its manifest is INCOMPLETE, not PASS.

## Candidate allocation and admission rules

For blocks and inodes independently, define F as staged free resources, G as
remaining durable package grant, A as independently proved account headroom,
and B as the existing floor (ceil(256 KiB/block_size), 16 inodes).

- Ordinary and package allocations preserve an account headroom H >= max(A,B)
  outside G. Ordinary allocations require F_after >= H + G. Tagged package
  spending d reduces G atomically with its allocation and requires
  F_after >= H + G_after. Unused account credit never becomes package credit.
- Fixed-role account allocations may consume H but must leave G untouched:
  F_after >= G, including directory/extent metadata. This is a proposed
  refinement of the existing scoped bypass, not implemented. The bypass remains
  unavailable to arbitrary paths, root writes or package session descriptors.
- On package-enabled roots, ACCOUNT_BEGIN must assess F-G, not raw F; honor the
  existing floor and the proved next-update requirement. Serialize that admission
  with competing package planning; repeated UPDATEs need renewed space planning,
  because a BEGIN token alone cannot promise several future updates' space.
  Reserve or exclude ordinary consumers while each admitted account operation
  uses its headroom. A preflight followed by unlocked competing allocation is
  insufficient. Do not hold a spinlock across the multi-callback protocol.
- If accounts need headroom before package intent, reject/defer package
  admission. An active account writer or pending account redo prevents new
  package intent. A package request loses contention to account work; it does
  not revoke an account token or invalidate a credential snapshot.
- After package intent, accounts run first within their independent resources;
  further package spending pauses if H cannot be preserved. Previously consumed
  package blocks and G do not become account credit. If assumptions fail, return
  the existing safe error and preserve pending state; never report both a
  guaranteed package recovery and an account success funded from the same G.
- No floor shrink, package use of `account_allocation`, raw-free ABI reinterpretation,
  new public RECOVER operation or account redo wire change is proposed.

Account headroom is a resource envelope, not simply three file lengths. Before
admitting any durable package intent, planner-backed proof must include maximum
live canonical/stage/publication/intent objects; baseline sizes and fragments;
metadata for all affected parents; external extent nodes rebuilt by writes;
pending states after every cut; boot setup of missing directories; and transient
new allocations before old storage can be reclaimed. Define a boot baseline
after orphan recovery and reject package eligibility when the layout is outside
that profile. Do not reject an existing non-package root merely for failing a
new package eligibility test.

One pass is bounded, but repeated interrupted attempts need a replay-invariant
envelope: temporaries from the prior attempt are removed first, and old orphans
are reclaimed on boot before allocating account recovery. Prove that sequence
from actual mounted code and exact allocation sets; do not multiply a one-pass
bound by an unlimited retry count. Retained live generations require separate
current-state accounting and pool checks, not free credit from future closes.

## Boot and failure ordering

Proposed sequence for a package-enabled installed root:

1. Existing replay preview, ownership validation, admitted replay and initial
   orphan recovery retain their established contracts.
2. Validate/reconstruct the durable grant before account directory creation or
   any other allocating consumer. Validate package metadata without borrowing
   account identity or making untrusted paths allocator authority.
3. Admit complete account recovery resources outside G; run the existing fixed-
   role redo and validation before installed authentication/root publication.
4. Reconcile actual free resources after redo; enforce H+G for ordinary allocation
   and package spending. Manual package recovery may run later using G.

Malformed grant, inconsistent allocation accounting or insufficient independent
account-recovery resources prevents writable installed-root publication and uses
the existing independent bootstrap recovery path. It must not silently discard G
or publish the root to run logging first. Account redo failure still blocks
authentication. This does not create a repair writer in bootstrap recovery.
Exact hook/failure routing requires protected boot-contract approval; none is
added. Package-free boot and live ETCFS behavior stay outside this new contract.

## Required implementation gates

1. Approve the compatibility definition and effective-headroom/bypass refinement,
   boot admission boundary and package-only failure policy. This document is a
   candidate, not approval of those runtime changes.
2. Build isolated **actual EXT4** adapters before choosing numeric A/H. Cover
   baseline small/max coherent account sets, fragmented files, nearly-full parent
   directories, missing setup directories, independent block/inode shortage,
   retained canonical fds and orphan/cache/heap/journal saturation. Reproduce
   ordinary floor bypass and distinguish its existing behavior from future grant
   checks. All these new mounted tests are NOT RUN here.
3. Validate atomic G spending and no account access to G; race account admission
   against package admission/spending and logs between every redo callback. Test
   repeated UPDATEs under one token, writer death and deferred cleanup without
   nesting process/filesystem locks. Recheck the effective headroom at each
   accepted operation, and prove admitted resources cannot be stolen afterward.
4. Enumerate write/flush/replay cuts with account and package pending together.
   Reboot twice and interrupt recovery; compare complete allocation ownership,
   exact account bytes/modes, package grant/state and unrelated files. Prove
   account redo can finish before manual package recovery whenever independently
   admitted resources and I/O assumptions hold; safe refusal is a separate gate.
5. Only then private BIOS/UEFI x SMP=1/4 installed NVMe regular-file fixtures,
   Linux filesystem audits and separate source/media preservation checks.

Remaining blocker is an actual EXT4 account resource envelope and approval of
the explicit changed headroom semantics, not an unknown account redo algorithm.
No grant implementation, production trust, install/remove/update, release build,
physical writes, commits or pushes were added by this review.

## First actual EXT4 resource observations

2026-10-21 follow-up, after the user authorized committing the checkpoint and
continuing. The prior protocol/review checkpoint is commit `73c96f5`.
Added `tests/pkg_account_ext4_host.c` and isolated runner
`scripts/test_pkg_account_ext4_host.py`. Neither changes production code.

Executed `wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_ext4_host.py`:
**20/20 profiles PASS under ASan/UBSan**, with Linux `e2fsck -fn`, exact canonical
and unrelated sentinel bytes, and canonical root ownership/0644/0644/0600 mode
assertions. Evidence:
`.codex-remote-attachments/pkg-account-ext4-20261021-120122/`.
Each image is a private 32 MiB regular file; no kernel, release-media or firmware
build. Retained copied sources and per-profile logs/images/dumps accompany the
manifest. Source preservation PASS for its explicit inventory, empty changes.
Shared release-media preservation is not measured by this host-only gate.

Six block/sector geometries (1/2/4 KiB blocks x 512/4096-byte sectors) each run:
small root-only coherent database; 8 KiB synthetic data for each canonical role;
three such updates while retaining all nine prior canonical descriptors. All
fixtures start with small canonical files, 80 unrelated `/etc` fillers and missing
`/var/lib/fortress/accounts` ancestors, so setup directory creation is included.
The byte/pin profiles replace userdb admission with a length-only validator;
they measure real fixed-role backend/redo behavior at its byte cap, not valid
maximum account-service requests or an ACCOUNT_BEGIN/UPDATE acceptance test.

Observed maximum **committed free-count deltas** below the pre-setup baseline:

| Profile | 1 KiB blocks | 2 KiB blocks | 4 KiB blocks | Inodes |
| --- | ---: | ---: | ---: | ---: |
| Small coherent update | 8 | 8 | 8 | 8 |
| Synthetic 8 KiB update | 50 | 26 | 14 | 8 |
| Three updates with retained descriptors | 100 | 52 | 28 | 16 |

Results are identical for both sector sizes. Sampling follows each write,
remove, rename and sync callback, plus setup. It includes committed filesystem
metadata effects but cannot observe temporary gross allocation demand inside
one planner transaction that also frees blocks. Thus these are measured deltas,
not sufficient admission budgets. One no-pending recovery follows each update;
the host probe checks every final role and checks old descriptor bytes before
close. Nine pinned orphans persist until close; close/sync reclaims all nine.

Two additional 4 KiB-block profiles (both sector sizes) really fill an ordinary
file in batches, then one block at a time, until ENOSPC. They assert free blocks
equal exactly the 64-block/256 KiB account floor. Fixed-role synthetic account
publication still succeeds and reaches 53 free blocks: an 11-block delta below
that floor, with five additional inodes. These deltas use the **post-fill,
post-setup baseline** and are not comparable to the table's baseline. Linux
audits pass. This is observed ordinary-floor bypass, not enforcement of a package
grant. No forged superblock count or lowered reserve is used to reach exhaustion.

Retained first campaign `pkg-account-ext4-20261021-115927/` passes 18/18 profiles
before adding floor cases, old-descriptor byte assertions and explicit Linux
ownership/mode assertions. It captures Linux metadata but does not assert it;
the expanded 120122 campaign does. No failed/partial run occurred in this actual
EXT4 continuation; the earlier protocol-model assertion failure remains retained.

This advances the actual EXT4 gate, but does not choose numeric A/H. Missing:
adversarial fragmentation/extent rebuild demand, precisely saturated directory
slack, inode-only exhaustion, heap/cache/orphan/journal capacity, cut/replay tests
with account pending and future package grant together, and race admission.
Host callbacks use stdio/fflush, not physical-device persistence; no power-loss,
hardware timing, mounted package-grant or BIOS/UEFI acceptance is claimed.
An 80-file parent is a concrete fixture, not worst-case directory proof.
Installation and production trust remain disabled; no kernel/account ABI,
boot ordering or scoped bypass behavior changed.

## Extended limits and interrupted account recovery

2026-10-21, continuation from `56efd47`. Added resource/fragmentation and
flush-failure cases to the same private actual-code runner, plus
`scripts/test_pkg_account_recovery_cuts.py`. No production source change.

Invocations:

```
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_ext4_host.py
wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_account_recovery_cuts.py .codex-remote-attachments/pkg-account-ext4-20261021-121959
```

Evidence campaigns are `pkg-account-ext4-20261021-121959` and
`pkg-account-recovery-20261021-122349` under `.codex-remote-attachments/`.
The first extends baseline fixtures to 2,048 inodes, leaving room for real cache
and inode exhaustion. The second copies that campaign's source workspace and
adds a narrowly asserted test-only hook immediately before account redo recovery;
its derived C source/hash is retained. It changes no production recovery code.

Results and scope:

| Gate | Cases / observations |
| --- | --- |
| Ordinary resource profiles | 33 profiles: previous 20 plus directory growth across all six geometries, two 1 KiB-block fragmentation profiles, two combined heap/journal/orphan profiles, two inode-only profiles, and one pinned-cache profile |
| Interrupted publication | 215 actual update flush boundaries x two device sector sizes = 430 cases; two independent clean remount/recovery passes per case |
| Interrupted account recovery | 172 recovery flush boundaries x two sector sizes = 344 cases; two independent clean remount/recovery passes after each interruption |

All use ASan/UBSan. Linux e2fsck and exact old/new canonical bytes are checked
after **each** of the 1,548 clean recovery passes; update-cut cases reject a mixed
old/new account set. Recovery-cut cases start from a pending intent verified by
Linux `debugfs` after update flush 60 and require the complete new byte set.
Ordinary profiles retain the prior sentinel/ownership/mode checks; cut campaigns
check filesystem consistency and account bytes, not all unrelated metadata.
Successful transient cut images are hashed then removed only after both audits;
logs and exact byte dumps remain. Seed/measured and ordinary-profile images are
retained. Failure paths retain their current image. This is not a full allocation
bitmap differential oracle or sector-tear campaign.

The fragmentation probe creates eight nonadjacent 1 KiB extents with real
separator allocations and asserts depth 1. Planning an existing-data overwrite
then stages at least two E4_NEW-role images (data and new extent metadata) while
recording old tree frees. The plan is aborted, not published. This demonstrates
temporary tree-allocation demand, rather than interpreting net free counts as
gross demand. It is a shared filesystem planner proof, not a fragmented account
file publication or a bound over every accepted extent topology.

Directory profiles create real names until `/etc` grows by exactly one block,
then perform redo. They prove growth in each geometry, not protection of future
account directory slots against an arbitrary competing writer.

At 4 KiB blocks, both sector profiles test: first host heap allocation failure
on account staging (ENOMEM); a privately shortened journal capacity view
(ENOSPC); and 64 real open/unlinked files filling the orphan table (EFBIG on
canonical removal). Each denial asserts zero device writes, restores/releases
the fixture condition and proceeds with successful redo. These are selected
resource boundaries, not exhaustive failure at every heap allocation.

Inode-only tests really allocate empty ordinary files until the 16-inode floor,
assert over 1,024 free blocks remain, then successfully perform fixed-role redo.
The cache test creates actual legacy entries to fill all 1,024 cached nodes,
marks the remaining fixture nodes as legacy-pinned, checks staging EFBIG with
zero writes, releases those private legacy flags and verifies retry succeeds.
No production pool size, floor or bypass is relaxed.

Failure model: the chosen flush returns failure, subsequent writes/flushes are
refused, and the process closes its private stdio file before fresh remount.
Already accepted writes may all persist. This exercises an allowed uncertain
I/O outcome with real JBD2 replay and account redo; it does not emulate physical
power loss, all cache persistence subsets, reordered/torn sectors, faults during
initial journal replay, or a kernel reboot. Recovery interruption is specifically
**account redo after mount preparation**, followed by fresh journal replay.
Both protocols still use synthetic 8 KiB validation for byte-cap tests.

Final results: **463/463 primary cases PASS** (33 resource profiles + 430
publication cuts), **344/344 recovery-interruption cases PASS**, and
`git diff --check` PASS. No failed or partial campaign occurred in this
continuation; earlier failures remain retained with their original status.
Primary manifest source_preservation=PASS with an empty change list for its
inventoried C/header/include/Python sources. The recovery runner is a deliberate
new source file added after that inventory began; its derived source is recorded
in its own campaign. No whole-repository or shared-media hash PASS is claimed.
Added opt-in `make test-pkg-account-ext4-host` for the primary gate; the recovery
gate takes the retained primary campaign path as shown above. Documentation and
Makefile edits following the gate are intentional. No release media was built.

These gates do not freeze numeric A/H or enable a grant. Remaining proof includes
full gross planner bounds over accepted account layouts, allocation ownership
differentials, exhaustive allocation/replay fault coverage, namespace/admission
races and combined account/package pending once the grant contract is approved.
No package grant exists to test yet. Boot/physical acceptance remains NOT RUN.
