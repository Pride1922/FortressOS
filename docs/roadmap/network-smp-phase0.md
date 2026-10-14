# Network SMP Phase 0 evidence

2026-10-13. Contracts, disposable fixtures and pinned baseline only.
No production handoff, socket admission or spawn-policy changes; no Phase 1.
Existing dirty tracked and untracked sources were copied into isolated builds.

## Decisions and audit

[Concrete contracts](../plans/NETWORK_SMP_PHASE0_CONTRACTS.md) record the typed
inventory, transition/ownership tables, cancellation and receive/accept commits,
error mapping, lock graph, 64 request records / 32 execution credits / eight
64 KiB slabs, 576 KiB ceiling, FIFO admission and bounded rotating owner service.
They trace syscall entry, borrowed fd lookup, inherited/final close callbacks,
reaper destruction, signals and remote scheduler wakes. Missing file/endpoint
reference and abandonment/reservation APIs are explicit implementation
prerequisites with defined ownership; they were not implemented in Phase 0.

The self-review increased request records from 32 to 64 while retaining 32
execution credits: stopped callers must retain identities without stranding
control execution credits. Copy pins prohibit reclamation during user copies;
reservation leases cannot introduce timeouts into untimed public I/O.

Three baseline discrepancies are material:

- TCP operations and configuration currently mutate state from IF-clear BSP
  syscall continuations. The desired sole-worker contract is a strengthening.
- Shell-staged `--worker 1` release enqueues on BSP. The prior probe's rejection
  can result solely from affinity mismatch, without AP execution. A snapshot-only
  exception for `/bin/net-cpu-probe` publishes that probe to its AP queue and sends
  a reschedule IPI. Release sources and ordinary commands remain unchanged.
- PROTECTED.md still mandates non-cryptographic ISNs with 120-second reboot quiet
  time. Current code hashes boot entropy, has conditional hardware-random
  assurance, announces unconditional cryptographic ISNs and zero quiet time,
  and does not enforce the old quiet-period check. Phase 0 preserves this code
  and records the conflict rather than changing an unrelated protected policy.

**Gate qualification:** the audit/fixture deliverables are recorded, but the
campaign's unconditional “no unresolved safety dependency” gate is held for
explicit reconciliation of the protected TCP ISN/quiet-time policy. Baseline
success does not settle that policy. Phase 1 has not begun; subsequent phases
must receive their own authorization and gates.

## Reproduction and evidence

2026-10-14 continuation: explicit user authorization resolves the protected
policy qualification by documenting the existing `056a12d` TCP behavior and
conditional entropy assurance. No production TCP code was changed. The initial
12/12 campaign and 61 exclusion negatives remain the baseline evidence; the
final target's concurrent-AGENTS.md preservation failure remains a failure.
`make test-net-eth-host` now passes actual-code ASan/UBSan after adding only
the missing `serial_raw_puts` / `serial_raw_print_hex` no-op test adapters.
This resolves the recorded fixture-link prerequisite, not real scheduler proof.
Phase 0 is accepted for proceeding to the separately gated Phase 1 mechanism.

Run `make test-network-smp-phase0` in WSL Ubuntu-24.04. It snapshots current
tracked and untracked sources, applies three test-only patches in the snapshot,
builds a strict kernel/initramfs, and reuses the installed-root baseline fixture.
The probe ELF is compiled into the disposable root only. It is absent from the
release initramfs; provenance strings are absent from the uninstrumented kernel.
Snapshots and disks are retained; no shared ISO/raw image is rebuilt or attached.

Initial complete campaign:
`build/network-smp-phase0/gate-8tsepefx/manifest.json` — **12/12 PASS**.
Source HEAD `a5013895fb7c49253176985518effc6ce3d2fd44` plus the dirty workspace;
the exact source identities are in `sources.json`, not inferred from HEAD.
Snapshot overrides are in its `network-smp0-overrides.json`.

| Scope | Evidence / result |
| --- | --- |
| BIOS/UEFI × SMP=1/4/8 × two boots, e1000/user backend | 12/12 guest cases PASS |
| Real probe Ring 3 SYS_NETCTL (42), SYS_SOCKET (38) | CPU 0 accepted; CPU 1 rejected at SMP=4/8, both syscall numbers logged |
| Worker execution provenance | `net_worker_main` logs actual CPU 0; no assertion that current syscall mutations execute in worker |
| Installed admission/login/sudo | PARTUUID root admission, operator authentication, permission denial and explicit `sudo ifup` PASS |
| Configuration persistence | First boot explicitly writes `/etc/network/network.conf` and `/etc/resolv.conf`; reboot begins with boot config and explicitly applies `sudo ifup`; exact dumped bytes and root:root 0644 metadata PASS |
| Pinned commands | ifconfig, ping gateway, dry-run/malformed ifup, ifconfig pipeline, local HTTP wget response and shell recovery PASS |
| Independent filesystem audit | Clean shutdown, extracted root, `e2fsck -fn` after every boot PASS |
| Baseline image and workspace preservation | Baseline image SHA-256 unchanged, snapshot-source hashes still match at campaign end PASS |
| Final saved QEMU argv | 12 strict audits PASS; one disposable NVMe disk only, paired OVMF code and per-boot vars |
| Exclusion adversarial cases | 61/61 PASS: extra drive/blockdev/CDROM/USB/shared target and actual prelaunch outside-directory rejection; no process launched by exclusion selftest |

HTTP success is a response substring assertion, not a full independent wire or
download digest audit. Public DNS and internet availability are not dependencies.
No packet-capture, e1000e, positive AP networking, cross-core handoff races,
pool/reference reclamation, link-flap, saturation or physical acceptance claim
is made by these baseline cases. Those belong to later gates.

Initial instrumented kernel SHA-256:
`4789ffa4e3be04dea77a20a655b748875c2aa7a72e8b6ef05c76fec4a3315ac8`.
Initial initramfs SHA-256:
`99c2f48ff32debfed664eabebb8f4ed1d0dcde5060c2e5f5ff9cd5024825c01f`.
Baseline assembled disk SHA-256:
`df969fd7026ba24675453540ba46e1dd7d3551eed931e61e4300b8b9ff0ce9c1`.

Uninstrumented strict build in isolated `guest-workspace-deycfqrl` PASS; hashes
in `build/network-smp-phase0/release-baseline-hashes.json`. Kernel:
`927c7e84212eb88c6cf2c8c8402981e3b9ace2f117d87902a98e47334da764d9`;
initramfs:
`a9061cd966360ed6ab01ba939a9374bdc7b3b38e576de76dc00ceaeb9a8f6a17`.
Archive contents and kernel strings were checked for fixture absence.

Host invocation in that isolated snapshot:
`make test-net-ifconfig-host test-net-socket-host test-net-tcp-socket-host test-net-eth-host`.
IFCONFIG/config parser/ifup, UDP socket and TCP socket actual-code ASan/UBSan
targets **PASS**. Ethernet worker host target **FAIL to link**: missing
`serial_raw_puts` and `serial_raw_print_hex` adapters in the pre-existing test.
Full output retained in `build/network-smp-phase0/host-baseline.log`. Mocks do
not prove actual IRQ scheduling or cross-core networking.

## Retained failed preparation

`gate-1a1sn658`: two SMP=1 cases passed; BSP functional SMP=4 boot passed but
AP provenance failed (actual CPU 0 despite AP affinity). Retained serial and
argv prove why the old rejection probe is insufficient.
`gate-shpbnj7q` / `gate-gjcs5seh`: preparation stopped before boot because the
thread patch anchor did not match CRLF sources. Normalizing snapshot text fixed
fixture matching. No production source normalization was performed.
An initial host invocation against the instrumented snapshot failed to link
CPU-local instrumentation; the host baseline uses restored original sources.
These failures are retained, not converted into passes by the later campaign.

## Final target invocation and concurrent documentation update

2026-10-14: `make test-network-smp-phase0` exercised the final Makefile target
and failure-retention code in `gate-4fs51srj`. All **12/12 guest cases passed**;
the campaign correctly ended **FAIL** at source-preservation validation because
AGENTS.md was updated concurrently outside this task. Its snapshot and the new
AGENTS.md are both retained; this task did not overwrite that update. The source
guard identified only AGENTS.md. The final saved-argv/exclusion audit separately
passed **12 audits / 61 negative cases**. This is not an unconditional target
PASS, and no guest rerun is claimed for the documentation-only drift.

Final instrumented kernel SHA-256:
`4a3a16b6b78e3ca87ae0824b582431f9284e8fd696e96395c1d0639b4137ca30`;
initramfs:
`b4fa2de13d519bd7fbbd9a3839f73572a9f89cc042a7158f959660c7f14ebcb0`;
baseline disk:
`16ec140b86b28a4277e6ce987614c4f1e38efda3b6e31075c563d76822bdc79c`.
The replacement AGENTS.md describes cryptographic ISNs with no quiet period,
while PROTECTED.md still specifies the old policy. The protected-policy gate
qualification therefore remains; no TCP behavior was changed.
