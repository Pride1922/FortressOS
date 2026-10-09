# Dell environment decision handoff — 2026-10-09

Step 4a preparation is complete. The user subsequently ran the Dell tests;
[physical results and the environment decision](smpbench-dell-results.md)
are recorded separately. No physical device was flashed or rebooted by the agent.
No kernel/boot/storage contract was changed for this handoff.

## Frozen bundle

`build/dell-smpbench-20261009/` contains:

- `fortress-dell-smpbench.img`: 130 MiB dual-boot GPT/FAT32/ext2 USB image.
- Matching `fortress.iso`, `fortress.elf`, `initramfs.tar` and `smpbench.elf`.
- `SHA256SUMS`, `manifest.json`, Git state and image verification/configuration.
- `README.md`: physical setup, commands, collection and decision criteria.
- `commands.txt`: metadata, CPU reference, one-worker references, then
  profiling off/on/on/off for signals, spawn_wait and pipes on all online CPUs.

Image SHA-256:
`176b78dcda472c34e7e0c402e056e88a717783d1ff8bca23589b333fd87e3af7`.
USB data PARTUUID: `62A14422-1519-4AA0-8048-7EE9E9C7C43D`.

The regular-file packager verifies GPT/filesystem structures, extracts the raw
image's kernel/initramfs from FAT and checks exact hashes against the bundle,
then checks `/bin/smpbench` inside the archive against the saved tool ELF. It
also checks the ISO's kernel and initramfs match those same bytes. Existing
bundles can be checked with `--verify-only`; mismatched manifests fail.

The image preserves the existing generated menu: the first Persistent Storage
entry is explicitly RW for this USB PARTUUID, while Recovery is RO. The guide's
RO-default shorthand differs from that generated first entry; this handoff does
not change either behavior. For durable evidence select Persistent Storage and
verify `/mnt` is the selected USB. Recovery can be used with serial/display
collection instead. Internal NVMe exclusion and raw-write gates are preserved.

## Actual verification

```sh
make -j4
python3 scripts/prepare_dell_smpbench.py --output build/dell-smpbench-20261009
python3 scripts/prepare_dell_smpbench.py --output build/dell-smpbench-20261009 --verify-only
python3 scripts/test_dell_smpbench_image.py \
  --image build/dell-smpbench-20261009/fortress-dell-smpbench.img \
  --output build/dell-smpbench-usb-smoke-20261009
```

Build and frozen artifact verification PASS. A **UEFI QEMU TCG USB smoke at
SMP=1** on a disposable copy passed selected RW USB mount, real Ring 3 profiled
signals and pipes (one worker, one warmup plus one timed repetition, three
iterations), phase/barrier validation, clean shutdown and offline `e2fsck -fn`.
The frozen source image's hash remained unchanged. Exact argv/log/audit result
are retained under `build/dell-smpbench-usb-smoke-20261009`.

That runner uses the existing USB persistence runner's drive/device preflight:
only paired OVMF pflash and the disposable USB image are attached, no NVMe or
other data disks. It has bounded waits and QEMU cleanup. This USB smoke is not
a physical pass, an SMP=8 USB test, a performance trial or a new panic-capture
validation. [Step 3's](smpbench-profile.md) ISO matrix supplies the prior SMP
profiling evidence. To prepare another frozen bundle use
`make prepare-dell-smpbench`; it never flashes a device.

## Physical procedure and pending inputs

Boot the frozen image on the Dell using a deliberately selected test USB and
follow the bundle README. Keep power/thermal conditions and background work
consistent and record actual online CPU count. One worker on an SMP Dell is
not equivalent to a single-CPU kernel boot; compare actual worker counts and
CPU placement with matching QEMU cases.

Corrected commands redirect records to separate `/mnt/dell-smp-*.txt` files
on the selected RW USB. Retrieve all files after `sync` and clean shutdown.
Alternatively omit redirection and capture terminal/serial output.
Check `ok=1 short=0`, rev=2 vs rev=3 and
the guest `/bin/smpbench` SHA-256. Keep full records and boot/exception output;
QEMU's all-CPU frozen RAM capture is unavailable on physical hardware.

Off/on/on/off here compares instrumentation modes of one frozen binary. It
limits simple ordering drift but is not the before/after optimization trial
of step 4b. Profiling changes execution and output volume, so keep its throughput
separate from plain results. No arbitrary speed or variance cutoff is imposed.
USB result collection can affect subsequent cohorts and is included in parent
collection timing; parent printing is outside worker timing. Keep capture mode
consistent and confirm performance conclusions using terminal/serial capture.

### Capture correction

The initial handoff incorrectly redirected full output to `/tmp`, whose
per-file capacity is 4096 bytes (`tmp_mem_file_t` in `src/fs/vfs.c`). The user
reported repeated `smpbench: write error` on the Dell during the CPU reference
command with five repetitions. That redirected log is incomplete and must be
rerun. Earlier smoke coverage checked terminal output, not redirected logs.
The procedure is corrected without changing the frozen kernel or image.
This output-capacity failure supplies no evidence of a kernel performance
regression or a completed physical benchmark pass.

The next input needed is all `dell-smp-*.txt` files (or full captured output), model,
firmware mode and actual CPU/power conditions. Then decide whether the long SMP
tails and spawn/signal/pipe phase distribution persist on hardware. Stable Dell
results alongside unstable QEMU results support using Dell for effect
measurement while retaining QEMU for correctness. Similar hardware slowdown
supports investigating the identified kernel paths. Mixed results remain
inconclusive. No environment decision or additional optimization is claimed yet.
