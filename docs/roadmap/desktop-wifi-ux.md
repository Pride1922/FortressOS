# Desktop Wi-Fi usability - 2026-10-28

App-only follow-up to the CPU-0 ownership fix. No kernel or driver behavior
is changed by this follow-up. The user's existing desktop, Wi-Fi ownership and
listener changes remain in the working tree.

Connection follow-up: the user subsequently reported working terminal Wi-Fi
but failed GUI association. [The connection audit](gui-wifi-connect.md) fixes
an exact-BSSID metadata mismatch in the manager and adds the CLI's firmware
preparation before GUI scan/join. That follow-up changes the manager and CLI
as documented separately; the usability work below remains preserved.

- Automatic scan on opening Network, with Refresh and scanning feedback.
- Clickable four-row pages, strongest-first ordering and one row per
  SSID/security pair, retaining the strongest BSSID. Signal quality and a
  connected marker replace the one-access-point Previous/Next selector.
- Selection focuses the password; Enter joins. Show/Hide, local name/password
  validation, password clearing on network changes and keyboard focus scrolling.
- Connecting/authentication progress, success, failure/retry and bounded
  monotonic timeout feedback; repeated join requests are suppressed and Cancel
  uses the existing disconnect request.
- No saved credentials or new profile policy. Open networks are explicitly
  unsupported because the existing connection service requires WPA2 credentials.

Verification: `make test-compositor-host` 33/33 PASS, including mocked
network selection, deduplication/BSSID retention, password visibility/reset,
validation and connection outcome/timeout handling. GUI firmware coverage uses
`python3 scripts/test_desktop_apps.py`: BIOS/UEFI x SMP=1/4, 4/4 PASS,
ISO-only private guests and QEMU's no-radio state;
it cannot establish real scan/password/association behavior. The user reported
working Dell app Wi-Fi before this usability change; the new flow needs a Dell
retest.

Retained development finding: the first release disassembly audit found a
2,376-byte generic small-sort frame. Replaced generic sorting with bounded
insertion sorting; the following audit reports a largest fixed frame of
968 bytes and no SIMD register references. No stack-budget exception was added.
