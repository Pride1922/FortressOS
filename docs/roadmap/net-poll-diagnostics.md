# Remaining TCP stalls: on-demand counters — 2026-10-04

The 1 ms RX grace Dell capture spans 0.708480 seconds for a stdout 1 MiB
transfer, with median data-to-ACK 1.806 ms, 55/151 server data records above
8 ms, and no observed sequence gaps/overlaps. User independently confirmed
the file download SHA-256. Retained capture: build/tcp-dell-rx-1ms-retest.pcapng.
Remaining timer-shaped delays justify measurement, not a larger budget yet.

Worker counters are static atomic uint64 values; no allocation, NIC access,
locks, per-packet output, counter reset or ABI change. BSP dmesg reads append
13 cumulative decimal NET POLL lines after user-range validation, with syscall
IRQs masked. Snapshot fields are sampled individually, not a coherent epoch.
Additional TSC reads/counter updates add small instrumentation overhead.

- clock-hz: current admitted clock; zero means unavailable or disabled.
- rx-frames: frames passed to net_input, including non-TCP/malformed frames.
- budget-starts: progress/work restarted the existing 1 ms deadline.
- rx-after-active-turn: RX batch observed with prior active budget state;
  this does not prove arrival before expiration or measure packet latency.
- expired: normal time-budget expiration transitions.
- backward-clock / iteration-backstop: failure transitions, each disables
  active clock use for this worker lifetime. These count once, not every idle turn.
- wait-returns: completed scheduler waits (not necessarily actual switches).
- tick-deadline-returns / hint-returns: deadline satisfied and/or work hint
  present on return; these can overlap, not exclusive wake causes.
- wait-ticks: BSP timer tick differences across waits, including ordinary idle.
- yield-over-1ms / max-yield-us: TSC interval across cooperative handoff,
  including preemption/other tasks and small instrumentation overhead.
  Microsecond conversion is approximate; invalid/backward intervals are skipped.

Compare before/after snapshots around one transfer; do not attribute all
cumulative idle waits to TCP. No exact per-packet scheduling correlation or
physical clock-fallback cause is claimed until the snapshots are available.

Host worker/socket ASan/UBSan and strict build/image PASS. Formatter tests check
small/zero capacity, unchanged guard byte, expected hint returns and no false
clock-failure counters. Fake-clock peer beyond the old grace, idle return and
budget failure/expiration tests remain. Live dmesg smoke gates recorded below.
Actual-worker injected backward and frozen clocks each publish the expected
failure counter once, set current clock Hz to zero and return to timer waiting.
BIOS/UEFI final-image USB smoke PASS 2/2, asserting every dmesg counter field,
real writes/sync/shutdown plus Linux bytes/fsck. Evidence:
build/ext4-usb/run-5ysiwhtw.

Image bin/fortress-ext4-test.img, local timestamp 2026-10-04 08:57:56.
SHA-256: 9a075f8330228db2acc2f2afaf4bbbcdda4d968155ccc31f8f29f0067db13ba1.

After 120-second TCP quiet period:

```sh
dmesg | tail -n 13
wget -q -O - http://192.168.0.153:8000/data-16m.bin | wc -c
dmesg | tail -n 13
```

Capture only the wget invocation; expected stdout count 16777216. Send both
counter snapshots, capture and elapsed time. No disk write is involved.
The 1 ms RX budget and 20 us TX fast stage are unchanged in this image.
