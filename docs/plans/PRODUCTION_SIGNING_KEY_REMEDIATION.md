# Production signing key remediation

Status: **planned, not implemented**. Recorded 2026-11-02.
The current repository and installer trust a publicly known deterministic demo
key. They must not be described as production-ready. Committing this plan does
not create keys, rotate trust, publish a catalog or modify an installed system.

## Confirmed deployment

- Cloudflare SSH entry: `fabio@ssh-edge2.ameyerocha.com`, hostname `edge2`.
- Internal Ubuntu repository: `fabio@192.168.0.247`, hostname `Repository-Fortress`.
- Public URL: `https://fortress.ameyerocha.com`.
- Document root: `/srv/fortress-repository`.
- Existing signing seed: `/home/fabio/.local/share/fortress-signing/repo-demo.seed`.
- Prior deployment script: `/home/fabio/.cache/fortress-publish/rot13-2.0/publish-rot13-2.0.py`.
  That script explicitly created the deterministic demo seed and published revision 2.
- Repository UUID `00010203-0405-0607-0809-0a0b0c0d0e0f`, root version 1,
  last observed catalog revision 2, rot13 build 2. Re-read live state before changes.
- `/etc/fortress-keys` was absent on both hosts. It is the proposed new protected
  location, not the location of an existing production key.

## Required implementation, in order

1. **Preserve the deployment before changing it.** Record the current catalog,
   public trust, object hashes and permissions. Back up public catalog/objects and
   retain both rot13 versions. Never erase or recreate installed package inventories,
   reset counters, format a disk or overwrite application settings to rotate trust.
2. **Create one real key on the repository server.** Use the operating system's
   cryptographic random source through the Ed25519 implementation to generate a new
   32-byte private seed, exclusively created at
   `/etc/fortress-keys/catalog-ed25519.seed`, directory 0700 and file 0600. Choose
   the signing owner explicitly (root or a dedicated signer), with no web-server
   read access. Refuse an existing destination, symlinks and unsafe ownership.
   Adapt invocation to that ownership instead of loosening permissions. No demo
   fallback, hardcoded seed, private-key stdout, chat output or Git copy.
3. **Back up the private key before activation.** Produce an encrypted backup to
   a separately controlled destination and verify restoration by deriving the
   public key in an isolated temporary location. Keep the backup secret outside
   the web root, source checkout and installer. Record only its location and
   public fingerprint in the operational runbook. Do not declare this complete
   until the backup destination and restore check are recorded.
4. **Make release trust an explicit build input.** Replace the hardcoded demo trust
   in `Makefile` and `scripts/create_installer_payload.py` with a shared validated
   public release-trust input. Thread it through the complete installer and base
   artifact builders. Release builds must fail if this input is missing or contains
   the known demo key; demo trust must require an explicit test-only selection.
   The private seed never enters build inputs or media. Preserve platform identity
   and base ABI unless a separately reviewed compatibility change requires otherwise.
5. **Specify the trust transition against actual backend rules.** Retain the
   repository UUID and propose root version 2 with only the new public key. Audit
   inventory/catalog floors and source verification first: confirm how existing
   rot13 and factory records behave across root-version changes. Implement any
   necessary explicit administrative migration; do not assume editing trust.conf
   alone handles persisted state. Never lower catalog/build floors. A catalog
   signed by the compromised demo key cannot securely authorize this transition.
6. **Prepare production publication separately.** Build a staging tree retaining
   every currently offered package and old content-addressed object. Verify their
   hashes against the recorded old deployment. Sign the replacement catalog on
   the server with the new key and the next valid revision (at least 3 if live
   revision is still 2). The existing `--retain-existing` path verifies old signatures
   with supplied keys, so implement/review an explicit migration path rather than
   silently bypassing verification or including the demo key in future client trust.
   Verify the staged catalog using only the intended new public key.
7. **Prepare the corrected installer before activation.** Rebuild the complete
   installer with the new public trust and four exact factory application packages.
   Audit both root payload and bootstrap for the correct public key, absence of
   demo trust/private material, and correct package hashes. Mark the current
   demo-trusting image superseded; retain a single final image after replacement
   succeeds. No physical flashing is performed by this plan.
8. **Migrate existing installations through a trusted local administrative path.**
   Back up their trust files, inspect backend compatibility and atomically install
   the new public trust under root authorization. Preserve rot13, documents,
   configuration, package records and monotonic floors. If backend changes are
   required, deliver and verify that migration before activation. No reinstallation
   is assumed or required merely to replace a public key. Physical operations
   remain user-operated. State the precise recovery procedure before cutover.
9. **Activate and verify.** Publish immutable objects first and atomically switch
   the catalog only when the new installer and existing-client transition are
   ready. Coordinate the switch because old-trust clients will reject the new
   catalog. Verify public HTTPS bytes and Ed25519 signatures independently, all
   retained packages, and the exact desktop factory artifacts. Prove new-key
   acceptance and old-key-only rejection without writes on rejection. Acceptance
   includes an actual application upgrade, settings/rot13 preservation, reboot and
   a complete base update/confirmation/recovery sequence. Host checks alone must
   not be reported as firmware or physical acceptance.
10. **Retire demo authority and document the final state.** Remove the demo key
    from all production trust inputs and prevent the historical deployment script
    from recreating it in future production runs. Keep any demo fixture explicitly
    isolated. Record key owner/path, public fingerprint, backup/restore procedure,
    catalog revision, image hash and publication commands in the repository runbook.
    Record failures and recovery steps without secret contents.

## Completion criteria

A private, backed-up production key exists only in controlled signing/backup
locations; builds require its public trust; no production client trusts the demo
key; publication and installed-client migration preserve existing packages and
floors; actual acceptance evidence and the operational runbook match deployment.
Until then, the key problem remains open.
