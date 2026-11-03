# Foreground DHCP and service acquisition contention — 2026-11-03

## Physical observation and reproduced defect

On the corrected GTK live USB, a reboot/manual Wi-Fi connection authenticated,
installed its 32-byte GTK and saved the profile, but its automatic DHCP child
reported an offer/ACK timeout. A manual `dhcpc` retry obtained 192.168.0.194/24.
The service photo then showed one stable root-owned `dhcpc -f` process, no
restarts, and a valid 7200-second lease. ARP and inbound ICMP remained working.
The original child's bind result was not recorded, so this is not proof that
the physical first attempt specifically lost a bind race.

Code inspection found the Wi-Fi CLI and running daemon can both start DHCP
acquisition. Kernel UDP port binding already excludes simultaneous ownership,
but the foreground process converted *any* bind error into the same generic
protocol timeout. The actual foreground-entry host regression reproduces a
busy port returning failure immediately, without sending a DHCP packet, while
an existing owner is still acquiring the matching lease.

## Correction and boundaries

A busy bind (EEXIST from the existing UDP API) now has a distinct internal
result. A normal foreground acquisition can observe an existing daemon for at
most ten seconds / 200 sleeping turns. It never sends another packet, signals
or replaces that daemon, or rewrites its configuration. Success requires the
original MAC still selected and online, a nonzero configured IP, matching
IP/netmask and a nonzero server in a complete newline-terminated lease file,
and an unexpired lease according to the same clock used by the writer.

Other bind failures do not enter that wait. Owner death, interface change,
link loss, expired/mismatched/incomplete lease files, backward/stalled clock
and timeout cannot turn into success. Temporary files and configuration are
not used as locks. The socket that lost binding is closed before waiting.
If no daemon exists, the busy error remains explicit. The existing free-port
standalone acquisition path is retained. Boot fast-probe and dry-run modes do
not adopt a daemon lease; renewal/rebind/release are unchanged.

Error output now separates local busy/bind failures, actual OFFER/ACK timeout,
and other acquisition failures. The kernel CPU-0 owner, UDP binding policy,
public ABI, boot ordering, synchronization and packet retry timers are unchanged.
This fixes a deterministic software failure mode; real packet-loss/AP timing
failures can still produce an actual timeout and need separate evidence.

## Verification

`make test-dhcpc-contention-host` compiles the actual `dhcpc_main` with mocked
syscalls under ASan/UBSan. It checks delayed owner success without packet sends
or configuration writes, expired/wrong-IP/incomplete leases, MAC change, link
loss, owner death, frozen clock, missing PID and non-contention bind errors.
Every attempted socket closes. The prior entry fails the success assertion and
prints the generic timeout; the corrected entry passes. The test is included
in `make test-dhcpc-host` alongside the existing v1/v2 codec suites.

`make test-dhcpc-contention` creates a private source snapshot and inserts a
three-second hold immediately after a successful real UDP bind. The marker and
delay exist only in the copied userspace source, never the release binary.
BIOS/UEFI x SMP=1/4 tests stop the service, restart it into the held acquisition,
then run the ordinary foreground client. They require managed success with the
same daemon PID, real DHCP/configuration/status and gateway ping, followed by
standalone free-port acquisition after stopping the owner. Only a read-only ISO,
user-mode e1000e networking and paired OVMF files are attached; no data disks or
physical storage. QEMU results and published image identity follow after gates.

Evidence: `build/dhcp-contention-evidence/`. Parallel UI/font work is excluded
from the release source checkpoint and left untouched. A physical reboot and
Wi-Fi connection without a manual DHCP retry is still the final acceptance gate.
