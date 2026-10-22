# Private account-capacity certificate in replay preview

2026-10-22. Extends [grant preview](PKG2_GRANT_PREREPLAY.md) in the copied-source
host harness. Production integration, installation and trust provisioning remain
disabled. No public API or kernel source is changed.

`capacity_certificate(fs, workspace, out)` now holds the shared role/map
calculation. Its callers supply exclusion and validated account-directory
anchors. The existing mounted wrapper retains its filesystem lock, namespace
fence, healthy/idle engine checks and refusal of account readers or orphans.
The preview wrapper instead owns an unpublished constructor engine exclusively
and performs unlocked reads through the actual JBD2 preview device.

Preview derives the root, `/etc`, `/var/lib/fortress/accounts` ancestor identities,
ownership and modes from that view. It uses the existing namespace validators to
check fixed roles, links, aliases, reserved name-slot geometry and file sizes.
The shared calculation checks dense initialized mappings, counts actual data and
extent-tree blocks and counts every present role inode, including empty durable
creates. It computes R_acc=C-A with C=(83,11)/(38,11)/(20,11) for 1/2/4 KiB.
Outputs remain unchanged on failure. Workspace and temporary maps belong to the
preview and are released on its normal disposal or rejection path.

No `fs->engine` pointer, trusted-root state, namespace anchors or fence is
fabricated/published to run this calculation. Only after successful replay does
the normal root caller publish its actual namespace fence. The test rederives
the mounted certificate and requires exact equality with the preview result.

Before replay, admission reads actual preview free counters F and the validated
durable remaining grant G, then requires F-G >= max(A0,R_acc) in both dimensions.
A0 is the existing ordinary account floor (256 KiB and 16 inodes), unchanged.
For this profile A0 already exceeds all possible R_acc values. This work adds
structural certification before writes, not a tighter numerical threshold on the
current geometries. Local arithmetic probes exercise exact equality, +1 grant
and subtraction-underflow rejection independently in each dimension; their F/G
values are explicit test inputs, not forged filesystem counters or a full-disk
mount campaign.
This retains the earlier private constructor's A0 admission restriction; it does
not establish recovery admission below A0 even when R_acc would fit. Production
account-priority and low-space recovery policy still require review.

The service runner's `--pre-replay` gate now includes:

- Normal, interrupted-account-recovery, IDLE and RESERVED restart paths through
  grant and capacity preview, followed by mounted-certificate equality checks.
- Wrong canonical role mode and an 8193-byte canonical file, both structurally
  healthy Linux fixtures. The size fixture grows the existing inode with the
  actual EXT4 planner under an explicit test-only account allocation bypass;
  it preserves the certified namespace geometry and is not a legitimate account
  operation. Admission never repairs either invalid-policy fixture.
- Actual certificate workspace-allocation failure after the grant checks, with
  ENOMEM, unchanged output and zero target writes/flushes.
- An actual account empty `group.publish` creation interrupted after JBD2 commit
  and before checkpoint by a copied-source error injection. A new process first
  verifies a nonempty committed journal, then previews/replays it. Preview must
  count the new inode: R blocks stay unchanged and R inodes decrease by one.
  Linux e2fsck, the empty role's size/i_blocks/links, and independently derived
  C-A from canonical file i_blocks corroborate that result.

The grant, namespace and certificate checks all precede the constructor's first
replay write. Existing account recovery still validates database semantics and
intent coherence before authentication/publication. This capacity certificate
does not replace those checks. Baseline test enrollment is a separate known
unenrolled setup path; no production absent/partial-enrollment policy follows.

Limits: one selected durable-commit boundary, not an all-write/flush cut sweep;
no heap/journal/device-liveness reservation; no charged-orphan recovery; no
authenticated boot selector, competing kernel callers, actual scheduler or
Ring 3 acceptance. Runtime, Linux oracles and compiled-input preservation are
reported separately. No new boot or release-media result is claimed.

Run `make -f tests/pkg_grant_candidate.mk test-pkg-service-prereplay` in WSL.
The maximum-byte account-capacity regression remains `test-pkg-account-capacity`.
Exact invocations, evidence, limitations and retained failures are in the roadmap.
