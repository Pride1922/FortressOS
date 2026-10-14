# Installed ifup and persistent configuration — 2026-10-13

## Problem and correction

Commit `438fae08` (October 8) changed ordinary VFS-spawned processes to
`cpu_affinity=-1` and `CPU_MASK_ALL`. NETCTL and socket syscall admission still
require CPU 0 and affinity 0. Therefore `ifup` returned
`ifup: operation not supported on this CPU` even on CPU 0 and at SMP=1.
`ifconfig`, ping and socket tools were affected too. BIOS/UEFI SMP=1/4 reproduced
the exact error on disposable live-boot copies of the existing installer ISO.
Earlier October 2 network acceptance predates the regression.

Ordinary VFS children now start pinned to CPU 0, including sudo helpers and
staged pipeline children. The existing `--worker N` benchmark placement remains.
The CPU mask is established inside process construction before publication;
the redundant post-publication assignment is removed. Network worker ownership,
AP rejection, locks, IRQ handling and NIC behavior are unchanged. This restores
the earlier ordinary-command placement and removes implicit all-CPU placement
for those commands; an explicit broader affinity policy is separate work.

## Installed configuration

Apply and save, with administrator authority:

```text
sudo ifup 192.168.0.199/24 192.168.0.1 1.1.1.1 8.8.8.8
```

The installed file is `/etc/network/network.conf` (root-created, mode 0644):

```text
address 192.168.0.199/24
gateway 192.168.0.1
dns 1.1.1.1
dns2 8.8.8.8
```

DNS lookup consumers continue using `/etc/resolv.conf`. Config-file application
now applies its DNS entries too. Both positional DNS servers survive a save and
read-back. Installed DNS configuration fallback uses the new network path;
missing installed files do not fall back to live storage. An explicit old file
can be imported with `sudo ifup /etc/network.conf` or another supplied path.
Live address configuration retains its existing runtime behavior and defaults.

After reboot, `sudo ifup` reapplies the saved settings. This change does not add
automatic application at boot. CLI calls without DNS keep the resolver file
unchanged. `--dry-run` changes neither the interface nor files.

Writes handle short progress, check close, and installed success requires a
global `SYS_SYNC` barrier. A save failure returns nonzero and explicitly reports
that the interface has already been applied. Persistence uses checked direct
file writes, like the existing hostname tool; it is not an atomic transaction
across configuration and resolver files. A failed write may leave an incomplete
file. No filesystem replacement-rename or new journaling contract is claimed.

## Validation

`make test-net-ifconfig-host`: actual tools/parser/network configuration under
ASan/UBSan with syscall/filesystem mocks. Exact serialized bytes, both DNS
servers, installed default read-back, seven-byte short writes, dry-run, denied
application, mkdir/open/write/zero-write/close/sync failure reporting. The two
missing raw serial diagnostic mocks are supplied in the host fixture.

`make test-net-dns-host`: actual resolver and literal wire vectors under
ASan/UBSan, including installed resolver selection and second-server fallback.
The host mock now recognizes the existing namespace status syscall.

`make test-perm-spawn-host`: credential vectors and actual sudo adapters pass.

`scripts/test_net_installed.py` builds an isolated snapshot by default, or takes
`--workspace` pointing to an isolated snapshot. `make test-net-installed` runs
the host configuration gate plus this runner. It creates a new regular-file
EXT4/GPT/NVMe fixture, authenticates through real Ring 3 login/sudo, and tests
BIOS/UEFI at SMP=1/4 with two boots each. It checks missing-file diagnostics,
unprivileged rejection, apply/save, file-mode DNS, ping, HTTP socket download,
pipeline launch, malformed rejection, dry-run, clean shutdown and reboot reload.
Linux fsck and exact-byte dumps independently audit both saved files. A test-only
Ring 3 probe checks ordinary BSP NETCTL/socket admission and explicit AP-worker
rejection. No test-only executable is added to production initramfs.

Initial installed gate: 8/8 PASS, evidence
`build/net-installed/gate-er6vqn_6/manifest.json` and adjacent logs/audits.
Kernel/initramfs were built strictly in
`.codex-remote-attachments/ext4-phase9/guest-workspace-psmcxprc`.
Additional BIOS/UEFI SMP=8 gate: 4/4 PASS, including real Ring 3 BSP acceptance
and AP NETCTL/socket rejection, evidence
`build/net-installed/gate-qqaal9q2/manifest.json`. Exact invocation:
`python3 scripts/test_net_installed.py --workspace /mnt/c/Sources/FortressOS/.codex-remote-attachments/ext4-phase9/guest-workspace-psmcxprc --cpus 8`.
All test data disks are newly created regular files; firmware variables
are disposable copies. Existing installer/live images and uncommitted changes
are preserved. No physical Dell or installer-write acceptance is claimed.
