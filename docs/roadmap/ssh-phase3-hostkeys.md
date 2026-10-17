# µSSH Phase 3 host identity

Implementation 2026-10-17 against handoff `d67ccf0`. Host-key host and guest gates
pass; this report does not close #52 or claim a functioning SSH server.
[Design and resolved decisions](../plans/SSH_PHASE3_HOST_IDENTITY.md).

## Behavior and interfaces

The user-only crypto archive now includes the bounded OpenSSH single Ed25519
codec in `src/lib/crypto/ssh_hostkey.c`, exposed by `ssh_hostkey.h`. It supports
only unencrypted openssh-key-v1, checks every length, key count, cipher/KDF,
comment, checkint and padding, and requires the outer public key, inner public
key and secret public half to match the public key regenerated from the seed.
Outputs remain untouched on failure; valid disjoint workspaces are wiped.
Generated keys use Phase 1 strong entropy for the seed and fresh checkint and
perform a fixed sign/verify self-check. No deterministic production fallback.

OpenSSH portable V_10_2_P1 is the format baseline, with reference hashes in
`src/lib/crypto/ssh_hostkey_reference.json`; no OpenSSH implementation source
is imported. Its serializer uses zero padding bytes for already aligned private
sections. Independent installed OpenSSH tests identify their client version in
`build/ssh-hostkeys-host/result.json`. This pins format behavior, not latest
client availability or an independent security audit.

`SYS_HOSTKEY` 79 takes `(op, buffer, bytes)`. Invalid operation/size precedes
EPERM, EFAULT, missing installed-root EOPNOTSUPP, then allocation/storage errors.
Both operations require effective UID 0 and CAP_SYS_ADMIN. READ takes capacity
1024 and returns the private container length; PUBLISH takes 1..1023 opaque
bytes and returns zero. READ has no output on error. This is a private monitor
load, not a public status/export syscall; crypto validation remains mandatory
for every consumer. Kernel code neither parses keys nor claims readiness.

The backend is fixed to `/etc/ssh/ssh_host_ed25519_key` on the admitted installed
root. Retained single-basename lookups exclude symlinks and mount substitution;
root-owned safe directory metadata and exact root:root 0600 file metadata are
validated on the objects used. A staging inode has correct attributes from its
creation transaction. Checked short-write/read loops, a mount barrier, backend
atomic no-replace rename, publication barrier and retained-object readback
precede success. Existing canonical keys are never replaced or truncated.
Failure can leave private staging residue requiring explicit administration;
no rollback unlinks a potentially replaced pathname. Atomic gate contention
returns EAGAIN; synchronous calls own/release their scratch and references,
with no process lease or sleeping wait under a new lock.

`/sbin/ssh-keygen --provision` creates an absent key or validates/preserves an
existing key; `--public` and `--fingerprint` export public information. All use
the root-only service and fixed path. The command validates private/public
consistency after loading and after publication. Corruption and wrong metadata
fail without regeneration. No boot code generates keys or changes enablement.

Installer v4 preserves existing request prefixes and adds an optional private
input buffer/length. Generation runs on the target after confirmation and lease
acquisition, before synchronous deployment, preserving user-only crypto.
Publication occurs after detached root/account preparation; freeze/discard and
fresh-mount metadata/byte readback precede acceptance. Entropy failure sets
INSTALL_STATUS_HOSTKEY_WARNING and leaves SSH disabled on an otherwise healthy
installation. Storage errors still fail installation. Private buffers are
wiped, including the runtime's retained failure owner. Existing v1-v3 callers
remain supported without automatic host identity provisioning.

## Evidence so far

- `make test-ssh-hostkeys-host`: actual codec/crypto ASan/UBSan, all truncated
  prefixes, 10,000 mutations, failure-output and workspace-wipe assertions,
  first/second entropy-call failure; 266 independent structural/CRLF/OpenSSH
  cases PASS. OpenSSH parses FortressOS output; FortressOS parses generated
  OpenSSH keys with every padding alignment and bounded comments. SHA256
  fingerprint matches independent Python and OpenSSH. New codec frames <=240
  bytes; existing verifier-only exception is unchanged.
- `make test-ssh-hostkeys-storage-host`: actual EXT4/JBD2/VFS/backend and kernel
  adapter with labelled host VMM/transport/heap adapters. **595 cases PASS**:
  normal publication at both sector sizes, all 23 flush boundaries at each,
  all 472 writes at 512-byte sectors and 59 writes at 4096-byte sectors. Fresh
  recovery leaves canonical identity absent or byte-identical with validated
  metadata. Independent Linux fsck and metadata/byte audits pass. This is a
  sector-atomic regular-file model, not physical sector tear or volatile-cache
  assurance. Basic OOM, gate contention, retained-reference reclamation,
  privilege and error precedence checks pass.
  Labelled write adapters additionally prove short-write progress, zero-progress
  rejection and ENOSPC handling. Real pthread competitors produce one successful
  publication and one EAGAIN/EEXIST; independently altered ancestor/key metadata
  is rejected by fresh mounts at both sector sizes.
- `make test-crypto-host`, `make test-fstat-host`, installer CLI/control/NVMe
  exclusion and detached provisioning host regressions PASS.
  Actual account-service/mounted EXT4 update regressions at 512/4096 sectors
  also pass (snapshot, writer contention/death, stale decisions, OOM, shadow
  permissions and immutable canonical paths), recorded in
  `build/ssh-hostkeys-host/account-regression.json`. The optional full unchanged
  account crash rerun was interrupted after the 512-sector flush=50 checkpoint;
  `build/root-phase3/gate-clcye_ch/` retains that interrupted attempt, not a
  full-suite PASS. The complete new host-key recovery campaign is separate.
- `make -j8 all build/tool-install.elf build/tool-install-preview.elf`: release
  kernel/tool/library/ISO/IMG compilation and GPT/FAT/EXT4 structure audits PASS.

`scripts/test_ssh_installer_host.py` passes the actual installer v4 runtime path
with an explicitly published deterministic fixture: detached publication,
freeze/discard/fresh mount, exact bytes/metadata, installed-root isolation,
resource reclamation and independent OpenSSH parsing. Evidence:
`build/root-phase4/runtime-5nwsduwv/manifest.json` and `host.log`. This fixture
does not claim a real Ring 3 installer invocation or physical provisioning.

`scripts/test_ssh_hostkeys.py` passes **18/18 installed guest boots**, BIOS/UEFI
times SMP=1/4/8, with fresh provisioning on six distinct targets, repeated
command-process reloads, three boots per target (including abrupt guest stop
after sync), privilege denial, normal-user private-file denial, unchanged
fingerprint and independent offline Linux/OpenSSH metadata/identity audits.
The two campaigns have distinct identities within each campaign; public
fingerprints are retained for comparison. Release ELF/initramfs/ISO/IMG hashes
remain unchanged throughout each campaign. Evidence:

- `build/ssh-hostkeys-qemu/gate-lh_f78na/manifest.json`: SMP=1, 6/6 boots PASS.
- `build/ssh-hostkeys-qemu/gate-ikvty3v4/manifest.json`: SMP=4/8, 12/12 boots PASS.
- `build/ssh-hostkeys-qemu/gate-5dsaqf5g/`: interrupted first harness attempt;
  an expected diagnostic string was corrected. It is not passing evidence.
- `build/ssh-hostkey-acceptance/gate-qgdbxyro/manifest.json`: real Ring 3 installer
  v4 provisioning and acceptance across BIOS & UEFI under three hardware profiles
  (`ready`, `absent`, `stall`). **18/18 boots PASS**:
  - `ready`: BIOS and UEFI installations received distinct identities (`SHA256:D5dwVwyatpWdLRvauNUAjYYfavPqYl/7QAQoLAbLxdI`
    and `SHA256:0cwCcxzyMPR9ooVhmOxpe/8F+K4Blv1QVbpom7ljg8E`), both verified across 3 reboots;
    `--provision` confirmed idempotent preservation of the existing key; offline
    `debugfs` audit confirmed exact directory (0700) and canonical key (0600 root:root)
    metadata, and OpenSSH `ssh-keygen -y -f` / `ssh-keygen -lf` exact fingerprint match.
  - `absent` (`random_hw_features` returns 0) and `stall` (`random_hw_word` returns false):
    installation completed cleanly with `INSTALL_STATUS_HOSTKEY_WARNING` emitted
    ("SSH host identity unavailable. SSH remains disabled; provision it explicitly after installation."),
    while neither canonical key `/etc/ssh/ssh_host_ed25519_key` nor staging residue
    `.fortress-hostkey.pending` was published or left on disk. Subsequent
    `/sbin/ssh-keygen --provision` cleanly refused generation ("strong random/key generation failed", exit 1).
  - Ring 3 `SYS_HOSTKEY` (syscall 79) boundary probe (`tests/hostkey_probe.c`):
    argument bounds and precedence, unprivileged caller rejection (EPERM), privileged
    bad-pointer/read-only buffer rejection (EFAULT), partial-page crossing into
    an unmapped page (`EFAULT` with zero pre-fault corruption), failure-output
    buffer preservation, and capability-drop denial via `SYS_CAPSET(0)`.
  - Immutable media and release exclusions: release ELF/tar/ISO/IMG hashes
    remained unchanged throughout; `hostkey-probe` is excluded from release media.
- `build/ssh-hostkeys-host/result.json`, `storage-result.json`, and
  `kernel-stack.json`: codec, 595 storage cases and frame measurements. New
  kernel helpers are <=160 bytes per frame. The existing installer-control
  frame is 1328 bytes versus 1312 at handoff; this is reported separately from
  the crypto library's unchanged verifier-only exception. Installer main is
  368 bytes, codec maximum 240 bytes.

The backend's active operation uses two 1024-byte heap scratch buffers plus
three retained directories and a retained file; contender adapter allocations
are transient and do not acquire backend ownership. Command static key/export
scratch is 4275 bytes plus the existing common tool buffer. There is no new
network state, protocol owner or kernel crypto state.

Generated private keys and their disks are temporary; retained QEMU evidence
contains public fingerprints, diagnostics, argv and results only. Deterministic
published fixture seeds are explicitly labelled and may be retained by host
tests. No real user password, private key log, physical write, listener,
daemon lifecycle, rotation or remote-login acceptance is claimed.

## Remaining requirements for issue #52 closure

Automated host identity provisioning, persistence, and boundary gates pass. Issue #52
remains open pending the following separate milestones:

1. **Atomic key rotation & incomplete-provisioning recovery transactions**:
   The current backend only supports first-time publication and explicit no-replacement.
   Rotation requires atomic inode replacement in one journal commit, comparing expected
   old fingerprints under filesystem exclusion, with rollback auditing and no journaled
   secret copies. Residue cleanup requires durable service receipts and proven
   object-identity provenance. Both operations currently remain unsupported and disabled.
2. **Daemon lifecycle & process monitoring boundary**:
   Proving that no running daemon holds open descriptors or monitor signing references
   to the active host key before rotation or unmount.
3. **Physical bare-metal acceptance**:
   Physical installer provisioning and persistence acceptance on hardware (e.g. Dell Latitude 5590)
   with physical write gates explicitly evaluated.

