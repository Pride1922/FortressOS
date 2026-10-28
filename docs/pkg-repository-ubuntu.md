# Ubuntu repository publication

The host publisher is `scripts/pkg_repository.py`. It produces the existing
PKG-1 signed binary catalog and immutable content-addressed FPK objects. The
target client, production enrollment and repeated upgrades are **not yet
implemented or accepted** by this checkpoint. These instructions prepare a
repository; they do not enable updating an installed Dell.

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

No server details or server access are needed for the local implementation and
disposable acceptance tests. The eventual client configuration and deployment
procedure must be documented once implemented. Software local floors do not
prevent rollback of the entire root plus its independent boot policy; retain the
approved signed boot checkpoint separately, as agreed in the contract proposal.
