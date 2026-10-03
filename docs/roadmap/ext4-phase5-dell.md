# EXT4 Phase 5 Dell test checklist

Performance/download hashes, clean reboot persistence, quiet background console responsiveness and offline Linux fsck manually PASS on 2026-10-03; see [recorded observations](ext4-usb-performance.md#dell-physical-performance-acceptance--2026-10-03). Checklist item 3 boot admission fields are now supplied in the linked record, including the complete matching PARTUUID. Physical post-sync mutation and namespace overwrite/delete persistence are now user-confirmed PASS in the consolidated acceptance record. The checklist below is retained for those remaining checks and repeat runs. Use the designated disposable USB confirmed by the user. This image is a bounded non-journaled filesystem test; it does not convert or preserve an existing USB filesystem. No hardware flash has been executed by the agent.

Performance follow-up: reflash the refreshed image containing the commit/batching
and [USB transport fixes](ext4-usb-performance.md). The latter reduces 4 KiB
writes from eight BOT commands to one and improves short-completion polling.
Record elapsed time for the 1 MiB wget file download and compare with
`wget -q -O - http://192.168.0.153:8000/data-1m.bin | wc -c`.
The earlier image took over three minutes to save versus three seconds to stream;
the later commit/batching image took approximately 50 seconds per MiB, still
failed performance acceptance. Check the 1 MiB hash before proceeding
to 16 MiB. The rebuilt image has a new PARTUUID in `build/ext4-dell/image.json`.

1. Flash the separate `bin/fortress-ext4-test.img` to that disposable USB using your normal raw-image/Rufus workflow. Leave `bin/fortress.img` as the ext2 image. Boot the entry labelled EXT4 E4-A TEST - NO JOURNAL without verbose.
2. A raw image is 130 MiB. If the USB is larger and the flashing tool leaves the backup GPT at the image boundary, FortressOS deliberately mounts RO. Do not override that policy. On this designated disposable USB only, a Linux GPT tool can explicitly relocate the backup table to the physical end without converting or resizing the filesystem. Verify the actual target disk and preserve the data PARTUUID. Then boot again and require a consistent GPT/read-write mount. No automatic GPT repair is implemented or executed here.
3. `make image-ext4` also prepares `build/ext4-dell/SHA256SUMS`, HTTP payloads and `image.json` (image hash/data PARTUUID). Record the USB identity/PARTUUID, GPT policy, durability class, `Selected filesystem: ext4` and actual read-write mount mode. Internal NVMe must remain unmounted. Record any error and console responsiveness during large writes.
4. On the Windows HTTP host, serve the prepared deterministic payload directory:

```powershell
python -m http.server 8000 --directory C:\Sources\FortressOS\build\ext4-dell
```

5. Configure networking using your working ifup settings. The following uses the previously configured LAN addresses; adjust them if your current configuration differs:

```text
ifup 192.168.0.168/24 192.168.0.1
wget -O /mnt/data-1m.bin http://192.168.0.153:8000/data-1m.bin
wget -O /mnt/data-16m.bin http://192.168.0.153:8000/data-16m.bin
wc -c /mnt/data-1m.bin
wc -c /mnt/data-16m.bin
sha256sum /mnt/data-1m.bin /mnt/data-16m.bin
sync
echo after-sync > /mnt/sync-check.txt
mkdir /mnt/e4-check
echo first > /mnt/e4-check/check.txt
echo second >> /mnt/e4-check/check.txt
mv /mnt/e4-check/check.txt /mnt/e4-moved.txt
cat /mnt/e4-moved.txt
poweroff
```

Expected sizes: 1048576 and 16777216. Expected hashes:

- `470952a05336a638e11755d028432cb890c3240d0b33668038a975e7e3b5b4ef  data-1m.bin`
- `71b83c780eeb1a2665d5aabf71229490ae069450ef9b88ca91d1229fe17f45c6  data-16m.bin`

6. Boot again, repeat size/hash checks and `cat /mnt/sync-check.txt` / `cat /mnt/e4-moved.txt`. Overwrite the moved file with `echo replacement > /mnt/e4-moved.txt`, confirm only replacement remains, remove it and the empty directory, sync and clean poweroff. Boot a third time and verify the deletion persists and both downloaded hashes still match.
7. After clean shutdown, use Linux to run read-only `e2fsck -fn` on the identified test USB **data partition**, and independently compare its downloaded files with the server payload hashes. Record the exact device/partition and output. Do not use a repair run as evidence.

Report firmware/model, USB identity/speed/durability, exact byte counts/hashes, successful sync followed by another write, overwrite/delete/reboot persistence, console responsiveness and offline fsck. These observations are required to close the physical E4-A gate; automated QEMU acceptance does not close it.

## Final acceptance status

**E4-A physically accepted — Dell 5590 — PASS (2026-10-03).** All seven
checklist items are explicitly user-confirmed. Earlier pending/failed notes
are historical checkpoints superseded by the [complete acceptance record](ext4-phase5-acceptance.md).
