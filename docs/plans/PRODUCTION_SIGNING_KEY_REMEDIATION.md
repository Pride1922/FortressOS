# Production signing key — direct reflash cutover

This replaces the earlier migration plan. The user chose a fresh Dell install;
there is no in-place trust migration or mandatory encrypted-backup work.

## Implemented

1. A random Ed25519 seed was generated on `Repository-Fortress` (`192.168.0.247`)
   at `/etc/pkg/signing.seed`, root-owned mode 0600. `/etc/pkg` is mode 0700.
   Only the public key was copied to the laptop. The private seed never entered
   the source tree, installer, web root or chat.
2. Public release trust is now shared at `assets/pkg/trust.conf`. `Makefile` and
   the installer producer use `scripts/release_trust.py`; it rejects the known
   demo key. The base installer refuses a stale bootstrap with different trust.
3. The complete installer was rebuilt with production public trust and four
   factory packages: desktop, files, notepad and terminal. The public key is
   `40bce69ef4889755601c4a2fab4e71c1d061ef8325cf722beddb41d09ac9cc5d`.
4. Repository signing uses `/etc/pkg/signing.seed`. The one-time preparation
   helper verifies the pinned old catalog and retained objects, signs with the
   new key and writes a separate staging tree. Activation verifies the new
   signature, saves the old catalog, publishes objects first and atomically
   replaces the live catalog. Existing rot13 objects are retained.
5. The user flashes the refreshed image and installs on the Dell, then uses
   `sudo pkg upgrade`. Old demo-trusting installations will reject the new
   catalog; that is expected, and this path replaces them by reinstalling.

## Commands and files

On the repository server, privileged key creation is repeatable without replacing
an existing valid key:

```sh
sudo python3 scripts/create_repository_signing_key.py
```

The signing seed is raw 32-byte Ed25519 material for `pkg_repository.py --key`.
The public file `/etc/pkg/trust.pub` is raw 32 bytes; release trust represents it
as lowercase hex. Public fingerprints and image receipts can be committed;
private seed contents cannot.

For later publications, use the existing publisher under sudo with
`--key /etc/pkg/signing.seed --retain-existing`, the next catalog revision and
real artifacts with increasing build numbers. The old-key transition helpers
are needed only once. Do not rerun the historical demo deployment script.

No mandatory key backup was made. Losing this seed without a backup requires
another public-trust replacement. Backups remain optional at the user's choice.

## Delivery status

- Production key and public release trust: complete.
- Complete installer rebuild and filesystem/media audits: complete.
- Signed catalog preparation/public activation: complete; revision 3. Public HTTPS
  signature and object checks passed. See [repository operations](../pkg-repository-ubuntu.md).
- Dell flashing, actual package upgrade and reboot acceptance: user-operated,
  pending. No physical device was written by this implementation.

The key correction does not establish base-update firmware or physical acceptance;
those existing acceptance limits remain in the subsystem records.

The delivered image captures the source snapshot in its manifest. Other desktop
work continued during the signing session; later edits are not part of that image
or its immutable build-1 package bundle. Changed artifacts must use a higher app
build number and a new catalog revision, never replace the published build-1 hashes.
