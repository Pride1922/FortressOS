# Multiple installed application packages

Implementation milestone, 2026-11-01. The runtime implementation and completed
automated acceptance are recorded in [the checkpoint](../roadmap/pkg-multiple-installed.md). The single-package laptop flow,
including rot13 upgrade/reboot after the offline kernel update, is user-confirmed
PASS. Manual previous/recovery kernel boots remain separate.

## Constraints before this milestone

The ordinary backend reads one 8192-byte `/var/lib/pkg/install.v2` record.
`px_record_read`, `px_prepare`, status and upgrade activation all target it.
The CLI's `pkg_previous_source` also reads this singleton to find the predecessor
artifact. Installation uses shared `/var/lib/pkg/work-v2/mNN` names; upgrade uses
`/var/lib/pkg/upgrade-v3/upgrade-BUILD`, which collides between packages sharing a
build number. Package data already lives below package-specific public paths.

## Required behavior

### User workflow

The agreed CLI replaces the separate catalog/download prerequisites:

```sh
sudo pkg install TOOL    # Refresh catalog, download, verify and install
sudo pkg upgrade TOOL    # Refresh catalog and upgrade one installed package
sudo pkg upgrade         # Show available upgrades and ask before proceeding
sudo pkg upgrade -y      # Upgrade all installed packages without prompting
```

Neither `pkg update` nor `pkg get` is required for installation or upgrade. Remove
them from the normal documented workflow and help; explicit-directory `pkg fetch`
may remain a diagnostic command. This is the target interface, not current-image
behavior.

Install and targeted upgrade refresh and verify the catalog automatically, resolve
the requested package, obtain any required artifacts, and reverify in the privileged
backend. Cached content may be reused only after verification against the selected
catalog. An identical installed build/hash is a verified no-op; install of a
different installed build directs the user to upgrade. Upgrade rejects rollback
and equal-build substitution. Download or verification failure preserves the
current installation.

Without a package name, upgrade enumerates installed packages and refreshes the
catalog once for the invocation. Show each available upgrade with old/new versions
and a total, then request confirmation before downloading artifacts or mutating
packages. `-y` skips only confirmation; all authorization, compatibility, signature,
hash, rollback and ownership checks remain mandatory. Without `-y`, noninteractive
input must fail clearly rather than implicitly approve. No available upgrades
reports that installed packages are current and succeeds without prompting.

Only installed packages are eligible; unrelated catalog entries are never installed.
Missing catalog entries are reported as unavailable and left installed. Process
eligible packages sequentially in a deterministic order using the verified catalog
snapshot. Each package activates atomically; the batch is not one transaction.
Stop on the first download, verification or mutation failure, return failure, and
report completed, failed and unattempted packages. Completed upgrades remain
installed. Recheck each installed record before activation; concurrent changes must
not silently alter the confirmed upgrade set. Do not hold filesystem locks or a
package mutation session while awaiting user confirmation or network downloads.

### Inventory and transactions

Install several independent applications, upgrade one, and retain every other
application and its inventory across reboot. Keep the existing single-writer
service credit and per-package single-command activation transaction. Do not add
concurrent writers, dependency resolution or a multi-package atomic transaction.

Use bounded package-specific inventory and private staging paths, with validated
package identities. Choose and document the package-count bound before runtime
implementation; exhaustion must refuse before publishing files. Retain the existing
one-command and 15-data/documentation-file bound per package.

Preserve the existing rot13 installation through a durable, restartable migration
from `install.v2`. Migration must validate the old record and preserve its build,
artifact, catalog and trust floors. Interruption must never select two conflicting
authoritative records or forget an installed command. No reinstall or root format.

Enumerate and validate records with bounded reads. Fail closed on corrupt records,
duplicate package identities or conflicting command ownership. Refuse base-command,
foreign-file and cross-package collisions without replacing existing files.
Catalog rollback/substitution protection must apply across packages from the same
repository, including new installations; a fresh package record cannot reset it.

Update predecessor lookup, installation, upgrade, status, recovery and CLI installed
listing together. Targeted info must distinguish the requested package from other
installed packages. Preserve the v1–v3 syscall wire layouts; any new query operation
needs an explicit versioned contract and actual-code ABI tests.

Keep executable replacement and that package's inventory publication in the same
JBD2 transaction. A failure or replay involving package A must preserve B's files,
inventory and settings. Recovery must identify incomplete operations across all
records before admitting a new mutation. Preserve existing filesystem locks,
account reservations, authorization and uncertain-I/O fencing.

## Acceptance

- Install directly from an empty cache without `pkg update` or `pkg get`; verify
  automatic catalog refresh, authenticated downloads and privileged reverification.
- Test targeted upgrade and all-installed upgrade with mixed current/outdated
  packages and unrelated catalog entries. Verify the preview, confirmation refusal,
  noninteractive refusal, `-y`, no-upgrade case and missing catalog entries.
- Fail a later package in a batch: verify earlier upgrades persist, later packages
  are untouched, exit status is failure and the summary identifies all outcomes.
  Exercise download/signature failures, stale installed records and reboot/retry.
- Migrate the existing single-package root; reboot and verify rot13 unchanged.
- Install two distinct applications with the same numeric build; run both and
  verify their individual installed records before and after reboot.
- Upgrade A twice while verifying B byte-for-byte, then upgrade B while preserving A.
- Reject duplicate command ownership, foreign files, malformed records, package
  capacity exhaustion, bad signatures, catalog rollback and build substitution.
- Inject staging and activation interruptions for A; replay and retry while
  independently checking B and filesystem consistency with Linux tools.
- Verify low-space refusal, verified same-version no-op, settings preservation and
  predecessor lookup for each package. Record existing remove support accurately;
  the ordinary backend's remove callback currently returns EOPNOTSUPP.
- Run focused actual-code host sanitizer tests, the normal kernel/CLI build and
  disposable installed-root QEMU tests. Deliver a matching kernel update and laptop
  commands; claim physical acceptance only after the user's test.

Desktop packaging follows this milestone. Retained-version garbage collection and
full repository-authenticated base OS updates remain separate work.

## Implementation decision

The implemented inventory bound is 16 packages. The legacy record remains slot
zero, preserving it in place; additional slots have separate bounded records and
staging directories. See [implementation and verification](../roadmap/pkg-multiple-installed.md)
for the transition, signed build-number preview, recovery and evidence boundaries.
