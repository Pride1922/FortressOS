# Phase 9G.4 — USB Writable Persistence & Durability Classification

Status: COMPLETE (2026-09-19). See `AGENTS.md` status table for current
summary; this file holds the detailed implementation notes and evidence.

Phase 9G.4 completes the USB storage stack: `/mnt` mounts read-write on real
hardware, files written from the shell persist across a full power cycle,
and durability is classified per-device with an explicit disclosure when the
device cannot be classified strongly. The work spans three sub-problems —
BOT stall recovery, SCSI cache-policy discovery, and mount eligibility —
each verified independently before the whole path was exercised end-to-end.

## What was built

### BOT stall recovery (Commit 1b)

- `xhci_bot_endpoint_reset()` issues Stop Endpoint, Reset Endpoint, Set TR
  Dequeue Pointer (with DCS bit set), and CLEAR_FEATURE(ENDPOINT_HALT) in
  sequence.
- `xhci_bot_transfer()` attempts a single bounded endpoint reset on a stall
  before latching the device offline.
- `latched_offline` is distinct from `transport_failed`; both prevent
  further BOT submissions and retain DMA allocations until reboot.

### SCSI cache-policy discovery (Commit 2)

- `MODE SENSE(6)` and `MODE SENSE(10)` caching page `0x08` support,
  requesting current values only. `MODE SELECT` is not implemented and
  device cache settings are not modified.
- Fallback sequence: `MODE SENSE(6)` first, then `MODE SENSE(10)` if the
  page is not found or the command is rejected. Transport failures stop the
  probe; command rejections do not.
- `xhci_scsi_probe_cache_policy()` parses the mode header, block descriptor
  length, page code, and page length independently, validates each against
  transferred byte count, and reads `WCE` / `RCD` / write-protect.
  Malformed, truncated, missing, or conflicting reports mean unknown.
- A separate `SYNCHRONIZE CACHE(10)` probe with `IMMED=0` records whether
  the device supports durable flushing. Command rejection is captured as
  `command_failed`, not `transport_failed`.

### Durability classification (Commit 3)

- `xhci_bot_probe_durability()` runs after block registration and before
  mount. It stores one of: `SYNC_BACKED`, `WRITE_THROUGH`,
  `ASSUMED_WRITE_THROUGH`, `READ_ONLY`, or `UNKNOWN` (probe not completed).
- Classification rules: explicit `WCE=0` → `WRITE_THROUGH`; working sync →
  `SYNC_BACKED`; `WCE=1` with failed sync → `READ_ONLY`; no page and no sync
  with healthy transport → `ASSUMED_WRITE_THROUGH`.
- `xhci_bot_flush_barrier()` is mode-aware: `SYNC_BACKED` requires the
  command to succeed; `WRITE_THROUGH` succeeds immediately;
  `ASSUMED_WRITE_THROUGH` attempts sync and succeeds even if the device
  rejects, failing only on transport loss. Modes latch to `READ_ONLY` on
  transport failure and are not silently changed.
- The `[USB DURABILITY]` boot dump prints the raw MODE SENSE attempts,
  WCE/RCD, sync result, classification, and mount eligibility. When
  classification is `ASSUMED_WRITE_THROUGH`, a three-line disclosure states
  that the device does not report cache policy, that write-through is
  assumed matching Linux and Windows, and that power-loss during writes may
  lose data.

### Mount eligibility & normal sync (Commit 4)

- `usb_mount_production_storage()` permits RW for `SYNC_BACKED`,
  `WRITE_THROUGH`, and `ASSUMED_WRITE_THROUGH`; RO otherwise. It still
  requires explicit `usb_data_mode=rw`, a matching PARTUUID, USB parent
  provenance, a strictly consistent or degraded-primary GPT policy, and a
  successful flush preflight.
- `usb_mount_sync()` flushes the mounted block device without marking the
  ext2 filesystem clean. It is deliberately separate from
  `ext2_sync_all()` (which sets `EXT2_VALID_FS` and freezes writes for
  shutdown).

## Verification Evidence

### Host ASan/UBSan unit suites

- `python3 scripts/test_xhci_bot_host.py`: stall recovery paths, MODE
  SENSE(6/10) parsing matrices (legal short replies, WCE on/off,
  write-protect, wrong page/subpage, bad lengths, zero sense, conflicting
  responses), and the full durability policy table.
- `python3 scripts/test_usb_mount_host.py`: mount eligibility across all
  durability modes, degraded GPT, missing write/flush callbacks, ext2 RW
  failure fallback, and `usb_mount_sync()` success/failure.

### QEMU three-boot persistence

`make test-usb-persistence` passed BIOS and paired-OVMF UEFI three-boot
create/read/overwrite/delete cycles on disposable 130 MiB USB images, with
offline `e2fsck -fn` returning zero after every clean shutdown. The runner
validates final QEMU argv (only the disposable USB data device, with
read-only firmware and disposable vars permitted). These are clean-shutdown
tests with substring content assertions; they do not establish physical
power-loss resilience.

### Dell 9G.4 hardware verification (2026-09-19)

User confirmed that Phase 9G.4 passed on physical Dell Latitude 5590
hardware with the Kingston USB DISK 2.0 (VID `0x13FE` PID `0x4200`,
30,320,640 sectors, 512 bytes/sector):

- `MODE SENSE(6) page 0x08: not found` and
  `MODE SENSE(10) page 0x08: not found`.
- `SYNCHRONIZE CACHE test: failed/unsupported`; command-failed CSW, sense
  `0/0/0`.
- `Classification: ASSUMED_WRITE_THROUGH`, disclosure printed at boot.
- `Mount mode: read-write`;
  `[USB 9G.4] PASS: Mounted sdap2 read-write at /mnt`.
- A file written via the editor on the Dell survived a full power cycle
  (power off, stick physically removed, reinserted, rebooted).
  Photographic evidence recorded.
- `e2fsck -fn /dev/sda2` on the stick from Linux reported 0 errors on two
  consecutive runs: one with the device mounted (kernel-cached view,
  warning noted), one after `umount` (raw on-disk view). File and block
  counts stable (14 files, 2091/65536 blocks).

The SanDisk USB 3.x stick was also tested and correctly identified as a
SuperSpeed device on Port 0x12, then skipped by 9G's scope at the time this
phase completed. That gap was closed by Phase 9G.5b — see
`phase-9g5-superspeed.md`.

## What this phase, on its own, did NOT establish (later closed by 9G.5b)

- **Strong durability on physical hardware.** No stick tested at this point
  reported a caching page or accepted `SYNCHRONIZE CACHE`. The
  `WRITE_THROUGH` and `SYNC_BACKED` paths were QEMU-verified only as of this
  phase.
- **USB 3.x devices.** SuperSpeed support was a prerequisite for physical
  verification of the strong durability paths, not a convenience — this
  motivated Phase 9G.5. Update (2026-09-20): SuperSpeed was implemented as
  Phase 9G.5b, and the strong path was verified on the SanDisk the same
  day — classification `SYNC_BACKED` on physical hardware. See
  `phase-9g5-superspeed.md`.
- **Physical power-loss tolerance.** The `ASSUMED_WRITE_THROUGH` disclosure
  states the boundary explicitly: clean shutdown is assumed durable;
  power-loss during writes may lose data. Abrupt-stop tests are simulated
  (mock volatile cache, injected flush failures), not physical. This
  remains an open gap, not closed by 9G.5b.
