# Wi-Fi CCMP priority replay correction (2026-10-27)

## Physical observation and evidence boundary

User-reported Dell Latitude 5590, live USB, no Ethernet cable or dock:
Intel 8086:24FD is detected, Wi-Fi connects, and a fresh DHCP exchange obtains
192.168.0.120/24 with gateway 192.168.0.1. Pinging 1.1.1.1 then reports ARP
timeouts; ifconfig shows wlan0 UP, RX 8 / TX 14. This establishes partial data
connectivity, not a successful unicast gateway exchange. No over-the-air capture
establishes the traffic priority or packet numbers of the missing ARP replies.

## Confirmed software defect and correction

The manager previously used a single pairwise CCMP receive packet number for
all traffic priorities, and a separate single group receive packet number.
A valid frame with a lower packet number on a different priority was rejected
as a replay. This can suppress valid ARP traffic after other network traffic;
it is a candidate explanation for the physical failure, not a proven diagnosis.

Receive replay state now has 16 entries per key, indexed by the authenticated
QoS TID. Non-QoS data shares priority zero. Pairwise and group domains remain
separate. Both arrays live with the WPA2 session, so all existing key/session
wipes also clear replay state. Authentication still precedes counter updates;
same-priority duplicate packet numbers remain rejected. No CPU ownership,
driver DMA, polling, locking, firmware or public syscall ABI changes.

Primary implementation cross-checks: Linux mac80211
[QoS security index selection](https://github.com/torvalds/linux/blob/master/net/mac80211/rx.c)
and [per-priority CCMP counters](https://github.com/torvalds/linux/blob/master/net/mac80211/key.h).
The additional mac80211 counter for robust management frames is outside this
manager's protected data receive path.

## Verification

The new actual-manager regression failed before the correction at the expected
missing RX packet assertion. After the correction it passes for all 16 TIDs,
using descending packet numbers across priorities, while rejecting duplicates
within each priority. Further checks reject forged high-PN frames with bad MICs
without poisoning replay state, and verify non-QoS shares priority-zero history.
WPA2 initialization checks both arrays are cleared from dirty prior state.

Host ASan/UBSan: `make test-wifi-host test-wlan-crypto-host
test-wlan-mac-host test-wlan-wpa2-host test-wifi-fault-host` PASS. These use mock
radio delivery and real manager/crypto code, not physical Intel radio execution.
The fault suite includes 300 connection/disconnection cycles.
`make test-wifi-stack` PASS under the existing authorized 1072-byte
`wpa2_sm_rx_eapol` exception; manager receive frame is 720 bytes.
`make -j4` PASS, including verified combined live/installer image generation
(`build/root-phase4/media-ktga9kue`, 592 MiB published as `bin/fortress.img`).

## Physical follow-up: gateway still unreachable

The user's subsequent photo shows a fresh DHCP lease and four ARP timeouts
pinging 192.168.0.1. The replay correction has **not** resolved the reported
failure. Retain the independently reproduced correction, but do not claim it
as the physical root cause.

Added payload-free `[WLAN]` diagnostics, capped at four lines per category per
connection: ARP transmit submission/refusal, authenticated group/pairwise RX,
replay/crypto rejection, LLC decapsulation rejection, queue exhaustion, and
ARP delivery to the network stack. A transmit submission explicitly does not
claim radio ACK completion. No keys, passphrases or frame payloads are logged.
Actual-manager sanitizer tests verify repeated failures do not exceed the cap;
Wi-Fi fault/soak and stack gates still pass (receive frame 720 bytes).
Diagnostic image build `make -j4` PASS; combined-media verification passed at
`build/root-phase4/media-8takytyd` before publication to `bin/fortress.img`.

Next physical test on rebuilt media: connect, `ping 192.168.0.1`, then
`sudo dmesg -v -f WLAN -n 40` and `ifconfig`. These observations distinguish
several receive failures without treating mock-radio tests as physical proof.
