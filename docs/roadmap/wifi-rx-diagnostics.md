# Wi-Fi receive-stage diagnostics — 2026-11-02

Cold inbound ARP is still physically failing on the latest ACPI image. See
[witnessed failure and counters](wifi-cold-arp.md). This change adds observation,
not another claimed radio fix. Existing ACPI, desktop, completion and network
behavior is retained.

## Reading the snapshots

On FortressOS, after boot/automatic connection and before pinging Bazzite:

```sh
sudo dmesg -n 4
```

Record the four RXDIAG rows (screen wrapping may make them occupy more lines).
Then on Bazzite, with FortressOS still idle:

```sh
arping -b -I wlo1 -c 10 -w 12 192.168.0.194
```

Repeat the FortressOS command and compare both snapshots. Use its current IP
if DHCP assigned a different address. A static neighbor is not needed for this
broadcast-only probe. Do not initiate a FortressOS-to-Bazzite ping first.

- `IWL`: valid-length firmware MPDUs, data/group frames before CRC/overrun
  rejection, bad status, software ring-full drops and admitted raw frames.
  `last-status` is the last MPDU firmware status word in decimal. These counts
  include management frames/other BSS traffic where applicable; they do not
  prove an absent frame was transmitted over the air or forwarded by the AP.
- `WLAN`: manager raw/data-group/target-BSSID group reception, authenticated
  group decrypt success, replay rejection, other decrypt failure. `cipher`
  (2 TKIP / 4 CCMP), GTK length/index and packet key index describe the last
  encrypted group attempt. They remain zero before such an attempt. A decrypt
  failure includes format, integrity or key errors; it is not solely a MIC error.
- `DECAP`: successfully queued Ethernet frames, ARP EtherType frames, full
  Ethernet ring drops and LLC/SNAP decapsulation failures.
- `ARP`: input, wrong-interface and malformed rejection, local-IP requests,
  reply TX callback success/failure and general Ethernet dispatch rejection.
  TX success means accepted by the driver, not independent over-air delivery.

Counters are boot-cumulative, never reset on read or reconnection. Each value
is atomic, but the rows are not one coherent instant. Compare deltas; unrelated
LAN traffic is possible. No keys, passwords or packet payloads are exposed.

## Ownership and bounds

Writers remain in existing CPU-0 polling/receive paths. Readers perform only
relaxed atomic loads and bounded decimal formatting, with no radio calls,
allocation, extra locks, sleeps, interrupt changes or packet-path logging.

SYS_DMESG retains root-only authorization and writable-range validation. The
new rows are copied into the current caller's validated output after the log
snapshot, on BSP or AP, rather than adding AP writers to the shared log ring.
Small buffers that cannot hold the complete diagnostic tail retain ordinary
log-read behavior. Existing public structures/syscall numbers are unchanged.
Each formatter publishes complete newline-ended rows within its supplied cap.

## Validation

ASan/UBSan actual-manager tests distinguish successful encrypted TKIP group ARP,
replayed packets and fresh-PN corrupt ciphertext; the latter two cannot reach
Ethernet. Snapshot reads perform no radio TX. Actual Ethernet/ARP tests verify
local-request and reply success/failure counters, formatting bounds and the
full uint64 decimal range. Concurrent snapshot reads join the existing owner
host ASan/UBSan and TSan tests. Wi-Fi fault and stack gates remain enabled.

The private Ring 3 ownership probe also checks root diagnostic snapshots on
every configured CPU, unprivileged refusal, invalid-pointer rejection, output
clamping and small-buffer canaries, plus the actual `sudo dmesg -n 4` command.
Guest/production-build results and artifact identity are recorded after running
those gates. QEMU uses a mock radio and cannot validate Intel DMA/RF reception.
Physical diagnosis remains pending installation and the paired snapshot test.
