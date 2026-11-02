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

The physical session also reports failed profile saving. A BIOS USB snapshot
initially saved direct/operator and sudo profiles successfully. The release UEFI
USB test then reproduced a failure precisely at global SYS_SYNC (-11), after
successful private write and atomic replacement. That proves this software
failure mode, not that the physical laptop necessarily returned the same errno.
The previous command flushed every mounted filesystem, allowing unrelated disk
errors to report a profile write failure.

The CLI now fsyncs the retained `/etc/network` directory after rename instead of
calling global sync. The existing EXT4 fsync implementation explicitly accepts
directory descriptors and commits that mount; installed-root durability is not
skipped. Volatile ETCFS reports EOPNOTSUPP for both file and directory fsync.
Other file/directory flush errors and close errors remain failures. No unrelated
mount flush is requested. CLI errors identify the failing stage and numeric code
without credentials. Host tests assert no SYS_SYNC call and cover both supported
and unsupported scoped fsync. Live /etc remains volatile; installed-root reboot
persistence still needs separate physical testing.

Evidence: `build/wifi-gtk-evidence/`, including the failing old regression,
ASan/UBSan suites, strict handshake tests, profile save/load tests and stack
gate. This is initial GTK delivery, not an implementation of later group-key
rotation. Physical cold ARP acceptance of the correction remains pending.

## Published candidate and test results

`bin/fortress-live.img`, 130 MiB, clean production source `d6c9984`, SHA-256
`c3b90563ad2d7f86ce2aa935814f2998c8ab1423eb2496701fe75a6da0d9787f`. The isolated checkout is
`.codex-remote-attachments/wifi-gtk-release`. This includes the previous desktop
changes and honest unsupported-battery handling; it is live-only media.

Host ASan/UBSan handshake/crypto/manager and fault suites PASS, plus profile
save/load/boot-auto tests, no-global-sync/volatile-fsync tests and concurrent
owner ASan/UBSan + separate TSan (4800 calls each). Stack gate PASS: 230 functions,
576-byte unwrap helper, 1072-byte EAPOL function within its existing 1280-byte
exception, all other functions within the default 1024-byte limit.

Actual raw live USB BIOS/UEFI SMP=4 PASS 2/2: login, unsupported battery CLI,
all RX diagnostic rows, desktop version, successful operator and sudo profile
saves, precise error on exclusive-temp collision, and retained profile contents.
Only one regular-file USB snapshot per boot; no installation target or physical
I/O; backing checksum unchanged. GPT/FAT/ext4 audit and embedded kernel/initramfs
byte identity pass. Exact argv, logs, before/after failures and receipts are in
`build/wifi-gtk-evidence/`. The initial UEFI save failure is retained under
`before-scoped-sync-smoke/`. No mock-radio result establishes Dell cold ARP.

On the laptop, boot this replacement USB and connect once. Expect `GTK bytes=32`
for the observed TKIP group cipher, then read `sudo dmesg -n 4` before initiating
any FortressOS-to-Bazzite ping. Repeat the measured Bazzite ARP burst and check
reply reception and successful group decrypt deltas. Physical acceptance and
installed-root saved-network reboot persistence remain open until tested.

## Physical live-USB acceptance: initial group RX and inbound ARP PASS

2026-11-02: the user booted the replacement live USB and connected to the same
AP at 192.168.0.194/24. The photographed snapshot now shows GTK length 32/index 1,
16 successful group decrypts and zero group decrypt failures. Previously the
same driver diagnostic reported GTK length zero and every group decrypt failing.

From Bazzite 192.168.0.159, with the laptop asked to remain idle:

- `arping -b -I wlo1 -c 10 -w 12 192.168.0.194`: ten broadcast probes and ten
  replies from f4:d1:08:c9:6b:e3, exit zero. Explicit broadcast ARP response is
  observed independently of any existing Bazzite neighbor entry.
- `ping -c 4 -W 1 192.168.0.194`: 4/4 replies, zero loss, TTL 64,
  min/avg/max/mdev 8.245/10.412/13.672/2.085 ms, exit zero.

No FortressOS-originated ping was requested to enable the reverse direction,
and no static neighbor was added during this test. No neighbor deletion was
performed; the broadcast-only arping result directly establishes responsive ARP,
while the ping result separately establishes inbound unicast reachability.

This is a bounded physical PASS for initial GTK installation, group reception
and inbound ARP/ICMP with the current live image. It does not prove the actual
AP M3 packet length, installed-root save durability/autoconnect, reboot/idle
recovery or later group-key rotation. Those boundaries remain open. Tool outputs
and photo counters are transcribed with explicit provenance in
`build/wifi-gtk-evidence/physical/acceptance.json` (not a packet capture).
