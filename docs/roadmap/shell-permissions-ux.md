# Shell permission feedback and startup, 2026-10-10

User requested a clean shell after login, visible user/admin context and clear
permission errors. Shell entry now clears the terminal and omits the developer
banner. Early kernel diagnostics remain intact; the login warning is printed
before authentication and cleared when entering the shell. Passwordless sudo
and credential policies remain unchanged.

Default prompt queries effective UID from the kernel, rather than trusting
USER/HOME: `[user:1000] fortress:<cwd> $ ` or
`[root/admin] fortress:<cwd> # `. Narrow screens retain a role indicator.
Root identity does not imply every dropped capability has been restored.

Permission denials now have explicit diagnostics for execution/redirection,
file inputs, chmod/chown, directory changes, mkdir/remove/rename, ls/view,
dmesg, job signals and power. Denied power calls return shell status 1.
Unknown errors retain generic diagnostics; no kernel authority was weakened.

Host shell/stream/credential/login/spawn regressions and actual prompt
ASan/UBSan checks pass (build/shell-ux). Initial host attempts retained:
expected old missing-file wording and an incorrect test reset argument were
corrected. Login 9 guest cases and sudo 10 guest cases print PASS, but both
campaigns end with their normal-image-unchanged assertion failing because
the new production image was copied into bin while those checks were running.
This orchestration mistake is not counted as a clean campaign PASS; logs and
isolated manifests remain retained. User requested image delivery for manual
Dell testing instead of further automated campaigns.

Fresh normal image independently verifies GPT/ESP/EXT4 and embedded ELF/tar
hashes, current runtime snapshot, locked root/passwordless operator and absent
credential test hooks. Exact hash and source manifest are in
build/shell-ux/delivery; prior bin outputs and Phase 5 accepted image remain
preserved. This new shell UX physical acceptance is pending. Existing Phase 5
physical evidence applies to its previous image, not automatically to this one.
Supplied Dell photo hashes are retained in build/shell-ux/physical-photo-index.json.


User subsequently reported the first role-labelled image Dell test PASS,
including sudo and nested shell exit. Requested prompt simplification and
sudo for all privileged commands. Default prompt now shows only `$ ` for
non-root/unknown identity and `# ` for effective root. Standalone mkdir/rm/mv,
shutdown/reboot/poweroff executables are packaged, so sudo invokes them directly
through its existing authenticated spawn path. Kernel permissions and no-cache
password policy remain intact; shell-state operations such as cd remain local
to whichever shell executes them.

BIOS/UEFI normal-login finite gate PASS 2/2: clean entry, user/root prompts,
denied ordinary shutdown and mkdir, sudo mkdir/rm, nested root identity, exit
return to operator, denied shadow read and direct sudo shutdown. Exact argv,
ISO hash, logs and manifests: build/shell-ux/ux-manifest.json. No data disks.
The simplified prompt host sanitizer gate passes. Fresh raw image and retained
prior image are in build/shell-ux/simple-delivery and prior-role-prompt-bin.
New image SHA-256:
78601315e4807592653292fabe0f34f1d8e61ebea57a81cb6ecfec8c778cc8c0.
Dell simplified-prompt/direct shutdown confirmation is pending.
