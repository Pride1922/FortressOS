# GUI Wi-Fi association follow-up — 2026-10-28

The user reports GUI scanning works but association fails, while the terminal
`wifi connect` works on the same machine. The supplied photo shows the manager's
failed state, not an IPv4/DHCP error. The photo does not prove the radio failure's
precise cause. No real Dell success is claimed by the changes below.

## Reproduced source difference and fix

The CLI sends zero BSSID and chooses the current strongest cached AP by SSID.
The GUI pins the AP selected from a previous scan. In `wlan_mgr_connect` the
explicit-BSSID branch copied the chosen address but obtained channel/RSSI/beacon
interval using `wlan_bss_table_find(ssid)`, which can describe a different AP.
If signal ranking changes between scan and join, or the selected AP is not the
current strongest, the driver can authenticate the selected address on the
other AP's channel.

The manager now matches both BSSID and SSID when fetching metadata for a pinned
AP. Automatic/CLI best-signal selection is unchanged. The existing behavior for
explicit addresses absent from the scan cache is unchanged. No ownership,
locking, ABI, hardware workaround, handshake timeout or roaming rule changes.

The actual manager host test was extended with two same-SSID APs on channels 1
and 11. It failed before the fix on the selected address/channel/RSSI assertion,
then passed after the fix, including the exact `prepare_auth` channel, address
and interval. Existing crypto, handshake, replay, timeout and roaming cases pass.

## Firmware preparation parity

The desktop previously omitted the firmware preparation performed by the CLI.
It now spawns the existing `/bin/wifi init` command before its first scan,
redirecting standard descriptors to `/dev/null`. The event thread polls
`SYS_WAITPID` with WNOHANG and reaps the exact child; no synchronous firmware
wait blocks rendering. Join/duplicate preparation actions are suppressed until
completion. Closing the Network window clears credentials as before; a running
credential-free preparation child is still reaped, without starting a scan in
a closed window. Spawn/preparation failure leaves the state retryable and shows
an explicit preparation error, rather than blaming the Wi-Fi password.

No SSID/passphrase enters child argv, logs or saved profiles. The CLI's bounded
gzip decoder and existing owner-safe LOAD_FW are reused. `wifi init` now retries
preparation for FAILED/SCANNING status: those states do not establish firmware
readiness. Connected/online status still reuses the working runtime, and the
driver's existing idempotent loader avoids restarting an already live image.
No new firmware loader or driver recovery mechanism is introduced.

This targets radio association. Persistent profiles and static/DHCP policy are
still the existing CLI/daemon's responsibility, as before this follow-up.

## Verification and artifact

- `make test-wifi-host`: actual manager/MAC/crypto/driver-parser ASan/UBSan
  suite PASS, including the before/after selected-AP regression.
- `make test-wifi-fault-host test-wifi-stack`: PASS; connection retries,
  fail-closed timeout, teardown and stack contracts preserved.
- `make test-wifi-prepare-host`: actual CLI preparation with mocked syscall
  and decoder adapters PASS. FAILED/SCANNING load instead of skipping,
  connected reuse, decompression failure and descriptor cleanup are checked.
- `make test-compositor-host`: **34/34 PASS**, including join suppression
  during preparation, retained bounded password input, failure/retry and
  preparation completion, plus the existing usability tests.
- `make test-wifi-owner-host`: ASan/UBSan and TSan PASS, 4,800 concurrent calls.
  Evidence: `build/wifi-owner/host-tti91sa5/manifest.json`.
- `python3 scripts/test_wifi_owner.py --cpus 1 4`: **4/4 PASS**, BIOS/UEFI ×
  SMP=1/4, actual Ring 3 BSP/AP calls with explicit mock radio.
  Evidence: `build/wifi-owner/qemu-47a95kx7/manifest.json`.
- `python3 build/gui-wifi-connect/prepare_qemu.py`: **4/4 PASS**, BIOS/UEFI ×
  SMP=1/4. A private copied compositor entry branch invokes the actual Rust
  prepare/spawn/WNOHANG-wait helpers, actual CLI gzip/firmware path and CPU-0
  mock radio. It checks successful child exit and reaping. This observer branch
  exists only in the copied workspace, not ordinary media.
  Evidence: `build/gui-wifi-connect/prepare-qemu/manifest.json` and logs.
- Ordinary desktop GUI smoke uses `test_desktop_apps.main()` with the normal
  private snapshot: **4/4 BIOS/UEFI × SMP=1/4 PASS**. It exercises the no-radio
  GUI, settings, persistence and timezone rather than claiming radio association.
- Ordinary strict kernel/initramfs/ISO/image and probe exclusion PASS:
  `build/network-smp-phase3/release-ouqw772s/manifest.json`. Shared release
  media are unchanged. New Dell candidate: the adjacent `fortress.img`.
- Compositor disassembly: largest fixed stack subtraction **920 bytes**,
  no SIMD register references. Evidence: `build/gui-wifi-connect/compositor-stack.json`.

The ordinary snapshot is
`.codex-remote-attachments/ext4-phase9/guest-workspace-vdqp6rg7`; its GUI logs
are in `build/desktop-apps-*.log`. The test-only preparation helper uses the
separate mock-radio snapshot `guest-workspace-ercy9z6k`.
That mock snapshot's initramfs/ISO were rebuilt for the later private Rust
helper test; the earlier owner-suite hashes identify its pre-helper media.
The owner-suite serial evidence and unchanged kernel remain retained; the
ordinary candidate image is separate and retains its verified media hashes.

All 51 pre-existing modified/untracked files were archived before editing in
`build/gui-wifi-connect-preservation/before.zip`. Existing desktop usability,
CPU-0 owner and TCP listener work are preserved. No commit, push or device flash.
Physical GUI association on the user's Dell remains the required confirmation.
