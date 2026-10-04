# I219 TX completion fast polling — 2026-10-04

The Dell supervisor retest capture contains two 1 MiB transfers at 2.219816
and 1.600583 seconds (SYN through final ACK), versus approximately 5.804 seconds
before the supervisor fix. Window reopening is now approximately 1.1 ms.
Flow commands are unknown; FIN does not establish reader termination, and
server capture timestamps do not measure client scheduling or LAN RTT.

The I219 send path previously checked descriptor DD once and, on a miss,
waited a verified PIT millisecond before the next check. It now rechecks DD
using the existing BSP boot-calibrated invariant TSC for up to 20 microseconds,
bounded independently by 4096 clock checks. Completion bookkeeping remains
under the device lock; clock reads and pause instructions run outside it.
The fast stage runs once per submission, not once per slow interval.

Unavailable, backward, stalled or expired clocks select the existing fallback:
100 verified PIT millisecond intervals, timer-failure containment and permanent
DMA quarantine. The additional fast stage can add up to 20 us of active time
before that unchanged fallback budget (preemption can extend wall time).
No new IRQ enabling, sleeping, runtime PIT ownership or hardware workaround
changes. Non-I219 polling remains unchanged. RX grace remains 200 us so the
physical comparison isolates this change; receive fallback remains a hypothesis.

Host ASan/UBSan I219 and ring regressions PASS. New tests exercise fast DD with
zero PIT waits, expiration, frozen/backward clock, original timeout and retained
DMA. Existing tests cover unavailable clock, late 90 ms DD and timer failure.
Strict build and optional EXT4 image construction PASS. Physical performance
is pending; no predicted completion rate or Linux-equivalent speed is claimed.
`make test-net-rings` PASS 4/4 (BIOS/UEFI, e1000/e1000e); this is QEMU
descriptor/wire regression evidence, not physical I219 timing evidence.

Image: `bin/fortress-ext4-test.img`, timestamp 2026-10-04 08:20:13 local.
SHA-256: 1eed2b4afa0197bbc0aee98554ce61077893d6b0148961c77692bb5775a7eeed.

Retest after the 120-second TCP quiet period, capture each command separately:

```sh
wget -q -O - http://192.168.0.153:8000/data-1m.bin | wc -c
wget -q -O /mnt/data-1m.bin http://192.168.0.153:8000/data-1m.bin
sha256sum /mnt/data-1m.bin
sync
```

Expected count: 1048576. Expected SHA-256:
470952a05336a638e11755d028432cb890c3240d0b33668038a975e7e3b5b4ef.
Record elapsed time and preserve capture independently for each flow.
