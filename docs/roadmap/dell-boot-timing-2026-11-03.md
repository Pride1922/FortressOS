# Dell physical boot timing — 2026-11-03

Authenticated SSH capture from the newly installed complete image, kernel release
`boot-timing-2026-11-03`, `base_layout=1`, eight CPUs, Hynix NVMe namespace with
500118192 512-byte sectors. Root partition: 499982991 sectors. Source image:
`c90403dcf73940630b334cc0021882a5af553f8e47c25256c3783d287d0c2063`, checkpoint
`11df26d`. Sanitized original capture: `build/boot-timing-release/dell-first-boot.log`.

User-reported installation elapsed time: **137 seconds**, compared with the
previous reported 135 seconds. One run of each does not establish a meaningful
change in installation speed. Earlier 21/20/19.85-second cold boots belong to the
previous image; no new stopwatch boot time was supplied with this capture.

## Measured scopes

KERNEL total, from `kmain` entry to kernel-ready: **27780 ms**. This excludes
firmware/bootloader time and subsequent greeter rendering.

| Kernel phase | Elapsed ms |
| --- | ---: |
| Early platform, before calibrated clock | 9724 |
| Input and USB | 1415 |
| Network controllers | 576 |
| Persistent-storage/root startup | 16061 |

ROOT total: **16060 ms**, nested within persistent-storage startup.

| Root phase | Elapsed ms |
| --- | ---: |
| Boot credentials/readiness | 953 |
| Bootstrap CLI | 0 |
| Device inventory | 3063 |
| Selected disk/GPT | 3 |
| Complete A/B base boot-set validation | 7727 |
| Flush preflight | 0 |
| Filesystem preparation | 4281 |
| Installed-system validation | 29 |
| Publication | 2 |

EXT4-MOUNT total: **4281 ms**, nested within ROOT filesystem preparation.

| Filesystem phase | Elapsed ms |
| --- | ---: |
| Workspace allocation | 1 |
| Journal source | 78 |
| Journal analysis | 0 |
| Recovery preview/ownership validation | 2185 |
| Journal replay | 0 |
| Reopened ownership validation | 2014 |
| Journal activation | 2 |
| Remaining phases | 0 each |

Do not add nested totals. Integer rounding explains small differences between
individual phase sums and their enclosing totals. These fixed boot records are
separate from the on-demand accumulated EXT4 PERF counters.

## Findings and implementation priorities

The visible storage startup delay is not entirely ext4: complete base boot-set
validation takes **7.73 seconds**, while ext4 takes **4.28 seconds**. This capture
explicitly uses the A/B base layout; the preceding supplied log lacked
`base_layout=1`. The old stopwatch baseline and this new layout are therefore
not a controlled before/after performance comparison.

`base_system_boot()` calls `base_update_prepare()`, which reads the 64 MiB ESP,
validates its FAT/layout and boot states, checks the running bank, and binds
loaded module hashes to the admitted state. Next isolate raw reads, FAT/layout
work and hashing inside this measured 7.73-second phase. Optimize redundant work
or processing while preserving all boot-set, rollback and integrity checks.
Do not skip boot authentication to reduce the measured time.

Device inventory includes NVMe/GPT discovery and an offline AHCI port. The AHCI
classifier has a bounded 3000-ms link-initialization path; its offline-port log
and the measured 3063 ms are consistent with that wait. A dedicated subphase
would establish attribution. Existing offline-drive wake/ownership behavior
must not be removed merely because this laptop boots from NVMe.

Early platform initialization takes **9.72 seconds** and still needs finer
breakdown. This scope includes several subsystems; the battery's pending/EC-busy
message alone does not establish that ACPI accounts for all of that time.

Root admission succeeded and all reported ext4 read failure/anomaly counters
were zero. Plain `sudo /bin/shell` authenticated successfully. The empty crash
section in this capture is not a physical graphical-gterm acceptance test.

Only diagnostic reads and an authenticated root shell were used; no reboot,
repair, profile change or deployment was performed. The old SSH host key was
replaced following the user-confirmed reinstall, the known Wi-Fi MAC matched,
and the newly presented fingerprint was pinned. The session was closed.
