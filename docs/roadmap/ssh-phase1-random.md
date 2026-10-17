# SSH Phase 1 — secure randomness

Date: 2026-10-17. Acceptance status is recorded below; physical entropy quality
is outside this automated gate. No crypto library, SSH exposure, installer,
network, DHCP, timekeeping or syscall-entry behavior is changed.

## Contract and construction

`SYS_GETRANDOM=77`: RDI writable user buffer, RSI length 1–256, RDX flags zero.
Available without capabilities, including the live operator account. Return
exact length or FortressOS negative errors: invalid length/flags `EINVAL` (-1),
then invalid/unmapped/readonly/kernel/overflow range `EFAULT` (-2), then source
unavailable/failed `EIO` (-9). Primitive reseed exhaustion is `EAGAIN` (-21).
The service reseeds every call, so exhaustion is not normally reachable there.
No partial output on error; no seed/readiness bypass flag. Pointer validation
uses `vmm_validate_user_range(...,true)` before touching the generator.

The DRBG follows HMAC_DRBG SHA-256, SP 800-90A rev.1 §10.1.2: K=00, V=01,
Update with entropy/nonce/personalization, HMAC generation, mandatory post-output
Update, reseed counter reset/increment. Maximum 256 output bytes per request;
the primitive enforces a conservative 65,536-request reseed interval. The
service uses fresh 32-byte hardware input before **every** Generate, stronger
than periodic-only reseeding. No invented hash-counter construction, TCP secret,
account seed adapter or Monocypher dependency. SHA-256 is the existing streaming
`root_digest.c`/`user/tools/digest.c` codec, not CRC32. HMAC contexts, pads, inner
digest and SHA-256 message schedules are erased using volatile writes.

Boot initializes after `time_init()` and before `sched_init()`; existing init
ordering is preserved. Instantiate input is 48 successful hardware bytes (32
entropy + 16 nonce) followed by a 32-byte SHA-256 personalization digest of a
domain string, TSC, RTC-derived epoch and the bounded kernel-owned memory map.
Only hardware is credited as secret entropy. Personalization does not rescue
hardware failure. All boot/reseed staging buffers are wiped on success/failure.

One ordinary Rank-1 lock owns kernel-only K/V and counters. No other lock,
allocation, logging, user copy, scheduler operation or I/O occurs inside it.
Requests from every CPU consume the same serialized state; spawn/exec/process
restart do not copy or reinitialize it. `random_init()` is boot-only and guards
against accidental repeat initialization. Kernel diagnostics snapshot byte,
successful reseed and hardware-failure counts under this lock (uint64 counters,
not quotas). No secret values are logged. Whole-VM snapshot/rollback resistance
is not claimed: clones need independent trusted hardware/hypervisor entropy.

## Hardware policy and limits

CPUID is checked on the executing CPU: RDRAND leaf 1 ECX bit 30; RDSEED leaf 7
EBX bit 18, with maximum-leaf checks. Prefer RDSEED when advertised; RDRAND is
accepted only on RDRAND-only machines. Advertised RDSEED exhausting its retry
budget fails the request rather than downgrading to RDRAND. Each word gets at
most 64 instruction attempts: 384 at boot, 256 per request. No timer-dependent
wait, sleep, readiness polling or 100 ms busy loop. These bound software retries,
not elapsed time when a CPU/hypervisor stops making progress or lock fairness.

All required words must succeed. Obvious zero/all-one/stuck adjacent words are
rejected; these conservative checks are not a complete entropy health test.
Failure latches unavailable until reboot, erases the old DRBG state, increments
the failure counter and returns EIO without caller output. Absence at boot
leaves OS boot usable with randomness unavailable. No weak fallback or output
from an old seed after a failed reseed. This intentionally supersedes the Phase 0
draft's continued-output proposal. RDRAND/RDSEED instruction success is a trust
assumption about CPU, firmware/microcode and hypervisor; it is not an independent
entropy assessment or NIST validation. Physical source quality, SRBDS mitigation
status and power-loss RAM/register erasure are not established by these tests.

Specifications: [SP 800-90A rev.1](https://csrc.nist.gov/pubs/sp/800/90/a/r1/final),
[Intel DRNG guide](https://www.intel.com/content/www/us/en/developer/articles/guide/intel-digital-random-number-generator-drng-software-implementation-guide.html),
[Intel SRBDS guidance](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/best-practices/srbds-mitigation-impact-intel-secure-key.html).

## Verification and evidence

`make test-random-host`: actual service and primitive at GCC -O1, ASan/UBSan,
pthread adapter for the actual lock API, mocked CPUID/instruction and user-range
adapters. NIST CAVP SHA-256 no-reseed COUNT=0 (two 128-byte Generates) is pinned
inline, as published in
[OpenSSL 3.0.15 evprand.txt](https://github.com/openssl/openssl/blob/openssl-3.0.15/test/recipes/30-test_evp_data/evprand.txt).
An independent Python hashlib/HMAC oracle checks instantiate/two Generates/reseed
against the compiled C. This does not import OpenSSL code into the kernel.

Additional host assertions cover all feature combinations, bounded 64-failure
stall, zero/all-one/repeated source words, partial collection failure with intact
caller canaries, latched failure and erased DRBG state, flags including high bits,
zero/257-byte lengths, pointer/error precedence, boot/reseed/staging erasure hooks,
reseed exhaustion/recovery and 1,024 distinct 32-byte outputs from eight pthreads.
Mocks do not establish real IRQ/rank/scheduler/hardware behavior.

`make test-getrandom` / `python3 scripts/test_getrandom.py`: retained snapshot ISO,
disk-free q35 TCG, paired OVMF with disposable vars, BIOS/UEFI × SMP=1/4/8 for
both hardware-present and absent, plus BIOS/UEFI SMP=4 RDRAND-only. No data disk
or physical device; final argv retained and checked. Operator Ring 3 calls check
256 success, 257/zero/flags errors, null/kernel/readonly/overflow pointers;
absent source checks EIO and unchanged bytes. Eight ordinary unpinned workers
generate concurrently into separate pipes; parent serializes terminal records.
Each success boot checks exactly 258 distinct nonzero 32-byte public samples;
the runner also rejects repeats across boots. CPUID APIC IDs adjacent to output
show ordinary worker placement on multiple CPUs, not an instrumented exact
syscall-entry CPU trace. Host threads separately prove concurrent serialized
generation. Shell recovery and privileged shutdown must complete.

Failed attempts are retained: first runner used unprivileged shutdown (denied);
second runner exposed terminal character interleaving between concurrent workers
and failed exact sample counts. Neither is relabeled a passing campaign.

No injected seed path exists in the release service. `RANDOM_HOST_TEST` adapters
and wipe observers exist only in the separate hosted test executable. The
freestanding primitive accepts deterministic input for vectors, but no user,
boot-command-line or fw_cfg injection path calls it. Probe logs contain public
samples only. Existing `accounts.c` and `net_tcp.c` source hashes are retained
unchanged, as are timekeeping and syscall-entry sources. This preservation plus
the existing credential-syscall/time host gates is the regression evidence;
no new physical installer or TCP interoperability acceptance is claimed.

Final results: **COMPLETE — host PASS; QEMU 14/14 PASS**. Successful campaign:
`.codex-remote-attachments/random-20261017-065922/summary.json`; each case retains
exact argv, serial log, QEMU stderr, result and disposable OVMF vars. Campaign
manifest records ISO/protected-source hashes and QEMU identity. There are 2,064
distinct samples across eight success boots. Every SMP=4/8 success boot observed
all configured APIC IDs (0–3/0–7). Six absent-source boots passed EIO/no-output
and shell recovery. Reseed-failure injection is host-only.

Commands actually run: `make -j8 iso`, `make -j8 all`, `make test-random-host`,
`make test-time-host`, `make test-perm-syscalls-host`,
`python3 scripts/test_getrandom.py`. No `test-crypto-host` exists yet.
Logs: `.codex-remote-attachments/random-host/host-final.log`, `host.log`
(time regression), `syscalls.log`, `qemu-final.log` and `build-final.log`.
The two prior failed campaigns remain at `random-20261017-065743` and
`random-20261017-065808`, with driver logs `qemu.log` and `qemu-retry.log`.
Final source/release identities and verification are retained in
`.codex-remote-attachments/random-host/release.json`. Release kernel `.text`
was compared byte-for-byte with the matrix-tested kernel after host-only
instrumentation and native-helper symbol renaming; no production instructions
changed. Later command-reference text is the only initramfs content update.
Final release smoke also passes BIOS/SMP=4 hardware-present (all four APIC IDs)
and UEFI/SMP=8 hardware-absent, retained under `random-host/release-smoke/`.
`make all` regenerates both ISO and IMG; raw-image GPT/ESP/ext2 audits pass.
