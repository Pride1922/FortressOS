# Parent history before nested sudo shell (2026-10-17)

The installed user's `sudo /bin/shell` entry was pending in the parent shell
while the root shell ran. The five-command autosave runs after execution;
rebooting inside the child never returns to that parent. Root history saved
correctly, but the parent's launcher entry could disappear.

`execute_parse_tree` now flushes pending interactive history before a command
list containing an external command runs, before temporary environment scopes
can change HOME. Builtin-only lists keep the existing five-command interval.
The helper skips clean and non-terminal state, retains dirty history on failure,
and uses the existing once-per-session warning. Root/user files remain separate.

## Verification

- `wsl -d Ubuntu-24.04 -- python3 scripts/test_shell_host.py`: PASS, actual
  shell/editor code under ASan/UBSan; new pre-child flush tests cover immediate
  save, clean/script skips, and failure retry with mocked history-save I/O.
- Focused installed-root QEMU test: login operator, `sudo /bin/shell`, `ps`,
  `reboot`; login operator again and assert the history listing contains the
  launcher entry, then reenter sudo shell and assert `ps` and `reboot` survive.
  Original shell FAIL: operator history contains only the newly typed `history`.
  Patched shell PASS in BIOS and UEFI, SMP=1, four boots total.
- Focused commands: `python3 build/check_sudo_history.py bios` (original FAIL),
  `python3 build/patch_sudo_history.py`, then
  `python3 build/check_sudo_history.py bios build/sudo-history-fixture-fixed.json`
  and the same command with `uefi` (both PASS). These local diagnostic helpers
  and fixture state are retained in ignored build output, not release media.
- Evidence: `build/root-phase2/gate-_jk_j8uy/`, including original
  `sudo-bios-1.log` failure and patched `sudo-bios-fixed-{0,1}.log` /
  `sudo-uefi-fixed-{0,1}.log`, exact QEMU argv JSON, and disposable disk copies.
  The original failure preceded firmware-qualified log naming.
- Durable installed regression added to `scripts/test_root_phase3.py`: while
  the parent is still waiting, root reads the operator history file and asserts
  the launcher entry is already present. The full Phase 3 suite was not rerun.
- `wsl -d Ubuntu-24.04 -- make -j4`: PASS, ISO/raw image generation and image
  verification. No physical disk writes or physical acceptance claimed.

Guest fixtures use copied regular NVMe image files, no physical device paths,
and separate copied OVMF variables. The focused test used the existing isolated
Phase 2 installed fixture preparation and replaced only `/bin/shell` in a
disposable template copy with the current strict freestanding build. Its helper
asserts the copied image location and always reaps QEMU. A separate unrelated
`test-perm-db-host` fixture failure (missing temporary `sbin` directory) occurred
during initial preparation; the focused preparation omitted that host suite.

## Sudo shortcuts follow-up

The bare-sudo default in this historical step is superseded by the explicit
`sudo -s` policy below.

Bare `sudo` now supplies `/bin/shell` as its default command before following
the unchanged wheel/password/credential-transition path. The shell recognizes
only a whole unquoted `sudo !!` input line (with optional spaces/tabs), before
adding that line to history. It prefixes the previous history entry with sudo,
displays the expanded text, and records the expansion. Quoted/escaped tokens
remain literal; missing history and capacity overflow return diagnostics without
execution. This is a bounded shorthand, not general history expansion.

`python3 scripts/test_shell_host.py` passes with actual history-expansion code
under ASan/UBSan, including literal forms, no previous entry, and exact output
capacity. `make -j4` passes and rebuilds/verifies ISO and raw image.

The focused copied installed fixtures now replace both `/bin/shell` and
`/bin/sudo` (retaining root ownership and 04755 on sudo). The same BIOS/UEFI
commands above pass with the extended local runner: ordinary `id` reports
uid=1000, `sudo !!` prompts for the real user's password and reports uid=0,
bare `sudo` prompts and opens a root shell with HOME=/root, then reboot and a
second login prove `sudo id` and `sudo` remain in user history while root's
`ps`/`reboot` remain separate. Four boots PASS; logs/argv use
`sudo-{bios,uefi}-shortcuts-{0,1}` names in the same evidence directory.
The non-wheel bare-sudo regression in `scripts/test_sudo.py` and the installed
shortcut checks in `scripts/test_root_phase3.py` were added and syntax checked;
their full suites were not rerun. No physical acceptance is claimed.

## Explicit root-shell policy (current)

At the user's request, bare `sudo` now prints usage; `sudo -s` explicitly
launches `/bin/shell`. `sudo command` and interactive `sudo !!` retain their
behavior. This is a bounded interface: unsupported options and `sudo -s` with
extra arguments print usage. Wheel authorization, caller-password verification,
locked direct root login and no authentication cache remain unchanged.

The same focused BIOS/UEFI runner commands above pass after copying the current
shell and sudo ELFs into a fresh disposable `sudo-policy-fixed-template.img`.
On each boot it verifies bare sudo/unsupported options print usage, an incorrect
password for `sudo -s` is rejected, ordinary `id` still reports uid=1000,
`sudo !!` runs `id` as uid=0 after authentication, and authenticated `sudo -s`
opens the root shell. The second boot verifies the user's `sudo -s` and
expanded `sudo id` entries and root's separate `ps`/`reboot` history.
Four boots PASS (BIOS/UEFI SMP=1); retained logs/argv use
`sudo-{bios,uefi}-policy-{0,1}` in the evidence directory above.

`python3 scripts/test_shell_host.py`, Python syntax checks for the modified
guest suites, `git diff --check` and `make -j4` pass. Durable installed and
non-wheel/wrong-password regression cases now target `sudo -s`; the full
Phase 3 and sudo acceptance suites were not rerun. No physical claim.
