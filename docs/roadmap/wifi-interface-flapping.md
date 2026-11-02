# Wi-Fi evicted by disconnected Ethernet — 2026-11-02

## Physical report and local reproduction

The installed Dell, on its normal charger with no Ethernet cable or dock,
repeatedly logs successful WPA M1–M4 completion, `eth0 -> wlan0`, then
`wlan0 -> eth0` and another authentication. This establishes a handover loop,
not a bad-password diagnosis. Bazzite's short probe received 4/4 replies;
subsequent loss cannot establish idle failure because it overlapped a reboot.
After the user confirmed the machine had booted and remained at login, a
20-probe run received only 1 reply and 17 local host-unreachable errors.
These observations alone do not identify every trigger of a radio state change.

The production selector in `src/net/net.c` chose an existing Ethernet device
whenever no interface reported online, even if Ethernet also had no carrier.
Switching away from WLAN then explicitly called `wlan_mgr_disconnect()`, cleared
ARP and discarded DHCP address configuration. A temporary Wi-Fi loss or another
connection attempt was therefore converted into an artificial wired handover
that aborted wireless recovery. The DHCP/startup retry paths could repeat it.

The normal `TEST_SMP_MEMORY` fixture bypassed the entire multi-interface
selector. The extended Ethernet fixture now opts into that actual code with
`NET_MULTI_IF_HOST`; scheduler, NIC and radio callbacks remain explicit mocks.
Before the fix it fails the assertion that Wi-Fi remains the selected interface
when both wired candidates have no carrier. The same regression passes after
the fix. This is an actual selection-code failure/reproduction, not proof of
Intel DMA/RF correctness.

## Fix

- Keep an existing selected interface when all candidates are offline; choose an
  offline fallback only when there is no selected interface at all.
- Disconnect Wi-Fi only when an actually online Ethernet or USB Ethernet link
  takes over. Preserve the established USB > Ethernet > WLAN online preference.
- `wifi auto` returns without rescanning/reauthenticating an already authorized
  link. If authentication/association/key exchange is in progress, leave it
  running and return a retryable unsuccessful CLI exit for the supervisor.
  Explicit user connect/disconnect operations retain their existing behavior.
  This is not a new central manager; DHCP remains separately supervised.

The CLI regression also fails before its fix: invoking auto on an already live
link increases the connect/spawn counters. After the fix it does neither, and
in-flight states issue no scan/connect/firmware/child side effects.

## Tests and boundaries

- `make test-net-eth-host`: ASan/UBSan actual selector, Ethernet, ARP and ICMP.
  One hundred Wi-Fi down turns retain identity and configuration without a forced
  deauthentication; recovery, real Ethernet takeover and real USB takeover pass.
  Three cleared-cache inbound ARP/echo cycles pass with no outgoing application
  ping. The fixture now supplies the real interface MTU for ICMP transmission.
- `make test-net-ipv4-host`: cold peer echo initiates ARP and sends the reply when
  resolved; inbound replies work again after idle. Corrected stale negative UDP
  fixture to use a non-DHCP destination port and separately assert offered-IP
  port-68 DHCP acceptance.
- Corrected the stale Ethernet ARP expectation: RFC 826 learning from an incoming
  targeted request already populates the cache. A different peer exercises the
  actual unresolved-neighbor path; assertions were not simply removed.
- `make test-wifi-profiles-host test-wifi-prepare-host test-wifi-host
  test-wifi-fault-host test-wifi-stack`: PASS, including credential retention,
  live/in-flight auto behavior, secure/open data paths, timeout and driver refusal.

Before/after logs are retained under `build/wifi-handover-evidence/`. Guest and
image verification are recorded there as they finish. Physical acceptance of the
changed kernel/CLI remains pending until deployed: no disconnected-Ethernet
handover loop, DHCP/DNS ready, inbound ARP/ping while idle and after reboot,
without a manual reconnect. Do not claim the old installation is fixed by
changing this checkout. Kernel CPU-0 ownership, ranks, polling and hardware power
settings are preserved; no speculative driver timing or radio reset was added.

## Replacement installer

The sole retained installer has SHA-256 `98bf2b01b2d9085e3859bd2e0afab81b61512eb9662a622c3b6a3459d3f38df6`,
528 MiB, and records clean source commit `1c2b88051bf9fe87d89134d95e5b300082acd5c6`.
The full kernel/C-tool rebuild, refreshed bootstrap and package/filesystem/media
audits passed. The updated CLI loadable ELF matches its bootstrap member, and
the release kernel contains no `net_test_service_active_interface` hook.
BIOS/UEFI × SMP=1/4 owner guest matrix PASS 4/4 (`qemu-1ye871x0`).
Raw-image boot evidence and final cleanup inventory are retained under
`build/wifi-handover-evidence/`. No physical Wi-Fi PASS is claimed.

Final raw-image BIOS/UEFI SMP=4 read-only boot checks PASS 2/2, reaching the
installer with no target disk attached; image SHA-256 stayed unchanged. The new
disposable guest ISO was removed afterward, leaving exactly one disk image.
