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

Build the unsigned desktop using the existing `build_desktop_package.py` with
the approved platform ID, compatible base range and strictly increasing build.
Keep `/bin/compositor` in the base and exclude mutable settings from the payload.
Catalog publication example (replace every placeholder with release policy):

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
  --artifact /release/fortress-desktop.fpk \
  --output /release/public
```

Include every currently offered package with another `--artifact`. Names are
unique and sorted canonically. Revisions are nonzero uint64 values; an existing
publication rejects lower revisions and different bytes at the same revision.
Identical publication is idempotent. Changing repository/root identity in place
is refused: online root/key rotation is not implemented. Ensure the signatures
meet the independently selected threshold and include no unauthorized signer.

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
