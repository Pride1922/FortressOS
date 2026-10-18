# PuTTY authentication handoff fix

2026-10-17: MobaXterm 25.3 build 5384 graphical SSH stalled before a
password prompt, while PowerShell and MobaXterm's terminal OpenSSH worked.
An independent Windows Plink 0.85 connection reproduced the stall: host-key
verification and encryption succeeded, then the client sent
`SSH_MSG_SERVICE_REQUEST` without receiving a reply. No password was sent
in that diagnostic.

The worker broke out of its KEX packet-drain loop at `NEWKEYS`, then performed
a blocking socket read before inspecting the remaining transport buffer.
When NEWKEYS and the encrypted service request arrived together, the service
request remained buffered while both peers waited. Authentication now extracts
buffered packets first and reads only when the framer returns `SSH_ERR_AGAIN`.
Socket EOF/errors and framing errors terminate that receive path.

## Verification

- `wsl -d Ubuntu-24.04 -- python3 scripts/test_ssh_transport_host.py`: PASS.
  ASan/UBSan tests include the actual worker receive helper with a mocked
  socket, real encryption/framing and real service-request parser. All splits
  between NEWKEYS and the encrypted service request pass; a fully buffered
  request produces SERVICE_ACCEPT without another socket read. Socket EOF,
  socket errors and buffered malformed framing are covered.
- `wsl -d Ubuntu-24.04 -- make -j4 bin/fortress.iso`: PASS.
- `wsl -d Ubuntu-24.04 -- make -j4`: PASS; rebuilt `bin/fortress.img`
  and verified its partition tables, EFI contents and ext4 filesystem.
- `PLINK=/mnt/c/Sources/FortressOS/build/putty-interop/linux/usr/bin/plink
  python3 -u scripts/test_ssh.py` in WSL Ubuntu-24.04: PASS on BIOS and UEFI,
  SMP=1. Ubuntu's PuTTY Plink 0.81 authenticates with a disposable fixture
  password and runs `echo putty_auth_success`, using the fixture's pinned host
  fingerprint. Existing OpenSSH host-key, public-key/password authentication,
  root rejection, interactive PTY, concurrency-cap and teardown gates pass.
  The final-source rerun output is in ignored
  `build/putty-interop/qemu-acceptance.log` (no private keys or disks).

`scripts/test_ssh.py` runs the PuTTY gate when `PLINK` identifies an executable
or `plink` is on PATH; otherwise it reports an explicit skip. Its password
file, generated private keys and disposable NVMe/ISO fixtures are deleted by
the runner, including on failure. Client binaries are ignored build tools,
not release contents.

## Physical follow-up (2026-10-18)

The user flashed the updated live USB and confirmed that MobaXterm 25.3's
graphical SSH session now connects on the laptop. With an explicitly configured
temporary live password, `whoami` returns `operator`, `top` displays live process
information, and sudo works. This is user-reported physical acceptance of those
specific interactions, not a complete physical regression or lifecycle audit.
The dmesg screenshot shows output and the paste review prompt; it does not prove
every dmesg scenario completed. No password or private key is retained here.

The live setup commands and RAM-only identity/password boundaries are documented
in [the SSH subsystem guide](../subsystems/ssh.md). Disk-free BIOS/UEFI acceptance
in `scripts/test_live_password.py` verified hidden password input, sudo reload,
runtime host-key provisioning, OpenSSH login and wrong-password rejection before
the later password-minimum adjustment. Windows Plink 0.85 acceptance logs are in
ignored `build/putty-interop/windows-putty85-bios.log` and
`build/putty-interop/windows-putty85-uefi.log`.

Two subsequent usability changes remain untested on hardware and absent from the
last USB image: an eight-character minimum for account-tool passwords and
one-Enter submission of single-line bracketed pastes. The password binary was
compiled; the updated eight-character acceptance case has not been run. At the
user's request, no additional tests or image rebuild were performed for these
follow-ups. Multiline paste review remains required.

These observations do not establish physical entropy quality, a new idle or
pre-authentication timeout, complete daemon-lifecycle acceptance, or installed
host-key rotation/recovery acceptance. Physical-write gates remain unchanged.
