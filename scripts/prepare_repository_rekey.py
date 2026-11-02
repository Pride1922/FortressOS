"""Prepare a new-key catalog for an explicitly authorized reflash cutover.

Runs on the signing server. Never exports a private key or changes the live
repository. The operator pins the prior catalog and public signing key; all
retained objects are verified before a new catalog is signed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time
import uuid
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
import pkg_repository as publisher


def prepare(repository, key, trust, bundle, output, expected, previous_public):
    if output.exists():
        raise ValueError('preparation output must be new')
    current = publisher.regular(repository / 'catalog.v1', 256 * 1024)
    if hashlib.sha256(current).hexdigest() != expected:
        raise ValueError('live catalog changed; inspect before retry')
    values = dict(line.split('=', 1) for line in trust.read_text().splitlines())
    identity = uuid.UUID(values['repository']).bytes
    root = int(values['root_version'])
    keys = publisher.signing_keys([key], output)
    public = publisher.public(keys[0])
    if public.hex() != values['public_key'] or public == bytes.fromhex(previous_public):
        raise ValueError('new private key does not match distinct release public trust')

    class PreviousVerifier:
        def public_key(self):
            return Ed25519PublicKey.from_public_bytes(bytes.fromhex(previous_public))

    manifest = json.loads(bundle.read_text())
    replacements = []
    for item in manifest['packages']:
        # Bundle paths are relocated onto the signing server, not trusted verbatim.
        data = publisher.regular(bundle.parent / (item['name'] + '.fpk'), publisher.MAX_ARTIFACT)
        if hashlib.sha256(data).hexdigest() != item['sha256']:
            raise ValueError('bundle artifact hash differs')
        if publisher.artifact_identity(data) != (item['name'].encode(), item['build']):
            raise ValueError('bundle artifact identity differs')
        replacements.append(data)
    artifacts, previous = publisher.retained_artifacts(
        repository, replacements, [PreviousVerifier()], identity, root)
    if previous != current:
        raise ValueError('catalog changed during preparation')
    revision = struct.unpack_from('<Q', current, 40)[0] + 1
    issued = int(time.time())
    wire, objects = publisher.catalog(identity, root, revision, issued,
                                      issued + 90 * 24 * 60 * 60, artifacts, keys)
    # Verify with only the production key before writing the public staging tree.
    body = 72 + struct.unpack_from('<H', wire, 64)[0] * 81
    keys[0].public_key().verify(wire[body + 32:body + 96], wire[:body])
    publisher.publish(output, wire, objects, expected_catalog=None)
    receipt = {'result': 'PREPARED_NOT_ACTIVATED', 'previous_catalog_sha256': expected,
               'catalog_sha256': hashlib.sha256(wire).hexdigest(), 'revision': revision,
               'public_key': public.hex(), 'packages': [publisher.artifact_identity(a)[0].decode() for a in artifacts]}
    (output / 'rekey-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repository', type=Path, required=True)
    p.add_argument('--key', type=Path, required=True)
    p.add_argument('--trust', type=Path, required=True)
    p.add_argument('--bundle', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--expected-catalog-sha256', required=True)
    p.add_argument('--previous-public-key', required=True)
    a = p.parse_args()
    prepare(a.repository, a.key, a.trust, a.bundle, a.output,
            a.expected_catalog_sha256, a.previous_public_key)
