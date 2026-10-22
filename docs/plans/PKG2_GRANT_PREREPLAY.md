# Private grant validation before journal replay

2026-10-22. Copied-source host candidate only. Production integration,
installation, trust provisioning and release-media builds remain disabled.

The existing constructor analyzes JBD2, then opens an unpublished read-only
preview of the committed replay result. The private overlay invokes grant
validation after the existing filesystem/ownership preview and before disposing
that preview or calling `jbd2_replay`. A rejection uses the constructor's normal
unwind; there is no writable fallback, publication or record repair.

The validator receives the explicit preview engine. It does not publish an engine
through `fs->engine` or fabricate mount/trust flags. It reuses the private exact
marker/ledger/table parser, fixed inode identity/mode/link/map checks, allocation
ownership audit and conserved grant checks. Expected identity is retained outside
the target fixture, never adopted from the records being validated. This is a
synthetic host expectation, not an authenticated production enrollment selector.

Preview is exclusively owned by the constructor and runs unlocked, matching
`jbd2_preview_read_sector`. Published mount checks retain ordinary filesystem
exclusion. The real preview and original JBD2 analysis/replay code are unchanged.

The runner obtains actual committed, uncheckpointed block charge and refund
transactions using the existing sector callback fault model. Journal preview
must validate the committed grant/inode/bitmap state even while home blocks are
older. Rejection tests assert the `grant-preview` stage, no published mount,
complete constructor release, zero underlying write/flush events and unchanged
fixture bytes. Valid recovery is followed by repeated interrupted replay/restart
tests and independent Linux audits.

The service runner's opt-in `--pre-replay` overlay also loads its external `.ctx`
expectation before `ext4_prepare_root`. Its preview check validates fixed record
objects, the actual filesystem UUID, the existing lifecycle ownership profile,
the fixed `/pkg` name bindings,
IDLE/RESERVED phase and remaining grant against free counters plus the existing
ordinary account floor. A separate fresh-process wrong-marker expectation must
reject before writes and leave the entire image hash unchanged. Normal account
interruption/recovery then runs through the same private admission boundary.
Baseline synthetic enrollment is explicitly separate: the known unenrolled
fixture is prepared before the test creates records; that setup is not a
production policy for missing or partial enrollment.

The exhaustive replay-cut gate remains the original one-linked-payload RESERVED
profile. Account-service recovery tests use checkpointed account interruption
boundaries, not an account JBD2 cut sweep. The subsequent
[account-capacity preview](PKG2_ACCOUNT_PREVIEW.md) now derives that certificate
before replay as well, sharing the mounted role/map calculation. Charged-orphan cleanup is not
covered at this boundary: the service preview refuses any pending orphan cleanup
until its refunds are coupled to the grant protocol. The test profile still
assumes its fenced package namespace; fixed name checks are not a complete
production pending/state/trust admission policy. Independently
authenticated selectors and concurrent caller dispatch remain open. No Ring 3,
boot, scheduler, SMP, physical-device or production admission acceptance follows.
Input preservation is reported separately from recovery and Linux results.
The pending-orphan refusal branch is source-reviewed rather than fault-injected
by the integrated service campaign.

Run `make -f tests/pkg_grant_candidate.mk test-pkg-grant-prereplay` in WSL.
Run `python3 scripts/test_pkg_service_integration.py --pre-replay --block N`
for the separate integrated service gate (N = 1024, 2048 or 4096).
The combined invocation is the standalone make target `test-pkg-service-prereplay`.
Exact evidence and retained failures belong in `docs/roadmap/pkg1.md`.
