# Installer payload request batching — 2026-11-03

Follow-up to the [ext4 performance release](ext4-performance-first-release.md).
Production checkpoint: `3db7077`; independent host matrix runner: `8f6e896`.

The installer previously copied 321 MiB using 4 KiB requests. Each write and its
readback independently refreshed the full target identity. On NVMe each refresh
issues three IDENTIFY commands. This amplified the administrative work even
though the data path already accepted multi-sector requests.

The trusted runtime now opts into 16 KiB payload requests. The generic engine
accepts only 4, 8 or 16 KiB, with zero preserving legacy 4 KiB behavior. Its two
buffers live in the existing heap-owned workspace, adding 24 KiB; no large stack
buffer, DMA allocation or hardware transfer size is introduced.

Each logical request still refreshes the selected target's complete identity.
The production backend checks the entire request's capacity before dispatch and
splits it into at most four existing 4 KiB storage calls. This works with NVMe
and the existing SATA block-run limit. A failed transfer immediately fences
subsequent I/O; accepted prefixes are neither retried nor rolled back. This
increases the bounded logical request from 4 to 16 KiB, and therefore the interval
between identity refreshes, without caching identity or bypassing its checks.

Immediate write/readback comparison, source SHA-256 validation before admission,
copy-time hashing, final boot payload verification, invalid readiness during
provisioning, A/B readiness publication order and all flush barriers remain.
Filesystem metadata requests and the public raw-control syscall's 8-sector bound
remain unchanged. No driver lifecycle, DMA quarantine or target exclusion changes.

## Verification

Evidence is retained under `build/payload-performance/` and the clean worktrees
`.codex-remote-attachments/payload-performance-{host,release}/build/`.

- Focused ASan/UBSan transport test: every length 1–32 sectors, last valid run,
  invalid/overflowing ranges before any I/O, and failure at each of four hardware
  transfers. Failed prefixes are not retried and all further I/O is refused.
- Actual runtime adapter test: one identity call for a 16 KiB request and changed
  identity refusal before dispatch. Existing metadata bulk bounds still pass.
- Strict kernel compilation and complete clean-checkpoint kernel/userland build.
- Actual runtime deployment with Linux FAT/ext4 audits: PASS in the isolated
  release worktree, including accounts, permissions and installation logs.

An initial main-workspace runtime audit reached its source-preservation guard
while parallel edits were occurring; it was repeated in an isolated checkout.
A second attempt reused old templates with the new kernel; the
kernel/payload matching gate rejected it before writes. Matching freshly generated
templates passed. The first image producer lacked its Limine dependency; dependency
files were copied before retrying. These failures remain in the evidence logs.

Physical Dell performance acceptance is pending. Fewer identity calls do not
establish a proportional reduction in total installation time. The replacement installer is published only after the complete host and guest
acceptance gates pass.


## Final results and release

All three ASan/UBSan profiles pass 292 deployment cases each (876 total), plus
1548 archive cases and an independent Python GPT/MBR/payload byte oracle. Final
successful deployment request counts, including fixed metadata/provisioning
fixture operations:

| Payload run | Writes | Reads |
| --- | ---: | ---: |
| 4 KiB (legacy) | 82250 | 98953 |
| 8 KiB | 41162 | 49545 |
| 16 KiB | 20618 | 24841 |

The payload-only write/readback identity-refresh request count falls from
164352 to 41088 (75%); fixed metadata operations remain. These are request counts,
not measured Dell installation time. The hardware data-transfer count is unchanged.
Receipt: `.codex-remote-attachments/payload-performance-host/build/root-phase4/payload-a6otv94b/manifest.json`.

The final uninstrumented image passes BIOS and UEFI USB installation onto fresh
regular-file targets, both without the disposable authorization bypass. Each
target boots twice at SMP=4 without USB, using its original authenticated boot
menus and graphical default. SSH login/elevation/shutdown, saved-note persistence,
all five factory app ELF byte comparisons and Linux `e2fsck -fn` audits pass.
Receipt: `build/payload-performance/acceptance/manifest.json`. No physical disk
was written and no physical or SATA guest performance result is claimed.

Published installer: `bin/fortress-base-installer.img`, 528 MiB (553648128 bytes).
SHA-256: `7d9e2c14162949ab8967c3086563ab99dc9694ddf7ae100c2435dc7bf7c7eec2`.
Built from clean source `3db70778ad4e5c7a0c9495fe9e5b5944f6aadcb3`. It retains the
previous committed desktop/Calculator, networking and metadata work. Parallel
uncommitted ACPI/Vault/terminal changes are excluded. The prior published image
is replaced only after all gates pass; its sidecar receipts are retained.

An initial in-flight payload host run was stopped after the test matrix was
extended; the final clean worktree run above covers all three sizes with the
complete failure cases. The earlier aborted run is not claimed as passing.
