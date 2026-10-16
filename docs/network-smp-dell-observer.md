# Dell physical AP syscall-entry observation

**Physical acceptance PASS (2026-10-16).** The final Dell photograph reports
network masks ping `0x9`, nslookup `0x3`, wget `0x5`, dhcpc `0x37`, with owner
`0x1` and the expected PASS footer. [Final evidence and boundaries](roadmap/network-smp-phase4.md#final-physical-ap-acceptance--pass).
The instructions below describe the completed procedure and remain available
for reproduction; the pending-review language describes the handoff stage.

2026-10-16. This separate **live TEST image** observes CS=3 syscall entry. It
does not install or request mounting the existing NVMe root/data. Keep the
installed system as it is. DHCPv2 is included. CPU placement is unchanged.

## Image and boot

Image: `bin/fortress-dell-cpu-observer-test.img` (136,314,880 bytes, 130 MiB).
SHA-256: `41b12ad43d348bb95ee3ba9a2bbba1150ec1efc5b77d8c58c7141e9c15168fb1`.

Flash the raw IMG to the test USB using Rufus/DD mode. Boot its UEFI USB entry
through F12 and choose the entry beginning **FortressOS DELL CPU OBSERVER TEST**.
Log in as `operator`, the normal live account. No reinstallation is needed.
Keep the Windows server serving `test100m.bin` at `192.168.0.153:8080`.

## Commands on the Dell

Confirm automatic networking:

```sh
ifconfig
dhcpc --status
```

Run this ordinary concurrent group without any CPU/worker placement arguments:

```sh
ping -c 4 192.168.0.153 &
nslookup example.com &
wget -q -O - http://192.168.0.153:8080/test100m.bin | sha256sum &
sudo dhcpc --dry-run &
sleep 12
sysinfo
net-cpu-probe cpu
```

The HTTP SHA-256 must match the Windows reference:

```text
20492a4d0d84f8beb1767f6616229f85d44c2827b64bdbfb260ee12fa1109e0e
```

Photograph `sysinfo` and the compact CPU report. All four program rows must
show **AP=YES**, the owner line must show **OWNER cpus=0x1** with nonzero steps,
and the footer must say **AP evidence PASS: four commands; owner CPU0**.
If a row says AP=NO, repeat the concurrent group and check again: an AP network
entry has not yet been observed for that program. Keep command failures and
their output separately. Do not add `--worker` flags to manufacture a result.

Optional per-syscall details:

```sh
net-cpu-probe cpu-detail
```

Masks accumulate since this boot. Bit 0 (`0x1`) is CPU 0, bit 1 (`0x2`) CPU 1,
bit 2 (`0x4`) CPU 2, and so on. Each **net** mask must contain a bit beyond bit
0. `all` includes non-network syscalls and is not sufficient evidence. `net`
includes syscall numbers 38–51 (socket/NETCTL), excluding ambiguous READ/WRITE
descriptors. Every AP need not appear: one AP entry per named program suffices.

The separate **BOOT DHCP pinned cpus=0x1** line is expected. The trusted initial
DHCP probe remains BSP-pinned and is excluded from ordinary masks. The ordinary
unpinned daemon and shell-launched dry run are observed. The dry run exercises
DHCP sockets without replacing the active configuration or starting a daemon.

After collecting the photographs, run `sudo shutdown`, remove the test USB and
boot the existing installed system normally. The physical AP gate closes only
after reviewing the Dell results; QEMU validation does not substitute for them.

## Source and validation record

Producer: `scripts/prepare_network_smp_dell_observer.py`. It reuses the Phase 3
instrumentation in an isolated snapshot. Entry/owner hooks, diagnostic boot
configuration, helper staging and private snapshot dispatch exist only there.
The production Makefile, syscall ABI, scheduler and assembly are not edited.

`tests/net_ordinary_observer.c` keeps its campaign modes and adds `cpu`,
`cpu-detail` and a guest bounds check. Private call `0x7ff6` returns one
704-byte versioned row. It checks the row index, exact size and writable user
range before copying atomic snapshots. It neither resets counters, changes
placement nor performs networking. No ordinary release image is overwritten.

Receipt: `build/network-smp-phase4/dell-observer-wa1d_fa5/manifest.json`.
Its `production-equivalence.json` confirms 221 kernel/include/library source
files and six ordinary networking/DHCP source files match installed build
`3cf6cf1` before instrumentation. This is source equality, not instrumented ELF
byte equality. The baseline ordinary kernel has no observer symbols.

Guest campaign: `guest-hyiy2otf/manifest.json` in that delivery directory,
**5/5 PASS**: BIOS/UEFI ISO at SMP=1/8, plus exact raw USB under UEFI SMP=8.
Both SMP=1 cases correctly report four net masks of `0x1` and incomplete AP
evidence. All three SMP=8 cases report AP networking for all four names, with
owner mask `0x1`. Actual Ring 3 calls reject null, overflow, kernel/read-only
pointers, wrong sizes and invalid rows. Foreground controlled HTTP hashes,
DNS, ping and DHCP outputs are checked separately from concurrent CPU masks.
Tests use controlled loopback peers, no public Internet, no data disks and no
physical devices. The raw USB copy is read-only; OVMF variables are disposable.

Earlier failures remain retained: `dell-observer-mtak07bf/guest-f7k2_fix`
rejected the normal named live-boot fault fixture; `guest-w3bvlt0t` found the
helper missing after initramfs regeneration. The final image's earlier
`guest-4ur1vhs4` recorded AP masks but failed an interleaved UART output check.
The final runner permits only the named completed boot fault fixture, rejects
additional post-login faults, and separates foreground output checks. The
final packaging stages the helper in the snapshot-only initramfs recipe and
verifies its exact archive bytes. Failed manifests are not relabeled.

Reproduce under WSL/Linux:

```sh
python3 scripts/prepare_network_smp_dell_observer.py
python3 scripts/test_network_smp_dell_observer.py --delivery build/network-smp-phase4/dell-observer-RESULT
```

The producer prints the fresh delivery path. Validation requires KVM, OVMF
and `sudo -n` to start only the loopback DNS fixture on port 53. Exact QEMU
arguments, source identities, serial logs and bounded process cleanup are
retained. The build/validation does not claim physical acceptance.
