# Central network management and credential hardening

Status: planned; no daemon, vault or new authorization ABI is implemented here.
Baseline checkpoint: 2026-11-02. Physical testing of the current installer is
in progress; do not replace its bytes while that evidence is being collected.

## Baseline

The sole retained disk image is `bin/fortress-base-installer.img`, 528 MiB,
SHA-256 `8afbab95e23d05360d45a53bf3e45dd16d16a5de2beff6e5164de53cf6b5736b`.
Wi-Fi fixes are in `cf68155`. The desktop/package/sudo source and tests that were
already included in this image are checkpointed in `adea133`. The image's original
receipt truthfully records that work as dirty at build time; do not rewrite the
receipt to pretend the later commit existed during the build. Executable/build
source hashes can establish equivalence to the checkpoint without rebuilding.

The Wi-Fi owner/manager/profile host gates and BIOS/UEFI mock-radio ownership
matrix pass. The raw image boots to the installer under BIOS and UEFI with SMP=4,
without a target disk. This does not establish an installed reboot or physical
Wi-Fi pass. The existing compositor built-in-app host test hung; 61 other tests
passed with that case filtered. Fix that test isolation before declaring a full
compositor regression pass. See `docs/roadmap/complete-installer-2026-11-02.md`.

## Findings to remove

- `user/wifi.c`, desktop `sys.rs`, Watchtower's `wifi.service` and `user/dhcpc.c`
  participate in connection policy. The DHCP daemon can spawn `wifi auto`.
  The kernel already serializes protocol/driver execution on CPU 0, but this is
  distinct from having a single owner of user-space connection policy.
- The CLI and desktop have independent profile serializers, buffer limits and
  error handling. Desktop reads of profile errors currently fall back to an
  empty profile set; that can overwrite existing profiles. Its save path reads
  profiles in the desktop process although the settings broker owns the write.
- `/etc/network/wlan.conf` contains plaintext `psk` fields. CLI checks chmod 0600
  before writing secret bytes; desktop `atomic_write` ignores chmod failure.
  SYS_OPEN currently takes path/flags, not a creation-mode argument. Never claim
  that a caller's ignored third argument establishes permissions. Test effective
  owner/mode and unprivileged denial on installed ext4 and live ETCFS.
- Both CLI and desktop use exclusive fixed temporary filenames. Crash recovery,
  concurrent updates and stale temporary-file cleanup need an explicit policy.
- CLI credentials can enter command arguments and shell history. Secret buffers
  and the kernel PMK comparison cache need a complete lifetime/wipe audit.
- Kernel SYS_WIFI management currently has no capability gate. A daemon alone
  cannot prevent unprivileged callers from bypassing it; migration must specify
  and enforce the desired authorization policy, rather than assume a UI broker
  makes the direct syscall privileged.

## Target contracts

A persistent privileged `/sbin/netmgr` owns profile mutations, attempt selection,
Wi-Fi association, DHCP coordination, DNS publication and reconnect policy.
CLI/desktop are clients and do not read the secret store or launch DHCP/retry
children. Watchtower supervises netmgr; it does not run a second connection loop.
DHCP supplies leases and renewal results to netmgr under an exclusive per-link
lease session. Driver and protocol work remains on the registered CPU-0 kernel
owner, preserving bounded requests, lock ranks and polling-only ingress.

Each attempt has an ID and interface generation. One active attempt controls a
link; late scan/authentication/DHCP results from earlier generations cannot change
its configuration. Cancellation and explicit user disconnect suppress automatic
reconnection until policy permits it. Wired preference and handover are explicit
policy, with one authoritative route/DNS publisher. Retries have bounded work,
backoff and reason codes; an absent password produces a credential request.

Publish independent stages: radio readiness, scan/selection, authentication,
association/key authorization, IP configuration, DNS configuration and optional
reachability verification. Do not collapse successful association into usable
network access or require an external Internet probe to accept a working LAN.

The management interface needs bounded versioned requests, authenticated caller
identity, cancellation, generation-bound replies and secret-free status/events.
Choose the transport/discovery mechanism after auditing implemented IPC; do not
assume AF_UNIX, a system bus or a new syscall already exists. Specify permissions
and legacy SYS_WIFI/NETCTL behavior before changing public ABI/access contracts.
No network or root-display authorization bypass is implied by this plan.

## Credentials and encryption boundary

Profiles expose a credential reference, security type and autoconnect policy;
secret material is owned by netmgr. Unprivileged clients receive metadata, never
passwords, derived PMKs or secret-bearing file contents. Missing/malformed
credentials must fail closed without overwriting the last usable profile.
The secret store and its containing directory require verified root ownership
and access restrictions, checked before secret writes, with atomic migration,
durability checks, bounded recovery and no secret-bearing logs/argv/history.
Migration retains the old usable record until the new record is committed.

Encryption at rest is a separate requirement from access control. It needs
vetted authenticated encryption and a specified source of key material, nonce
uniqueness, rollback/corruption behavior, recovery and rotation. Do not invent
cryptography or put a hardcoded key in the kernel/image. A key saved on the same
unencrypted filesystem does not establish protection against offline disk access.
A derived WPA2 PMK is still an authentication credential, not a password hash
safe to disclose. WPA3 and interoperability must not be broken by a shortcut.

Key-release policy is unresolved: unattended pre-login Wi-Fi needs a key available
at boot; login-unlocked storage delays secret access until login. TPM sealing or
full-disk encryption requires platform support and a recovery design; neither is
assumed implemented. Choose and document the threat model before promising
password encryption or implementing the vault. Existing plaintext data is not
made retroactively confidential by a new serializer or daemon.

## Delivery order and acceptance gates

1. **Baseline and diagnostics:** finish this installed-image physical test; retain
   exact image identity and results. Fix host syscall isolation and add actual
   profile access-denial, failed-chmod, failed-read and crash-recovery regressions.
   No new image is needed merely to checkpoint the current source.
2. **Shared profile/secret boundary:** one validated format and transactional
   implementation, explicit security type, permission enforcement, no silent
   overwrite on failed reads, secret-free client queries and safe legacy import.
   Resolve key-release policy before encrypted-storage implementation.
3. **Central service:** implement the management ABI/authorization boundary,
   attempt state machine and exclusive DHCP coordination, then migrate CLI,
   desktop and startup. Remove the old competing policy paths after parity tests.
4. **Release gate:** install, connect once, obtain IP/DNS, reboot twice without
   credentials, update, reboot, and reconnect. Exercise incorrect credentials,
   absent/returning AP, Ethernet handover, manager/DHCP crashes, disk-full and
   interrupted profile writes, user cancellation and simultaneous CLI/GUI requests.
   Use actual storage for durability and actual Intel hardware for RF claims.

Use isolated builds from committed sources in the existing documented container.
Promote the exact tested bytes as the one current installer; ordinary signed OS
updates preserve configuration. Record host, mock-radio, guest installation and
physical evidence separately. Do not mark the lifecycle complete from unit tests
or boot smoke alone. Preserve PROTECTED.md contracts throughout implementation.
