# Cold inbound ARP on Intel Wi-Fi — 2026-11-02

## Physical isolation

After the interface-handover fix, the installed Dell has a quiet log, an
associated/authorized Wi-Fi link and 192.168.0.194/24. It can ping 1.1.1.1.
Bazzite 192.168.0.159/24 cannot initially ping it. Initiating traffic from
FortressOS to Bazzite makes the reverse direction work.

The user independently ran this sequence on Bazzite, without first pinging
Bazzite from FortressOS:

1. Delete the .194 neighbor entry: ordinary ping receives 0/4 replies and local
   destination-host-unreachable errors.
2. Supply MAC f4:d1:08:c9:6b:e3 with `ip neigh replace ... nud reachable`:
   ping receives 4/4 replies, TTL 64, RTT 5.624–74.351 ms.

A separate unprivileged `arping -I wlo1 -c 4 -w 5 192.168.0.194` transmitted four
broadcast probes and received zero ARP responses. This isolates initial neighbor
resolution from the working unicast ICMP path. It does not prove whether each
ARP request reaches the Intel RX ring, fails group decryption or is lost earlier
on the AP. Supplying a neighbor entry is a diagnostic workaround, not a fix.

## Configuration defect and candidate correction

FortressOS installs no PTK/GTK in Intel firmware; `wlan_mgr` authenticates and
decrypts encrypted data in software. MAC_CONTEXT previously requested acceptance
of group frames but left firmware unicast/group decrypt-disable bits clear.
The firmware API provides independent DIS_DECRYPT bit 3 and DIS_GRP_DECRYPT bit 4.
Configure both for this software-crypto architecture, before and after
association. Do not enable promiscuous reception, broadcast filtering or FCS
appending, and retain software MIC/replay verification.

Primary API reference (fixed upstream revision):
[Linux v6.12 Intel firmware mac.h, enum iwl_mac_filter_flags](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/intel/iwlwifi/fw/api/mac.h#L210-L237).
This establishes the firmware flag meanings, not a physical PASS or proof that
missing flags are the sole cause of the observed loss. Linux's normal MVM
station path uses hardware keys, unlike FortressOS's software-crypto path.
The multicast command structure already matches the documented firmware layout;
it is not changed. Existing Dell REMOVE_STA workarounds and power settings are
preserved.

The actual MAC_CONTEXT serializer is extracted into `src/drivers/iwl_mac.c` so
host tests inspect the command sent by the production wrapper, rather than a
host stub that omits command generation. Inputs are copied owner-local metadata,
not credentials. Both ADD and MODIFY use the same builder. No user ABI, driver
ownership, lock ranks, interrupt policy or key authentication rules change.

## Regression evidence and limits

The independent wire contract test expects filter bytes 0x5e before association
and 0x1e after association at offset 52 of the 148-byte command. With the old
serializer expression it fails that assertion. After correcting the flags it
passes ADD/MODIFY across 2.4/5 GHz while checking addresses, reserved bytes,
EDCA masks, timing and beacon filtering. It also checks that promiscuous,
broadcast-drop and FCS flags remain absent. The test is a prerequisite of
`make test-wifi-host`.

`make test-iwl-mac-host test-wifi-host test-wlan-crypto-host test-wlan-wpa2-host
 test-wifi-fault-host test-wifi-stack` passes with ASan/UBSan where applicable;
CCMP/TKIP software authentication and replay tests remain enabled. Stack gate:
222 functions, 1024-byte default and existing authorized EAPOL exception.
Logs are retained under `build/wifi-cold-arp-evidence/`.

These are command-generation and software-MAC tests, not Intel firmware/DMA
execution. Deployment and cold physical ARP acceptance are still required.
Do not report the radio failure solved from these host results. The corrected
image must be tested after deleting Bazzite's diagnostic neighbor entry, with no
FortressOS-originated ping, then after idle and a reboot. If cold ARP still
fails, capture/observe raw group RX and authenticated-decryption outcomes before
making another radio change. Command completion is separate work and is not
edited by this fix.

## Candidate artifact

`bin/fortress-base-installer.img`, 528 MiB, SHA-256
`0688b23e6ab8bb74d221876c213ba85f172afbc13ae9cdfb778d8a57f21a57cc`. Clean isolated build commit `85e8484` includes the
completed command-completion change `8bc30ad`. Independent package, ext4 and
media audits pass; BIOS/UEFI × SMP=1/4 owner guests PASS 4/4. The corrected
firmware command is not physically validated by the mock-radio guest.
The user’s concurrent battery/desktop work is left untouched.

Final candidate gate: raw image BIOS/UEFI SMP=4 boot smoke PASS 2/2, with
no installation target attached and unchanged image SHA-256. Two owned
disposable image duplicates were removed; one disk image remains. Physical
Intel cold-ARP validation of this candidate remains pending.

## Physical follow-up: latest ACPI image still fails

The user confirmed installation of the latest ACPI/desktop image (SHA-256
`13283ee703b3fe9dfefc099a1ac3e2a81ef5f43d63c1be0f9357203ccfe88a9e`) and
unchanged FortressOS address `192.168.0.194/24`. After deleting the Bazzite
neighbor entry, ordinary ping again received no replies. The firmware-filter
correction is therefore insufficient; cold inbound ARP remains **FAIL**.

A measured follow-up used `arping -b -I wlo1 -c 10 -w 12 192.168.0.194`
on Bazzite: ten broadcast requests, zero responses. User-reported FortressOS
`ifconfig` counters changed from RX 43 / TX 8 to RX 79 / TX 8. These RX counters
increment after successful 802.11 decapsulation and queue admission, not at raw
Intel DMA reception. The increase proves some Ethernet traffic was admitted
during the interval, but does not identify it as the ARP probes; unrelated
broadcast/multicast traffic is not excluded. Unchanged TX provides no evidence
of transmitted ARP replies.

Existing diagnostics do not distinguish raw group reception, GTK authentication
or replay rejection, successful ARP decapsulation, and network-stack dispatch.
Stage-specific bounded diagnostics are needed before attributing the physical
failure to one of those stages or publishing another purported fix.
