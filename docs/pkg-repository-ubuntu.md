# Ubuntu repository publication

The host publisher is `scripts/pkg_repository.py`. It produces the existing
PKG-1 signed catalog and immutable content-addressed FPK objects. The download
client verifies these with ordinary root-owned repository public keys. Device
enrollment, separate key USBs and laptop identities are not part of this workflow.
Installation and repeated upgrade integration are still in progress; these
instructions do not claim a completed Dell update.

## Public layout

Serve the publication directory as an HTTP document root:

```
catalog.v1
objects/sha256/<64-lowercase-hex-full-artifact-sha256>.fpk
```

Catalog bytes are specified in [PKG1_CATALOG_DESIGN.md](plans/PKG1_CATALOG_DESIGN.md).
Each entry names a package/build, exact artifact byte length and SHA-256.
Catalogs use threshold Ed25519 verification against independently provisioned
repository UUID, root version and catalog-role public keys. HTTP/TLS identity
does not replace this signature authority. The publisher itself checks envelope
hashes and manifest identity, not the complete payload/ELF policy; target-side
full catalog/artifact verification remains mandatory.

## Signing host

Use an offline or otherwise separately protected Ubuntu signing host with
Python 3 and `python3-cryptography`. The HTTP server receives only public output.
Existing approved raw Ed25519 seeds must be external 32-byte regular files,
mode 0600 or stricter, with one hard link, outside both the source checkout and
publication tree. The tool never generates a key. Selecting production keys,
thresholds, platform identity and trust provisioning requires a separate explicit
operational action; no such action has been performed here.

## Build and inspect a standalone tool

Run these commands from the FortressOS checkout on Ubuntu 24.04 (Python 3.11+
with `python3-cryptography`, GCC/binutils and NASM installed). This example builds
an actual freestanding stream filter, `rot13`, rather than a synthetic ELF.
Release 1.0 rotates ASCII letters from stdin to stdout; release 2.0 adds `--help`.
Both support `--version`. The example is separate from normal initramfs contents.

```bash
sh examples/pkg/rot13/build.sh
# Set these to approved release compatibility values, not device identities.
export PLATFORM_ID=APPROVED_64_HEX_PLATFORM_ID
export BASE_MIN=1 BASE_MAX=1
python3 - <<'PY'
import json, os
from pathlib import Path
root = Path.cwd()
for release in (1, 2):
    source = root / f"examples/pkg/rot13/v{release}.json"
    spec = json.loads(source.read_text())
    spec.update(platform_id=os.environ["PLATFORM_ID"],
                base_min=int(os.environ["BASE_MIN"]), base_max=int(os.environ["BASE_MAX"]))
    spec["executable"] = str(root / f"build/pkg-rot13/rot13-v{release}.elf")
    spec["files"][0]["source"] = str(root / "examples/pkg/rot13/README.txt")
    (root / f"build/pkg-rot13/release-{release}.json").write_text(json.dumps(spec))
PY
python3 scripts/pkg_build.py build build/pkg-rot13/release-1.json --out build/pkg-rot13/release-1.fpk
python3 scripts/pkg_build.py inspect build/pkg-rot13/release-1.fpk
python3 scripts/pkg_build.py build build/pkg-rot13/release-2.json --out build/pkg-rot13/release-2.fpk
python3 scripts/pkg_build.py inspect build/pkg-rot13/release-2.fpk
```

The checked-in `v1.json`/`v2.json` use an explicitly illustrative `42` repeated
platform ID and base range 1..1 for host verification. Use the approved values for
a real release. Build counters must increase for every changed artifact, including
metadata changes; the retained publisher refuses rollback and equal-build changes.
An identical artifact/build can be republished in a newer catalog.

The JSON schema has required `name`, `command`, `version`, `build`, `platform_id`,
`base_min`, `base_max`, `executable`, plus optional `description` and `files`.
Each file is exactly `{"source": "relative/local/file", "path": "doc/name"}`.
Local paths resolve relative to the JSON, regardless of the working directory.
Absolute source paths also work. Unknown/duplicate fields are rejected.
FPK v1 permits one static ELF64 x86_64 executable at `bin/<command>` (0755,
maximum 4 MiB), and at most 15 nonempty data/documentation files under `doc/`
(0644). It does not support arbitrary install paths or multiple executables.
Links (including hard links), special files and set-ID source files are rejected.
Payloads use deterministic gzip/USTAR, bounded manifest/archive sizes and hashes.
Build and inspect compile a temporary host adapter around the existing C validators
for complete manifest, payload and ELF validation. Inspection prints identity,
version, build, file count and artifact SHA-256; it does not establish catalog trust.
Output creation refuses an existing file. Keep each release's JSON and artifacts.

The existing `build_desktop_package.py` CLI and Python `build()` API remain wrappers
around the same encoder, with the original desktop file set and metadata.

## Sign and retain the existing offer

Use the retained local publication tree as the source of truth. If it lives on
another machine, securely copy its current catalog and objects to the signing
host before publishing. Replace the placeholders below with approved release
policy and existing external seeds; these commands never create keys.

```bash
issued=$(date -u +%s)
expires=$((issued + 7 * 24 * 60 * 60))
python3 scripts/pkg_repository.py \
  --repository APPROVED-REPOSITORY-UUID \
  --root-version APPROVED-ROOT-VERSION \
  --revision NEXT-CATALOG-REVISION \
  --issued "$issued" --expires "$expires" \
  --key /protected/catalog-signer-a.seed \
  --key /protected/catalog-signer-b.seed \
  --retain-existing \
  --artifact build/pkg-rot13/release-1.fpk \
  --output /release/public
```

For release 2.0 repeat with a higher catalog revision, a fresh validity interval
and `--artifact build/pkg-rot13/release-2.fpk`. Keep `--retain-existing`: it verifies
the previous catalog signatures against the supplied signing keys, verifies the
retained objects' hashes/identities, replaces the named package, and keeps all
other offers. All prior signers must still be supplied; key rotation is outside
this workflow. Expiry does not prevent a publisher from refreshing its own old
offer. A concurrent change to the catalog causes refusal under the writer lock;
retry against the latest publication. Do not run concurrent deployment pipelines.

Without `--retain-existing`, the original explicit full-catalog mode remains:
include every offered package with another `--artifact`, or omitted offers will
be dropped. Names are unique and sorted canonically. Revisions are nonzero uint64
values; publication refuses lower revisions or differing bytes at the same
revision. Identical publication is idempotent. Repository/root identity changes
in place are refused. Ensure signatures meet the target's configured policy.

Objects are flushed before atomic catalog activation. On the HTTP host, copy
new objects first, then copy the catalog to a temporary file on that same
filesystem and rename it to `catalog.v1` last. Do not remove older objects while
clients may still use a previously authenticated catalog. Do not deploy by
copying `catalog.v1` first or syncing an actively changing signing directory.

The publisher serializes writers with `.publish.lock`. Abrupt process death may
leave that lock, `.catalog.v1.new`, or a partial newly created object. After
confirming no publisher is running, inspect these files against the retained
release inputs before explicit cleanup/retry. The tool refuses differing existing
objects rather than overwriting evidence. This is fail-closed publication, not
automatic cleanup of an uncertain prior run.

For the existing Ubuntu document root, deploy only after local publication
finishes successfully (on that host, with the required filesystem permissions):

```bash
sudo mkdir -p /srv/fortress-repository/objects/sha256
sudo rsync -a /release/public/objects/sha256/ /srv/fortress-repository/objects/sha256/
sudo sync -f /srv/fortress-repository
sudo install -m 0644 /release/public/catalog.v1 /srv/fortress-repository/.catalog.v1.deploy
sudo sync -f /srv/fortress-repository
sudo mv -T /srv/fortress-repository/.catalog.v1.deploy /srv/fortress-repository/catalog.v1
sudo sync -f /srv/fortress-repository
```

Do not use `rsync --delete`. Keep keys outside both `/release/public` and the
HTTP root. These are deployment instructions, not evidence of server changes.
Stage 1 produces validated runnable ELF packages; installation, launch on
FortressOS, and Dell acceptance belong to Stage 2. After that stage, the filter
can be exercised with `echo Hello | rot13` (expected `Uryyb`).

## HTTP host

Install nginx on the Ubuntu HTTP host and serve only the public directory.
For example, a server block can contain:

```nginx
server {
    listen 80;
    server_name REPOSITORY-HOSTNAME;
    root /srv/fortress-repository;
    location = /catalog.v1 {
        add_header Cache-Control "no-cache" always;
        try_files $uri =404;
    }
    location /objects/sha256/ {
        add_header Cache-Control "public, max-age=31536000, immutable" always;
        try_files $uri =404;
    }
    location / { return 404; }
}
```

Run `nginx -t` before reloading the configured server. Configure HTTPS through
the server's normal certificate procedure if required by deployment policy.
Do not place signing seeds, authority configuration, source checkout or release
working files in `/srv/fortress-repository`.

## OS repository configuration

The current download command is:

```text
pkg fetch fortress-desktop /root/NEW-download-directory
```

Both configuration files must be root-owned regular files without group/other
write permission. `/etc/pkg/repository.conf` is exactly:

```text
version=1
url=https://fortress.ameyerocha.com
```

Use no trailing URL slash. `/etc/pkg/trust.conf` contains these ordered,
LF-terminated fields, replacing the uppercase placeholders:

```text
version=1
repository=REPOSITORY-UUID
root_version=1
platform=64-LOWERCASE-HEX-PLATFORM-ID
base_version=1
public_key=64-LOWERCASE-HEX-ED25519-PUBLIC-KEY
```

Use the same canonical lowercase repository UUID and root version as the
publisher. Platform/base values describe OS/package compatibility. Repeat the
`public_key=` line for each trusted catalog signing key, up to eight distinct
keys. At least one signature is required; every signature present must be valid
and from a configured key. This is publisher trust, not device enrollment. No
private key belongs on the laptop or public HTTP server. Initial public keys
must come from the publisher through a trusted transfer, not an unverified
download from the repository itself.

The client creates a new private staging directory, invokes the existing wget
without a shell, verifies the signed catalog before downloading its selected
object, then checks complete FPK integrity, ELF and platform/base compatibility.
Catalogs must be currently valid, so the OS clock must be correct. Failed or
rejected downloads stay in staging and never trigger installation.

Existing wget HTTPS verification, downgrade refusal, deadlines and 10 MiB
response bound remain. The client caps catalogs at 256 KiB and enforces the
existing FPK size limits. HTTP also works for signed-content/local testing.

This command retains `catalog.v1` and `artifact.fpk`; it does not install files
or persist installed-version/rollback floors. The next implementation step is
the ordinary installation backend. The existing Dell/USB binaries have not been
replaced by this source cleanup.

## rot13 2.0 update candidate

The second example is version 2.0, numeric build 2. It reports `rot13 2.0`
and adds `--help`; the filter still maps `hello` to `uryyb`. Its documentation
is a separate source file so building 1.0 does not silently change its artifact.
Use the approved platform/base values when creating release-2.json as above.
Publish release-2.fpk with a higher catalog revision and `--retain-existing`.

The Stage 3 source adds verified `pkg upgrade` support; refresh both the kernel
and `/bin/pkg` before testing it on the Dell. The old Stage 2 image cannot gain
upgrade support just by downloading rot13 2.0. Preserve the installed root and
data; do not format or delete its package database.

After booting the matching new kernel and CLI:

```sh
pkg update
pkg get rot13
sudo pkg upgrade rot13
/usr/bin/rot13 --version
echo hello | /usr/bin/rot13
sudo pkg upgrade rot13
```

Expect `upgrade complete`, then `rot13 2.0` and `uryyb`; a repeat reports
`package is up to date rot13 v 2.0`. Reboot and check the version/filter again.
`pkg install` still rejects replacing a different installed build. Keep old
repository objects available: the privileged upgrade downloads the prior object
by its installed SHA-256, and the kernel revalidates ownership independently.
See [bounded upgrade contracts and verification](plans/PKG3_BOUNDED_UPGRADE.md).
The repository's current deterministic demo signing key is not suitable for a
production trust deployment.
