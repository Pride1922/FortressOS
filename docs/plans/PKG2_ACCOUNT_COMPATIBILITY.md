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
