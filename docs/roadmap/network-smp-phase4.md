# Network SMP Phase 4 — physical acceptance

2026-10-16. **COMPLETE — bounded user-confirmed physical acceptance.**
Installed functional/reboot checks and the separate live CS=3 observer results
demonstrate ordinary AP networking with the protocol owner on CPU 0 under
[the Phase 4 gate](../plans/NETWORK_SMP_PLAN.md#phase-4--physical-acceptance).

## User-reported HTTP timing

The Windows peer address is `192.168.0.153`. The test file was created with
`fsutil file createnew`: 104,857,600 bytes (100 MiB). The user reports ten
downloads with a mean elapsed time of 7.20 seconds, corresponding to
14.56 MB/s (13.89 MiB/s), or 116.51 Mb/s of application payload.

One separately reported run took 7.19 seconds, with 0.00 seconds user time
and 0.79 seconds system time. Those CPU times belong to the downloader;
they do not include all work performed by the separate CPU-0 network worker.
They do not establish the bottleneck. These are physical observations, not
QEMU networking measurements, and no gigabit-throughput target is claimed.

The suggested timing command was:

```sh
time -p wget -q -O /dev/null http://192.168.0.153:8000/test100m.bin
```

The exact executed timing command, individual timings, minimum/maximum, server
and link details were not supplied. File creation
and the mean timing are user-reported; no capture or independent byte audit
is available. An earlier attempt returned `DNS_NXDOMAIN`; the exact failing
command was not supplied, so its cause remains unconfirmed.

The Windows reference SHA-256 for the 100 MiB file is
`20492a4d0d84f8beb1767f6616229f85d44c2827b64bdbfb260ee12fa1109e0e`.
The user subsequently confirmed that streaming the HTTP body through
`wget -q -O - http://192.168.0.153:8000/test100m.bin | sha256sum`
matched that reference. This is user-confirmed physical HTTP integrity without
writing the body to USB; no independent capture audit is claimed.

A prior attempt to save the file under `/mnt` failed after 62,685,184 bytes
with reported error 13. The syscall ABI defines `SYSCALL_ENOSPC` as -13;
the earlier conversational interpretation as permission denied was incorrect
(the VFS and syscall error numberings differ). The user suspects the live data
partition is 64 MiB, consistent with insufficient capacity for 100 MiB.
This failed save is retained separately from the passing streamed checksum.

## Remaining acceptance evidence

The user subsequently confirmed live-USB passes for `ifup`, `ifconfig`, ping,
DNS, `smpbench`, `sysinfo` and cable recovery. This is manual functional
acceptance of those operations; exact arguments, outputs and recovery timings
were not supplied. Privileged `sudo ifup` invocation was not separately
confirmed. `smpbench` success does not establish AP syscall execution by the
ordinary network commands.

At this earlier checkpoint testing was live USB only, and the user reported
installer-based persistent networking had not been tested. Subsequent installed
configuration persistence and reboot results are recorded in the final section.

Record laptop model, firmware, CPU count, boot-image hash, configuration path
and exact commands/results. Confirm ordinary unpinned commands, authorized
`sudo ifup`, gateway-specific ping, reboot configuration
persistence/reload, concurrent/background commands and cable loss/recovery
with shell recovery. Record approved physical AP execution evidence separately
from functional success. Live-USB downloads do not establish the plan's
installed-root/reboot gate. Phase 3 automated acceptance remains separate.

## Fresh disk-selection installer handoff

2026-10-16: rebuilt the existing `INSTALLER_MULTI=1` profile from committed
`859b4a52adc57f67b8a62b1f9000b3aef9ee7c58` in an isolated snapshot. All
uncommitted parallel DHCPv2 changes, including Makefile, code, tests and plan,
are excluded; committed DHCP v1 remains. No production source changes or
physical disk writes were needed. The older Dell-bound profile is not the
handoff image. The disk-selection profile lists supported NVMe disks and
requires explicit selection and confirmation, with source-disk exclusion.

Image: `bin/fortress-installer-network-phase4.img`, 553,648,128 bytes (528 MiB).
SHA-256: `7439fd7f87a28318c405dc42bfd7afb0d3c183fe17229c1d1a71171c5f27a571`.
Receipt, source manifest, build log, profile audit and separate single-NVMe
evidence: `build/network-smp-phase3/installer-5fse8hqa/`.
Snapshot: `.codex-remote-attachments/ext4-phase9/guest-workspace-to6q7gum`;
audited producer: its `build/root-phase4/media-m_y20dok` directory.

Fresh disposable-file KVM checks PASS: full BIOS/UEFI two-NVMe suite (both
disk selections, three installed boots each, independent filesystem/account
audits, unchanged unselected media, cancellation, duplicate-root recovery and
installer-source exclusion). Separate UEFI single-NVMe installation, filesystem
audit and installed boot PASS. Profile/symbol/archive checks confirm no fixed
Dell drive binding or private network probes. These are virtual installer
checks; physical installed-network persistence still requires user testing.

## Combined DHCPv2 installer handoff

Later on 2026-10-16, the user requested inclusion of the completed DHCPv2 work
so one installation can test both milestones. A fresh isolated snapshot of
`3cf6cf1b72e99d503f33eeaf3673a4817e5dd401` includes the
committed DHCPv2 implementation without the earlier exclusions. The
`INSTALLER_MULTI=1` disk-selection profile is retained; its installer sources
are unchanged from the preceding handoff.

Use `bin/fortress-installer-dhcpv2.img` for this combined test, rather than the
earlier DHCPv1-only image. Size: 553,648,128 bytes (528 MiB).
SHA-256: `f7e9bc612447166fe80b16d39bb9e63798c4432d6bf5b3b497dfa887ee627aaa`.
Source/build/artifact receipt and guest logs:
`build/network-smp-phase3/installer-dhcpv2-x0np7_mr/manifest.json`.

Fresh checks PASS: DHCPv1/v2 codec ASan/UBSan; the existing DHCPv2 BIOS/UEFI
lifecycle and offline-boot suite (four boots); exact-payload UEFI single-NVMe
installation and independent Linux filesystem/account audit; two installed
boots under SMP=4 with automatic DHCP before login, daemon status, interface
address, gateway ping, renewal signaling and daemon survival. Installed data
survives the reboot. These checks do not independently establish wire-level
renewal ACKs, T1/T2 expiry, physical link recovery or physical installed-network
acceptance. No production source edits or physical device writes were needed
to prepare the image; the preceding two-disk suite was not repeated for this
handoff. Its source/build provenance remains separately recorded above.

## User-supplied installed-system physical results

2026-10-16: the user supplied three photographs after installing the combined
image and confirmed the tests worked, including a clean shutdown and restart
following the instruction to remove installer USB media. The reboot photograph
identifies build `3cf6cf1`, eight CPUs online, and one storage device with three
partitions and two mounted filesystems. Laptop model and firmware mode were not
separately shown in these photographs.

Observed manual results:

- DHCP lease at `192.168.0.168/24`, gateway/server `192.168.0.1`, DNS
  `195.130.131.3` and `195.130.130.3`; daemon running, PID 52, 3600-second lease.
- Peer `192.168.0.153` and gateway ping: four replies from four probes, zero loss.
  `nslookup example.com` returned addresses.
- Foreground and background HTTP streams produced the Windows 100 MiB reference
  SHA-256 recorded above. Ping and DNS remained usable while a background
  download ran; its job completed and `jobs` subsequently showed no jobs.
- Privileged `dhcpc --renew` reported SIGHUP delivery; the daemon stayed alive
  and the displayed lease expiry refreshed. This is functional renewal evidence,
  not an independent packet-capture audit of the exchange.
- Cable disconnect and reconnect produced retained-ring link-down/online
  messages. Link UP, active DHCP status, gateway ping, DNS and the matching HTTP
  checksum were observed afterward.
- After reboot, `phase4-reboot.txt` contained `phase4-reboot-check`, and
  `/etc/network/network.conf` retained the address, gateway and both DNS values.
  DHCP status, gateway ping, DNS and HTTP checksum passed before any requested
  manual reconfiguration. This demonstrates installed file/config persistence
  and automatic networking after the user-run restart; it does not prove that
  the DHCP path used the persisted static configuration to acquire its lease.

Photo references supplied in this chat: `image-1792170904386.jpg`,
`image-1792171520306.jpg`, and `image-1792172030720.jpg`. They are user-supplied
physical observations, separate from the retained automated guest manifests.
The first photo retains a malformed wget invocation followed by the SHA-256 of
an empty stream; corrected foreground/background invocations match the reference.
The reboot photo retains an `ipconfig` typo (unknown command); the earlier
`ifconfig` output shows the actual interface. Neither typo is a kernel failure.

At this checkpoint functional physical acceptance above was demonstrated, but
the Phase 4 gate remained open for physical evidence of ordinary network
command AP syscall execution: eight CPUs online, successful commands and
`smpbench` alone do not establish that provenance. Confirm remaining hardware
metadata and any explicit `sudo ifup`/configuration-reload evidence before
claiming every item of the original gate. No physical T1/T2-expiry, wire-level
renewal, physical two-NVMe or independent filesystem-audit claim is added.

## Physical AP observer image prepared

The user explicitly requested the existing CS=3 observer on a separate Dell
test image. [The Dell observer guide](../network-smp-dell-observer.md) records
the image, hashes, exact commands, mask interpretation, implementation and
retained validation failures. This live image does not request mounting the
installed root/data or require reinstallation. Ordinary `ping`, `nslookup`,
`wget` and DHCP calls are observed without worker-placement arguments. The
trusted pinned boot DHCP probe is separately recorded and excluded.

Handoff: `bin/fortress-dell-cpu-observer-test.img`, 130 MiB, SHA-256
`41b12ad43d348bb95ee3ba9a2bbba1150ec1efc5b77d8c58c7141e9c15168fb1`.
Receipt: `build/network-smp-phase4/dell-observer-wa1d_fa5/manifest.json`.
Guest reader validation `guest-hyiy2otf`: 5/5 PASS, including BSP-only negative
controls and four-command AP masks with CPU-0 owner on BIOS/UEFI SMP=8 and
exact raw USB UEFI SMP=8. Kernel and target-command source comparison to the
tested installed build passes. Release media hashes are unchanged.
At image handoff, physical AP acceptance was pending the user's photographs;
the results below close that requirement.

## Final physical AP acceptance — PASS

2026-10-16. The user supplied `image-1792174102336.jpg` from the Dell live
observer image. It shows CS=3 syscall entry with no placement changes and:

| Ordinary program | Network-call mask | CPUs observed |
| --- | --- | --- |
| `/bin/ping` | `0x9` | 0, 3 |
| `/bin/nslookup` | `0x3` | 0, 1 |
| `/bin/wget` | `0x5` | 0, 2 |
| `/sbin/dhcpc` | `0x37` | 0, 1, 2, 4, 5 |

Owner mask is `0x1`, with nonzero steps (`0xfba33`). The separate boot DHCP
mask is `0x1` and is excluded from ordinary masks. The reader reports
`AP evidence PASS: four commands; owner CPU0`. These are network syscall
numbers 38–51, not inferences from CPU availability or non-network syscalls.
No explicit worker placement or affinity change was used.

Earlier photographs `image-1792173815157.jpg`, `image-1792173905144.jpg` and
`image-1792173996338.jpg` retain the progression: wget/DHCP AP entry first,
then ping, and finally nslookup after further ordinary launches. Initial
incomplete reports and exit status 1 are retained, not relabeled as passes.
Counters accumulate within the diagnostic boot; this does not require every
invocation or every syscall to execute on an AP, nor every AP to be used.

This closes the remaining physical AP evidence requirement. Together with the
installed build `3cf6cf1` checks, matching HTTP hashes, concurrent commands,
cable recovery and clean-reboot persistence documented above, **Network SMP
Phase 4 is COMPLETE**. The AP measurement is explicitly from a separate live
instrumented image with matching production kernel/target sources, not a claim
that the ordinary installed release contains instrumentation. Hardware evidence
is user-supplied photographs and confirmations, not an independent wire audit.

Phase 5 throughput redesign remains deferred. This acceptance does not add
physical T1/T2-expiry, wire-level DHCP renewal, physical two-NVMe, independent
filesystem-audit, NIC interrupt/RSS or multi-owner protocol claims. The earlier
116.51 Mb/s live-USB timing remains a separately qualified baseline.
