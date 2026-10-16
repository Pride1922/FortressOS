# Network SMP Phase 4 — physical acceptance

2026-10-16. Physical testing is in progress. The user reports testing from a
live USB on a laptop. Full installed-root acceptance remains open under
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

The exact executed command, individual timings, minimum/maximum, server and
link details, and a downloaded-file checksum were not supplied. File creation
and the mean timing are user-reported; no capture or independent byte audit
is available. An earlier attempt returned `DNS_NXDOMAIN`; the exact failing
command was not supplied, so its cause remains unconfirmed.

## Remaining acceptance evidence

Record laptop model, firmware, CPU count, boot-image hash, configuration path
and exact commands/results. Confirm ordinary unpinned commands, authorized
`sudo ifup`, `ifconfig`, gateway ping, DNS, HTTP checksum, reboot configuration
persistence/reload, concurrent/background commands and cable loss/recovery
with shell recovery. Record approved physical AP execution evidence separately
from functional success. Live-USB downloads do not establish the plan's
installed-root/reboot gate. The user's other preliminary tests have not yet
been itemized here. Phase 3 automated acceptance remains separate.
