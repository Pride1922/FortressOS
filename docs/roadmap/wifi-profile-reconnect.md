# Wi-Fi credential retention and reconnect — 2026-11-02

The reported Dell installed-root screenshot shows a WPA2-PSK scan entry for
Halloween, followed by an invocation without a password, an OPEN association
and DHCP timeout. This is evidence of false local port authorization, not a
successful WPA handshake or evidence of a DHCP server fault.

## Changes

- An empty password no longer overrides beacon security. Protected networks
  reject missing credentials before authentication and return EACCES through
  the copied CPU-0 owner. The CLI explains how to provide the initial password.
  Uncached explicit BSSIDs also default to protected, never inferred open.
- Auto-connect previously passed saved-table fields into do_save(), which
  reloaded and cleared the same entry before copying its arguments. Independent
  snapshots now preserve SSID, password and IP settings through saving and DHCP.
- CLI profile writes use a private exclusive temporary file, checked mode 0600,
  complete short-write handling, fsync, atomic replacement and checked sync.
  Failed writes/replacements preserve the previous destination. A failed save
  is an error, rather than a successful connection claiming boot persistence.
- Profile read/write buffers support all sixteen bounded entries; the former
  2048-byte buffers could truncate or overflow serialized configurations.
  Reads now handle short reads too.
- An uncached directed scan reports EAGAIN through the owner so the CLI retries
  connection setup. It previously reported success without starting a connection.
- Failed firmware preparation in `wifi auto` now exits unsuccessfully so the
  existing Watchtower restart-on-failure policy applies. Wait loops use the
  actual Fortress EINTR constant and configuration waits check wait errors.

The enabled `wifi.service` already invokes `/bin/wifi auto` in both normal
initramfs and installed-root templates. Installed profiles reside in persistent
`/etc/network/wlan.conf`. Live ETCFS remains RAM-backed; these changes do not
add live-media persistence or preserve credentials across a destructive reinstall.
No password can be recovered from an already erased profile.

## Verification and limits

PASS:

- `make test-wifi-profiles-host`: actual CLI with mocked radio/child/filesystem
  syscalls; initial save, two simulated process-restart boot-auto invocations,
  credential retention, short reads/writes, failed write/rename preservation,
  sixteen-profile round trip beyond 2048 bytes. This is not ext4 reboot evidence.
- `make test-wifi-host test-wifi-fault-host test-wifi-prepare-host`: actual WLAN
  behavior with mocked hardware; missing-password refusal, existing secure/open
  flows, firmware preparation and 300 connect/disconnect cycles.
- `make test-wifi-owner-host`: actual owner/WLAN code under ASan/UBSan and TSan;
  4800 concurrent calls, missing-password EACCES and scan-retry EAGAIN coverage.
- `make test-wifi-stack`: 221 functions; 1024-byte default with existing
  authorized `wpa2_sm_rx_eapol` exception (1056 bytes).
- `make build/wifi.elf bin/fortress.elf`: changed CLI and kernel compile/link
  using existing build intermediates. Not a clean media build.

Attempted `python3 scripts/test_wifi_owner.py --cpus 1 4 --firmware bios uefi`.
Private snapshot first failed existing unused-but-set-variable errors in
memory_boot_test.c with this compiler. A fixture-only Makefile change retained
those warnings without making them fatal; retry then stopped because NASM was
absent. QEMU and xorriso are also absent. No guest boot PASS is claimed.
Attempt logs: `build/wifi-owner/qemu-5gw7kt9n/` and
`build/wifi-owner/qemu-xo61cl9n/`. Production warning flags were preserved.

Physical installed-root acceptance remains required: connect once with the real
password, verify profile save, obtain a lease and DNS, reboot without supplying
credentials, verify automatic association/lease/DNS, then repeat reboot. This
checkout has not been deployed to the user's laptop. Intel RF, DMA, DHCP delivery
and reboot durability are not established by mocked-radio tests.

## Complete installer follow-up

The existing `fortress-dev` Ubuntu container provides NASM, QEMU and xorriso.
The follow-up build uses that toolchain for the complete A/B installer, with
normal desktop factory packages and enabled Wi-Fi auto-connect service. Earlier
host-tool failures above are retained as historical attempts, not the final
verification boundary. See the final installer manifest for exact source hashes
and image identity. Physical installed-root Wi-Fi acceptance remains pending.
