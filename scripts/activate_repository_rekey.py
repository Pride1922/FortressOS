"""Activate an inspected production rekey staging tree; no private-key access."""
import argparse
import hashlib
import json
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
import pkg_repository as publisher


def activate(repository, staged, trust):
    receipt = json.loads((staged / 'rekey-receipt.json').read_text())
    fields = dict(line.split('=', 1) for line in trust.read_text().splitlines())
    if fields['public_key'] != receipt['public_key']:
        raise ValueError('staged signer differs from release trust')
    if fields['public_key'] == '03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8':
        raise ValueError('demo production signer refused')

    class Verifier:
        def public_key(self):
            return Ed25519PublicKey.from_public_bytes(bytes.fromhex(fields['public_key']))

    import uuid
    artifacts, wire = publisher.retained_artifacts(staged, [], [Verifier()],
        uuid.UUID(fields['repository']).bytes, int(fields['root_version']))
    if hashlib.sha256(wire).hexdigest() != receipt['catalog_sha256']:
        raise ValueError('staged catalog receipt differs')
    previous = publisher.regular(repository / 'catalog.v1', 256 * 1024)
    if previous == wire:
        print('Production catalog is already active.')
        return
    if hashlib.sha256(previous).hexdigest() != receipt['previous_catalog_sha256']:
        raise ValueError('live catalog changed; inspect before activation')
    backup = staged.parent / 'catalog-before-production-key.v1'
    publisher.immutable(backup, previous)
    objects = {publisher.sha(a).hex(): a for a in artifacts}
    publisher.publish(repository, wire, objects, expected_catalog=previous)
    print('Production catalog activated; previous catalog saved:', backup)
    print('SHA256:', hashlib.sha256(wire).hexdigest())


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repository', type=Path, required=True)
    p.add_argument('--staged', type=Path, required=True)
    p.add_argument('--trust', type=Path, required=True)
    a = p.parse_args()
    activate(a.repository, a.staged, a.trust)
