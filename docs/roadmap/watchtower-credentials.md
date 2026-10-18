# Watchtower service credentials — 2026-10-18

`User` and `Group` now select the real service process identity. Previously they
were parsed/displayed labels while services inherited the root supervisor.
Dynamic service discovery and invalid-config rejection already came from
`efe73a9`; this delivery adds credential switching and enforcement.

## Contract

- Defaults remain `root:root`. Set both directives for an unprivileged service.
  Names and decimal IDs must resolve to existing public account/group records;
  all-digit overflow is rejected, not treated as an account name. Login shell,
  password validity and shadow hashes are not prerequisites for a service.
- Every initial start, manual restart and automatic relaunch takes a fresh
  account snapshot. The generation is checked during preparation and again
  immediately before spawn. Failure leaves no child, no PID and no readiness
  claim. There is no fallback to a legacy unrestricted spawn, including pipe
  allocation/nonblocking setup failures or a missing enforcing adapter.
- `Group` replaces the account primary group. Supplementary memberships come
  from that account's database memberships, deduplicated, at most 16 entries.
  No supervisor groups are inherited. The complete real/effective/saved UID
  and GID triples are installed while child credentials remain private.
- Nonzero UID has zero capabilities. UID zero inherits only the current caller
  mask, never replenishing dropped capabilities. Both CAP_SETUID and CAP_SETGID
  are required for an explicit-identity launch, even for unchanged IDs.
- Target credentials govern cwd, executable and fd OPEN actions. Initial set-id
  image bits cannot replace the selected credentials. The child uses the existing
  secure-entry/descriptor sweep before publication; stdin is `/dev/null`, stdout
  and stderr are its log pipe, and other inherited descriptors survive only when
  explicitly mapped by fd actions. Default cwd is `/`.
- Watchtower retains its own credentials and directly tracks the service PID;
  no intermediate launcher changes the existing group/session lifecycle.

## Additive spawn ABI

`SYS_SPAWN_EXT` keeps v1/v2 at 64 bytes. New `spawn_opts_v3_t` is 144 bytes:
the original base (size 144/version 3, v2 flags/PGID semantics), followed by
an 80-byte inline `spawn_identity_t` containing uint32 UID, GID, group count,
zero reserved word and sixteen uint32 group slots. Unused group slots must be
zero. No nested user credential pointers or capability-grant fields exist.

The syscall validates the complete user range before copying bounded values.
The process constructor derives credentials from a registry snapshot using the
pure `creds_service_identity` helper, then uses the existing private TCB,
credential binding, staged/runqueue publication and rollback paths. It holds
no process lock across VFS work or scheduler publication. Ordinary v1/v2
spawn/set-id semantics are preserved.

## Verification

Commands run:

```sh
make test-watchtower-host
make test-perm-creds-host test-perm-spawn-host
make test-watchtower-identity
```

- Watchtower config, control, coordinator/logging and service/discovery suites
  pass under ASan/UBSan, including a missing/rejecting identity adapter that must
  neither launch nor claim readiness. The identity suite executes the real
  resolver and kernel value transformation: names/numeric IDs, UINT32_MAX,
  overflow, missing users/groups, exact/deduplicated/maximal groups, failed
  account load and stale generation, required capabilities, root mask retention,
  and unchanged output on failure. Account I/O in this host suite is mocked.
- Existing credential suite passes 70,496 transition vectors; existing spawn
  security suite passes 32,768 set-id vectors and the actual sudo adapters.
- Real Ring 3 identity acceptance passes BIOS/UEFI × SMP=1/4 (4/4 boots).
  Services themselves verify all six IDs, exact supplementary groups and caps;
  named/numeric accounts, group override, initial set-id suppression, direct
  privilege-regain denial, shadow denial, explicit-spawn authorization, reduced
  root capability inheritance and fresh restart execution are checked.
  Unknown users/groups and inaccessible executables/cwd fail without readiness.
  The root probe exercises malformed size/version/reserved/flags/group fields,
  unmapped/kernel/wrapping pointers, fd OPEN denial under the target identity
  and v1/v2 compatibility. Crond remains alive.
- Compiler frame limits remain enabled for Watchtower, including the new
  resolver. Large account storage is process-local static/BSS, not user stack.
- The QEMU runner creates a temporary ISO using the existing release archive
  as a read-only base. Its probe/configs are excluded from release staging.
  Release ISO, USB image and archive hashes remain unchanged. No data disks,
  physical devices, private keys or passwords are used by identity acceptance.

## Broader lifecycle regression: not a full PASS

This section retains the initial investigation. The subsequent authorized fix
and new verification are recorded in [SSH shutdown send interruption](ssh-shutdown-send.md).

The existing real Watchtower/SSH lifecycle campaign was also run on a temporary
fixture with this kernel and supervisor. BIOS SMP=1 passed service stop/start,
restart/PID changes, crash recovery/retry budget, stopped-child escalation,
authenticated SSH stop/restart, wire disconnect and reconnect. Its final remote
shutdown assertion failed: OpenSSH reported `Bad packet length` / `Packet
corrupt` instead of the expected RFC disconnect.

The same BIOS SMP=1 campaign against the unchanged release ISO reproduces that
failure after the same preceding checks pass. The baseline ISO SHA-256 is
`aa0291ca985ed5f8b77c4335409fdaf84f57259ad7c26c885cfe0fce89a0b80b`;
the archive is
`a93d90b31ddd93e656e560e58da72186a55748c426fcc71faec747e70f17b224`.
This establishes that the observed failure also exists without the credential
changes; it does not establish its root cause. No SSH transport fix, full
lifecycle-matrix pass or new physical shutdown/reboot acceptance is claimed.
The broader campaign stopped at that failure; the other three lifecycle
firmware/CPU combinations were not run in this investigation.

## Remaining boundaries

This is credential enforcement, not sandboxing. Normal subsequent set-id
execution and intentionally granted account memberships keep their existing
semantics. Account snapshot/check is not an atomic account-update/spawn
transaction, and running services are not retroactively revoked on account
edits. Service configurations still reload only at supervisor startup.
Forking PID-file adoption trust and control-mailbox concurrency remain separate
work. This delivery has no new physical Dell or installed-root acceptance and
does not change hardware-write gates or enable SSH in release configuration.
