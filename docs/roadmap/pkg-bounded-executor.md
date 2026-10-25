# Package bounded executor checkpoint

2026-10-25. Private Gate 3 implementation, without production enablement.
[Contract and remaining integration](../plans/PKG2_EXECUTOR_PRIVATE.md).

The executor now performs actual EXT4/VFS/JBD2 install and removal from verified
plans, with journaled claims/reservations, owned directory checks, local durable
floors, staged executable evidence and fresh-owner recovery. The new private
CLI frontend dispatches copied package names through a trusted registry callback;
the default CLI and kernel dispatch remain unchanged.

## Exact validation

Command: `wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_install_fs.py`.
The runner records every GCC/native/e2fsck/debugfs command, compiler inputs and
hashes in its private campaign manifest. Host compilation uses C11, strict
warnings, ASan and UBSan. A separate freestanding x86-64 compilation records
private stack frames and enforces a 1024-byte maximum. All targets are fresh
regular files; no release build, installation or physical device is involved.

Four profiles cover one executable; 16 members including the exact 64-directory
bound; a 4 MiB executable plus 15 documents; and insufficient capacity with an
unrelated 24 MiB filler. Each successful profile exercises install/remove/
reinstall, verified no-op, open executable byte preservation, concurrent ordinary
log writes and CLI output failure without a second mutation callback.

Each of those three profiles tests 13 selected write/flush interruption offsets
for install and remove: 78 fresh-owner recoveries total, each followed by another
recovery with no package writes. These are sampled offsets, not an exhaustive
cut of every sector event. Four authority-loss phase boundaries per profile
test BUILD abort, COMMIT/RETIRE roll-forward and DELETE completion; authority
remains revoked for a further no-write request before fresh-owner recovery.
Callbacks model authority here; they do not prove real signal/reaper behavior.

Wrong local MAC keys, externally imposed rollback floors and modified ledgers
are rejected before writes. Independently seeded foreign entries in owned
documentation directories must be rejected before replay/writes. Low space
must reject before the reservation transaction. Linux e2fsck checks initial and
completed fixtures, and an independent Python oracle checks HMAC, generation,
floors, exact file hashes/metadata/identities and unchanged baseline account and
enrollment objects. Cut recovery is checked by the actual-code admission and
ownership validator; no independent Linux oracle per cut is claimed.

Regression command: `wsl -d Ubuntu-24.04 -- python3 scripts/test_pkg_cli_host.py`
PASS; default read-only CLI behavior and private ELF link/structure pass
(43,520 bytes, 89 mapped pages). Ring 3 was not run by that command.

## Retained failures and preservation

Final campaign:
`.codex-remote-attachments/pkg-install-20261025-090304-520545`.
Runtime **PASS**, boot acceptance **NOT RUN**, preservation **FAIL**.
The four concurrent changed inputs are `user/tools/live_password.h`,
`user/tools/login.c`, `scripts/test_sudo.py` and
`scripts/test_user_database_host.py`. Executor source hashes still match the
compiled copy. No concurrent files are included in this commit. Maximum measured
private frame: **1008 bytes**. Independent completed-output and foreign-fixture
Linux checks pass; all four profile results pass.

Native executable SHA-256:
`6a7b84e90aa994273402bcd6013dfd690460eae4dcecfc4de2890237f3484d98`.
The campaign `receipt.json` hashes retained logs, configurations, binaries,
images and oracle results; its SHA-256 is
`471a25519c3000f0508a3a6690002dc37616bb01f0b43b2149bf5fa3475db03b`.

Earlier campaigns remain under `.codex-remote-attachments/pkg-install-*`:

- `20261025-081831-733071` and `081958-542935`: overlay replacement assertions
  found multiple source matches; replacements were narrowed to exact contexts.
- `082324-869616`: strict compile rejected missing freestanding `strcpy` and
  misleading indentation; replaced with bounded-context copies and clarified
  statements, without disabling the warning.
- `082704-912000` and `083329-552943`: initial mount admission refusal, retained.
- `083712-164560`: diagnostic stdio inclusion preceded the POSIX feature macro;
  fixed include ordering rather than suppressing the warning.
- `084144-890174` and `084424-729943`: directory admission refused Windows-host
  fixture modes `0777`; the fixture now sets exact modes with debugfs.

An early temporary workspace was unavailable on inspection. Later campaigns
retain the complete copied workspace inside their evidence directory; no
reproducibility claim depends on that missing temporary copy.

Subsequent successful development campaigns retain preservation FAIL when
executor inputs were edited or concurrent unrelated account work changed during
execution. Their runtime PASS does not erase those failures. The final campaign
receipt records runtime and preservation separately. The original integration
plan retains SHA-256
`f1e8a769becaac55509a82a1633fda9c6057167588becab8c9cba91521fb1017`.

## Delivery boundary

No boot acceptance is claimed for this executor. Kernel retained-owner binding,
installed-root admission and a real Ring 3 mutation campaign remain. Native
account-service concurrency, every durable micro-transition/cut, fragmented-map
refusal campaigns, physical power-loss behavior and production local-MAC-key
custody remain unaccepted. Existing status/recover boot evidence cannot be
reused as mutation evidence. Production trust provisioning, release media,
public mutation enablement, pushes and physical writes remain disabled.
