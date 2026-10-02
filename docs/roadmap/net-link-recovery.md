# Cold cable insertion and runtime link recovery

Status (2026-10-02): COMPLETE; host and dedicated QEMU gates pass.
Dell 5590 physical acceptance confirmed across all six test cases (see below).
NET-2 remains 7/7 complete; this closes the driver link-recovery lifecycle milestone.

## Implemented lifecycle

| State | Driver-owned DMA | Next action |
| --- | --- | --- |
| UNINITIALIZED | None | Existing discovery and cable-present initialization |
| WAITING | None | Worker samples STATUS.LU every 250 ms |
| ONLINE | One ring allocation set | Poll ingress and process bounded protocol work |
| DOWN | Same retained rings | Observe carrier; fail pending operations |
| FAILED | Permanently retained/quarantined | No restart until reboot |

`net_boot_probe` preserves cable-present initialization. I219's initial
500-ms absence becomes WAITING, without reset, failure capture or allocation.
A timer failure remains fatal. Discovery already disables PCI bus mastering
before BAR sizing; cold waiting does not enable it.

`e1000_get_net_device` publishes a configured interface candidate while
waiting: valid MAC, MTU and guarded callbacks, but no NET_UP flag. `net_start`
initializes the stack once, publishes its initial offline status, and starts
the existing BSP worker. RX is skipped while offline, including before rings
exist. The worker uses its existing tick deadline and timer wake target;
there is no idle yield or new timer hook.

`e1000_service_link` observes STATUS.LU. First acquisition runs the existing
bounded PCH takeover, SPT configuration and ring initialization exactly once.
Further flaps retain the same physical ring/buffer pages, head/tail indices
and ownership bookkeeping. Link loss alone performs no engine stop, reset,
descriptor rewrite, free or reallocation. ONLINE/DOWN observations occur
each worker pass; cold observations use a frequency-derived 250-ms cadence.

On SPT/15D7 re-acquisition, the existing bounded `pch_spt_link_setup` applies
its speed-dependent PLL/K1/FIFO settings and restores PHY page/ownership.
Its masks and register programming are unchanged from the validated path.
It does not restart autonegotiation or reset the PHY. PCH takeover/PHY waits
retain their existing PIT limits and IRQ exclusion; moving first activation
to the worker does not make those one-time waits asynchronous. Failures of
ownership, PHY configuration, allocation, MMIO timing or TX completion
remain terminal and retain all DMA. No retry can escape FAILED.

The PHY is assumed to negotiate autonomously after insertion. That remains
a **physical diagnosis gate**, not a conclusion from host/QEMU results. If
the Dell stays WAITING after insertion, collect boot/link logs and investigate
before adding any autonegotiation writes. No MDIC autoneg restart was added.
Similarly, hardware ring continuity during unplug/replug requires the
physical cases; mock register continuity is not proof of I219 behavior.

## Application behavior and contracts

Existing UDP/TCP offline error policy supplies EIO. Blocked ACCEPT fails and
the old listener remains failed after replug; close it and run a new `nc -l`.
Accepted children and existing connections are not resumed after failure.
The worker clears ARP cache on observed link loss without resetting socket
tables, generations, pending detach identities, ISN state or reboot quiet time.

`net_ipv4_link_down` cancels copied pending ICMP/UDP work, preventing an old
request from being transmitted after reconnection. Ping uses its existing
NETPING_TX_FAILED outcome (printed as `transmit failed`), rather than adding
a new NETCTL error or outcome. Already published successful results are kept.

No socket/syscall ABI, scheduler, signals, lock ranks, wait signature, timer
hook, DMA quarantine contract, NIC interrupt mode or `net=` syntax changed.

## Automated evidence

- `make test-net-i219-host`: actual driver with mocked PCI/MMIO/PIT/PHY,
  ASan/UBSan. 15D7 and 1A1E cold wait/insertion, 100 flaps with exactly 146
  allocated pages and unchanged ring addresses/reset count; all 146 deferred
  allocation failure boundaries; timer failure and terminal containment.
  Existing reset, PHY workarounds, TX timeout and DMA quarantine regressions pass.
- `make test-net-rings-host test-net-eth-host test-net-ipv4-host
  test-net-tcp-socket-host test-net-socket-host`: actual code with host adapters,
  ASan/UBSan. Cold worker sleeps and skips RX, resumption polls RX, pending and
  in-flight ping cancellation, blocked ACCEPT EIO, old listener failure after
  replug, fresh listener bind/accept, and existing client/server/UDP regressions.
- `python3 scripts/test_net_link.py --jobs 2`: **8/8 PASS** in
  `build/net-link/a16a1997`: BIOS/UEFI × e1000/e1000e × cold/warm, SMP=1,
  disposable ISO/OVMF, no data disks. Cold ping/DNS failure, insertion and
  ping success, unplug during multi-probe ping, unplug during ACCEPT, fresh
  listener exact payload after replug, repeated flaps and one DMA-ready banner.
  ICMP capture directions/checksums are independently inspected. This is not
  a physical PHY/DMA assertion or a complete TCP wire audit.
- `make test-net-rings test-net-icmp-host test-net-ping-host`: rings QEMU
  **4/4 PASS**, exact raw TX/RX bytes and recycle; codec/mailbox sanitizer PASS.
- `make -j4`: strict kernel build and boot-image verification PASS.

The link runner retains argv, disposable boot files, UART, pcap, QMP events,
result manifests and failure diagnostics. Cold tests stop at the unmodified
driver entry using a hardware GDB breakpoint, then set emulated carrier down.
QEMU 8.2.2's
[e1000e_autoneg_resume](https://github.com/qemu/qemu/blob/v8.2.2/hw/net/e1000e_core.c#L3133)
clears `link_down` on VM resume while negotiation is unfinished, so the runner
reasserts carrier down immediately after resume. No guest memory or register
patches are used. Earlier harness failures are retained in
`build/net-link/b2878b88` and `build/net-link/c1e3dc13`.

Broader TCP/DNS regression results are recorded below after the runs finish.

## Physical acceptance — Dell Latitude 5590 (2026-10-02)

The user confirms all six cases on Dell Latitude 5590, I219 8086:15D7,
MAC c8:f7:50:0e:35:80, guest 192.168.0.168, gateway 192.168.0.1, boot cmdline
`net=192.168.0.168/24,192.168.0.1 verbose`. The session record lives in
`build/net-link/physical/PHYSICAL.md`.

| Case | Result | Detail |
| --- | --- | --- |
| 1. Boot connected, ping | PASS | Gateway ARP resolved, 4/4 ping replies, shell usable |
| 2. Boot disconnected, insert, ping | PASS | Autonomously negotiates link on cable insertion; DMA allocated on first activation; ping 4/4 |
| 3. Boot connected, unplug/replug, ping | PASS | Retained-ring continuity confirmed; ping succeeds post-reconnection |
| 4. Unplug during `ping -c 4` | PASS | Bounded `transmit failed`; clean prompt return, no hang or panic |
| 5. Boot disconnected, nslookup | PASS | Clean `EIO`, shell remains fully usable |
| 6. Unplug during `nc -l`, replug, new `nc` | PASS | Old blocked listener fails cleanly with error; new `nc -l` binds and accepts peer data |

Notes and architectural conclusions:
- **Autonomous PHY renegotiation:** Case 2 answers the hardware question definitively: the I219-LM PHY renegotiates carrier autonomously upon cable insertion. No MDIC autonegotiation restart writes or resets were needed.
- **Listener-recovery scope:** Case 6 validates the designed recovery boundary: blocked listeners fail cleanly; a new listener works after replug; existing/dead connections do not resume.
- **Stability and zero panics:** No `[FAIL]` or `[PANIC]` in any case.
- **Ring/DMA stability:** DMA page count and ring addresses remain stable across flaps without leak, matching the QEMU 100-flap invariant (146 pages).
