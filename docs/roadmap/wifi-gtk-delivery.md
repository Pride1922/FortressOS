# Initial GTK delivery failure — 2026-11-02

Physical live USB diagnostics show an authorized link and successful unicast
DHCP at 192.168.0.194/24, but no installed GTK (length/index zero) while group
cipher is TKIP (2). Target-BSSID group RX/decrypt-failure counters progressed
32 -> 166 -> 257 with zero group successes, no CRC/ring-full/decap failures.
The measured Bazzite ten-broadcast ARP burst received zero responses.
Three ARP packets did reach Ethernet, including two local-target requests and
two accepted reply sends; the failure is not universal ARP dispatch failure.
Those particular packets were not captured, so their source is unproven.

## Reproduced code defect and correction

The AES unwrap helper accepted at most 64 bytes of plaintext, although the M3
caller permits 256 wrapped bytes. A normal combination of RSN IE, TKIP GTK,
IGTK and padding can exceed that limit. The handshake ignored unwrap failure
and could authorize a link with no GTK, producing working pairwise traffic
while dropping encrypted broadcasts. The AP's actual M3 bytes have not been
captured: the oversized block is a reproduced candidate cause, not proof of
its exact packet length on this laptop.

The unwrap helper now handles bounded 24..256-byte wrapped input (up to 248
plaintext bytes). Its 248-byte temporary array keeps successful-only output
publication and integrity verification. No heap/ownership/IRQ changes.
M3 key-data lengths are bounded by the authenticated EAPOL body. Failed unwrap,
missing GTK, malformed delivery or wrong GTK length (16 CCMP / 32 TKIP) cannot
complete authorization. M4 is sent only after a suitable GTK is extracted.
The M3 log states installed GTK byte count, and rejection no longer misleadingly
attributes every key-data error to a wrong passphrase/MIC.

The independent test vector was generated using Python cryptography AES key wrap
with zero KEK: 22-byte RSN IE + 40-byte TKIP GTK KDE + 30-byte IGTK KDE + 4-byte
padding. It has 96 plaintext / 104 wrapped bytes. The actual prior handshake
fails the GTK assertion while reporting authorization; corrected code installs
the exact 32-byte GTK. Valid-MIC corrupt wrap and valid-MIC missing GTK reject
without authorizing. Earlier mock AP success fixtures delivered no GTK; they
now explicitly supply a CCMP GTK instead of retaining that impossible success.

## Saved profile failure

The physical session also reports failed profile saving. The actual current
published live image, under BIOS USB snapshot with operator login, successfully
saves both direct `wifi save` and `sudo wifi save` synthetic profiles. This does
not establish the physical failure's cause. CLI errors now identify create,
chmod, write, fsync, close, rename or global sync stage and numeric return code,
without printing profile contents or credentials. Atomic replacement, private
permissions and checked durability behavior remain unchanged. Live /etc is
volatile; installed-root boot persistence still needs separate physical testing.

Evidence: `build/wifi-gtk-evidence/`, including the failing old regression,
ASan/UBSan suites, strict handshake tests, profile save/load tests and stack
gate. This is initial GTK delivery, not an implementation of later group-key
rotation. Physical cold ARP acceptance of the correction remains pending.
