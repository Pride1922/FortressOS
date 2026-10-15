# Persistent root Phase 3 contracts

2026-10-11. Based on checkpoint `abc6625` and the Phase 0–2 contracts.
Implementation and acceptance use disposable regular-file NVMe fixtures only.
Live and installer artifact families remain separate; installer construction,
raw leases and physical acceptance remain Phases 4/5.

## Account publication and authentication

The chosen no-replacement redo protocol is implemented by `account_redo.c`.
It accepts eleven fixed file roles, never intent paths. The 160-byte little-endian
intent has magic FOSACCT1, nonzero generation, three bounded lengths, zero reserved
bytes, three SHA-256 digests and SHA-256 over the preceding 128 bytes. Files are
bounded at 8192 bytes; databases retain 16 records and existing parser limits.

The root-owned transaction directory `/var/lib/fortress/accounts` is 0700.
All immutable staging files and intent files are 0600, including copies of public
databases. Publication temporaries live in `/etc` with final 0644/0644/0600 modes;
shadow is never world-readable. Stage all three complete files and sync before
creating/syncing temporary intent. Rename intent into the absent pending name and
sync. For each canonical file, write the complete temporary, sync, unlink the old
name, rename into the absent canonical name, sync. Read back/hash/validate all
canonical bytes and sync; remove pending and sync before reclaiming staging.
Every error remains an error. There is no rollback after uncertain publication.

Boot validates installation identity, then resolves account redo against the
detached root before canonical validation or installed-process publication.
Absent pending requires a coherent canonical set before abandoned staging cleanup.
Malformed intent, wrong metadata, absent/corrupt staging, I/O failure or unsafe
homes blocks root publication and uses independent bootstrap recovery.
This does not make canonical filenames atomic for arbitrary privileged readers.

Canonical account files and `/etc` removal/rename remain protected even against
ordinary root writes. A fixed-role EXT4 adapter bypasses only the required inode
policy inside each specific kernel filesystem operation, preserving lock ranks,
cache publication and journal barriers. There is no global or per-process raw
filesystem bypass. `/etc` allows ordinary non-account configuration changes;
reserved canonical and publication names remain protected, including during gaps.

The account service serializes operations with a non-waiting operation gate.
No spinlock spans filesystem I/O or scheduling. Copied snapshots hold exclusion
only until all requested files are captured; no reader owns a lease while typing.
Competing operations/writers return EAGAIN. BEGIN checks 256 KiB free-space
headroom (not a reservation), credentials and expected generation. Writer tokens
are process-owned, monotonic, non-inherited and fail closed on exhaustion.
Snapshot generations are boot-local; no process or authentication decision
survives reboot. The durable intent generation identifies a bounded redo record.
Writer death is deferred safely if an operation is running: no pending cancels
the lease only after coherent canonical validation/barrier; pending or uncertain
reads disable authentication until restart. Explicit login failure latches off
independently and cannot be cleared by writer cleanup.

## ABI refinement

Use reserved ACCOUNTCTL number 72 and the Phase 0 64-byte v1 request layout.
SNAPSHOT/BEGIN/CHECK/FINISH retain their specified shapes. RECOVER is never
available through Ring 3. Two explicit operations refine the original reservation:

- UPDATE=5 takes the writer token, expected generation, three copied complete
  database buffers/lengths and zero flags. The kernel owns staging and redo;
  users cannot publish canonical account files directly. Root/CAP_SYS_ADMIN is
  required. It invalidates older snapshots before the first attempted mutation,
  including when an uncertain update eventually returns an error.
- STATUS=6 uses empty fields and returns installed-root generation without
  authenticating; this selects configuration precedence even with auth off.
  Boot-published namespace identity is queried outside the account operation
  gate; generation publication/read is atomic. Writer contention cannot select
  live fallback paths for an installed consumer.

SNAPSHOT with shadow additionally requires euid 0 and CAP_DAC_READ_SEARCH.
Public snapshots never copy shadow to user memory. FINISH validates pending
absence, canonical coherence and root barrier before releasing the writer.
CHECK is the privilege-decision linearization point. Login and sudo apply only
credentials from the checked copy; changed generations retry/deny the decision.
Temporary writer contention waits at login, while disabled authentication fails
to recovery. Live account loading retains strict readonly metadata admission.

INSTALLSEED=73 supplies 1..256 bytes to privileged euid 0/CAP_SYS_ADMIN callers.
CPUID-gated RDSEED has 64 attempts per word, then advertised RDRAND has 64 attempts.
No TSC/MAC seed fallback; failure publishes no bytes. No installer write privilege
is provided. Normal boot does not require RNG. QEMU account tests advertise
`-cpu max`; physical RNG support remains unverified.

## Tools and machine configuration

`passwd [USER]` changes an installed non-root password. Root remains locked.
The root-owned 04755 tool uses secure-entry validation, authorizes a non-root
caller only for their own UID and requires their old password. New passwords
must match, contain 12..128 printable ASCII bytes and use fresh 16-character
crypt-alphabet salt with `$5$rounds=100000$`. Input is raw/no echo, overflow is
drained, cancellation fails and plaintext buffers are wiped. `sudo passwd USER`
is the administrator path. Recovery credentials remain entirely independent.

`useradd NAME UID` is the bounded provisioning primitive: privileged real/effective
root only, unique UID/GID >=1000, matching private group, `/home/NAME` mode 0700,
matching ownership and `/bin/shell`. It requires a confirmed password; no default,
passwordless or skip mode. It grants no wheel membership. A failure after home
creation may leave an unreachable home; it never removes a home after uncertain
account publication. Retrying that name requires explicit recovery/cleanup.

Installed `/etc/network/network.conf` and `/etc/resolv.conf` are authoritative default
paths. Missing/malformed installed configuration does not fall back to `/mnt`.
An explicit `ifup /mnt/.fortress/network.conf` remains an intentional import/apply
path; live defaults retain `/mnt/.fortress` compatibility. `hostname [NAME]`
reads/writes bounded `/etc/hostname`, defaults to fortress when absent, and checks
write/close/sync results. It does not introduce a hostname networking protocol.
Installed history is `$HOME/.fortress-history` (0600); live history retains
`/mnt/.fortress/history`. Homes are separate per user with no implicit migration.

## Verification

`test-account-redo-host` tests the actual byte engine with a labelled memory
durability adapter and interruption/restart/corruption cuts. It does not establish
EXT4 journal sector-tear behavior. `test-accounts-persistent-host` adds actual
EXT4/JBD2/account-service adapters at 512/4096 sector sizes, Linux fsck and existing
admission/permissions regressions. Its file-device failure adapter cuts before
and after each actual device flush and restarts in a fresh process. Accepted
sector writes can reach the file immediately; this adapter does not model
sector tears or a volatile controller cache. OOM and a labelled zero-headroom
superblock-read adapter check admission without changing fixture bytes.
`test-accounts-persistent` extends the isolated
Phase 2 BIOS/UEFI runner with password/account/home/hostname/history persistence,
new-user authentication, sudo denial and independent Linux byte/mode oracles.
Focused runs are debugging evidence; only an unchanged-source complete run may
be reported as phase acceptance. Detailed results belong in the Phase 3 report.
