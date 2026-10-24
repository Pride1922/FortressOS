# Private package trust floors and rollback boundary

2026-10-24. Step 2 implementation for the bounded, private Gate-1 running-root
profile. Default boot and package dispatch remain unchanged. Installation,
production trust provisioning, release-media builds, pushes and physical writes
remain disabled. This is not production enablement or a general upgrade engine.

## Authority and durable advancement

`tests/pkg_trust_floor.{h,c}` implements an allocation-free signed checkpoint
reader and monotonic transition/admission rules. The fixed 1,024-byte record has
independent selector-role threshold signatures, an epoch, minimum boot contract,
installation/filesystem/partition/repository/enrollment identities, platform,
kernel/bootstrap/selector measurements, root/catalog/build numbers and hashes,
grant/state floors, authorization times and a signed fixed-file locator.
Equal numbers require equal hashes; counters and contracts cannot decrease.
Unknown epochs, same-epoch forks, malformed locators, wrong roles, invalid
signatures and incompatible contracts fail closed. Decoder output stays unchanged
on failure. No private signing key enters the kernel.

`scripts/pkg_trust_records.py` creates external TEST signatures for three exact
checkpoints: epoch 1 restricts root/catalog to at least 3/7; epoch 2 advances them
to 4/8; epoch 3 additionally requires build 1, grant sequence 7 and state
generation 2. Existing catalog signatures and selector/enrollment admission still
validate independently. This does not implement online role/key rotation.
Optional producer version arguments retain the existing 3/7 defaults.

`tests/pkg_trust_fs.inc` authenticates the projected on-disk floor before the
first journal replay write. It checks the exact signed inode/generation/map and
current authenticated package records. The preallocated, root-owned mode-0600
`/pkg/trust-floor.v1` occupies one dense initialized block. Each floor update uses
the actual EXT4 semantic/JBD2 transaction, then re-reads the signed record; it
does not allocate or borrow package escrow. Uncertain I/O requires a fresh owner.

The copied-kernel overlay commits epoch 2 before detached package recovery and
epoch 3 before real account-service readiness and running-root publication.
Build/state floors therefore precede exposing the new executable to users. It
retains the existing bounded pre-signed demo endpoints; generalized install,
remove and upgrade planning belong to step 3. The running-root semantic fence
protects the floor's contents, inode and allocation bits from ordinary mutations.

## Whole-disk rollback and older kernels

The local floor rejects rollback of package records while that floor survives.
It cannot, by itself, detect rollback of an entire disk including the floor.
`verify_readback_and_raise` independently checks TEST signatures, the durable
completion record and grant/state readback before producing a boot policy whose
minimum is epoch 3. It modifies no root disk. That newer policy is the rollback
anchor, held independently of the root disk. An old root snapshot, including its
old floor, refuses against this newer trusted policy before writes.

The private boot command is `boot_mode=pkg-trust-v2`, with checkpoint contract 3.
Active proposals must name the measured kernel, bootstrap and selector. The
previous accepted private kernel rejects this new selection before writes and
does not publish the enrolled root. Replacing both the trusted boot policy and
the root with older copies is outside this software-only guarantee. Protecting
against a boot administrator requires a hardware-backed boot/monotonic policy;
Secure Boot, TPM counters and production provisioning are not implemented here.
Authorization time is the independently supplied TEST policy time, not a claim
of a hardware-trusted clock. Signed checkpoint publication is still an offline
operational step, not an automatically provisioned production service.

## Verification

Exact commands, hashes, fault positions, QEMU arguments, serial logs and Linux
oracles are retained under `.codex-remote-attachments/`. Boot acceptance and
source/media preservation are separate results. See the accompanying
[step-2 evidence](../roadmap/pkg-trust-floors.md) for final counts, retained
failures and bounds. Host faults and QEMU boots do not establish physical
power-loss safety. No public install/remove acceptance is claimed.
