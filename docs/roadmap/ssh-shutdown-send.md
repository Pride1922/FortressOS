# SSH shutdown send interruption — 2026-10-18

## Failure and correction

The Watchtower lifecycle regression reproduced OpenSSH `Bad packet length` /
`Packet corrupt` during remote `sudo shutdown`, including against the unchanged
release ISO. Service stop/restart and reconnect had passed before that failure.
The original observation and baseline hashes are retained in
[the credential report](watchtower-credentials.md#broader-lifecycle-regression-not-a-full-pass).

The worker encrypted a channel packet, thereby advancing its outgoing cipher
sequence, then treated `SYS_SEND` returning `EINTR` as a terminal send error.
Cleanup still saw an active transport and encrypted a disconnect with the next
sequence. The client was missing some or all of the preceding frame and could
not decode that disconnect. Short writes followed by interruption had the same
problem; encryption cannot be rolled back safely after a prefix is emitted.

The worker now finishes the already-encoded bytes across short sends and up to
16 interrupted-send retries, even after its termination flag is set. It does
not re-encrypt the packet or decrement/reuse its cipher sequence. The same
sender handles the disconnect packet. A zero-progress, permanent, oversized
result or exhausted interruption retry marks the transport failed; cleanup
closes that stream without appending another encrypted packet. Channel encoding
failures also invalidate the transport, since encoding can consume a sequence.

The existing monitor's five-second worker teardown and Watchtower escalation
remain the outer bounds. This does not promise delivery to a dead or permanently
blocked peer: those streams close, rather than receiving an invalid encrypted
tail. No kernel networking, crypto algorithm, key material, physical-write gate
or client setting was changed.

## Verification

`make test-ssh-transport-host` passes ASan/UBSan transport, actual worker adapter,
and live OpenSSH handshake-oracle checks. The actual worker code is included in
the adapter; only socket I/O is mocked. New deterministic interruption cuts at
0, 1, 4, 137, 4095, 4096 and 4120 bytes of a 4096-byte channel-data packet verify
that the receiver authenticates/decodes the complete DATA followed by DISCONNECT
with `System going down`. Sixteen interruptions succeed; seventeen fail closed
without adding a disconnect. Existing full/short writes, zero progress and
permanent socket-failure checks remain covered.

`make test-ssh-shutdown` builds a disposable ISO with the rebuilt kernel,
Watchtower and SSH daemon, then runs the existing real lifecycle suite using
`scripts/test_watchtower_lifecycle.py --staged`. It uses the existing release
archive as a read-only base and preserves the release ISO, USB image and archive.
No fixture data disks or physical devices are attached. Runtime SSH keys and
password state live only in guest RAM; no secret-bearing guest disk or console
transcript is retained. Acceptance probes/configs remain outside release staging.

The staged runner exercises BIOS/UEFI × SMP=1/4: service stop/start, restart/PID
changes, crash recovery, retry exhaustion/reset, stop-timeout escalation,
authenticated SSH stop/restart with disconnect and reconnect, then remote
shutdown (SMP=1) or reboot/new login (SMP=4). No new physical Dell acceptance is
claimed.

The first staged run passed all runtime checks in all four combinations,
including clean RFC disconnect plus actual poweroff/reboot. Its final shared
release-input preservation assertion failed: the shared ISO/image/archive were
rewritten during the campaign, and an unrelated `scripts/create_boot_img.py`
edit appeared concurrently. This test only writes its own temporary ISO and
firmware variables; it did not invoke the release image builder. The external
edit/artifacts were left untouched. That failed preservation result is retained
separately from the 4/4 runtime passes.

A second complete runtime campaign also passed all four combinations (8/8
successful guest runs across the two campaigns):

| Firmware | CPUs | Final action | Runtime result, both campaigns |
| --- | --- | --- | --- |
| BIOS | 1 | Remote shutdown | Clean SSH disconnect and QEMU poweroff |
| BIOS | 4 | Remote reboot | Clean SSH disconnect and new boot/login |
| UEFI | 1 | Remote shutdown | Clean SSH disconnect and QEMU poweroff |
| UEFI | 4 | Remote reboot | Clean SSH disconnect and new boot/login |

The second campaign again exited nonzero only at the final shared-input
preservation guard: all three release artifacts changed during the run. An
additional unrelated `scripts/test_usb_persistence.py` edit also appeared.
The guard was retained, not bypassed, and no third attempt was made while
concurrent image work continued. A complete `make test-ssh-shutdown` PASS is
therefore **not** claimed: the SSH runtime checks pass, while shared-file
preservation is unverified under that concurrent work.

Second campaign SHA-256 changes (full values retained for reproducibility):

| Shared artifact | Before | After |
| --- | --- | --- |
| `bin/fortress.img` | `d31a0e658ad54eb84a4997e6f02b02a55cd5d8eb5a41ce7cda3d2ce6e17ce77b` | `621506d997d2f655ab1de550a8fd2570591cbdea4f8179f00bddaa8a85e3e6b8` |
| `bin/fortress.iso` | `57f600edd3099fff88f2181b4f9559f8ef3859a5549fcabacc862c6c79b573cb` | `d6617e3fe8c62efe1d241cd60ea497ef23051970c190a20e7365f00db35703cf` |
| `bin/initramfs.tar` | `15faffc1e4abec349c6b23373c3d5aec582fec4960759928646e7e81cd5d2a1c` | `a86c638bcc5447f835e957316ca7b2fe504ac92a3005a9ae9c8736212877a37a` |

`make test-ssh-daemon-host` also passes its configuration, readiness, admission
and bounded-teardown ASan/UBSan checks. The modified worker compiles with a
464-byte main worker frame and a 64-byte send-helper frame; no large automatic
packet buffer was introduced.
