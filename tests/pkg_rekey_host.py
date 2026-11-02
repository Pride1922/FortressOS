"""Actual rekey preparation/activation with disposable keys and real app FPKs."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import time
import uuid
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import pkg_repository as pub
from prepare_repository_rekey import prepare
from activate_repository_rekey import activate
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.serialization import Encoding, PrivateFormat, NoEncryption

root = Path(__file__).resolve().parents[1]
receipt = json.loads((root / 'bin/fortress-base-installer.manifest.json').read_text())
bundle = receipt['factory_packages']
with tempfile.TemporaryDirectory(prefix='fortress-rekey-') as folder:
    out = Path(folder)
    old_key, new_key = (pub.Ed25519PrivateKey.generate() for _ in range(2))
    seed = out / 'new.seed'
    seed.write_bytes(new_key.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption()))
    seed.chmod(0o600)
    identity = uuid.UUID('00010203-0405-0607-0809-0a0b0c0d0e0f').bytes
    records = bundle['packages']
    # Retain terminal while replacing the other three application offers.
    original = Path(records[-1]['path']).read_bytes()
    wire, objects = pub.catalog(identity, 1, 2, int(time.time()), int(time.time()) + 3600, [original], [old_key])
    live = out / 'live'
    pub.publish(live, wire, objects, expected_catalog=None)
    source = out / 'inputs'
    source.mkdir()
    selected = []
    for item in records[:-1]:
        (source / (item['name'] + '.fpk')).write_bytes(Path(item['path']).read_bytes())
        selected.append(item)
    manifest = source / 'bundle.json'
    manifest.write_text(json.dumps(dict(bundle, packages=selected)))
    trust = out / 'trust.conf'
    trust.write_text('version=1\nrepository=' + str(uuid.UUID(bytes=identity)) + '\nroot_version=1\npublic_key=' + pub.public(new_key).hex() + '\n')
    expected = hashlib.sha256(wire).hexdigest()
    try:
        prepare(live, seed, trust, manifest, out / 'refused', '00' * 32, pub.public(old_key).hex())
    except ValueError:
        pass
    else:
        raise AssertionError('changed previous catalog accepted')
    assert not (out / 'refused').exists()
    staged = out / 'signed'
    prepare(live, seed, trust, manifest, staged, expected, pub.public(old_key).hex())
    signed = (staged / 'catalog.v1').read_bytes()
    broken = bytearray(signed)
    broken[-1] ^= 1
    (staged / 'catalog.v1').write_bytes(broken)
    try:
        activate(live, staged, trust)
    except (ValueError, InvalidSignature):
        pass
    else:
        raise AssertionError('invalid signature activated')
    assert (live / 'catalog.v1').read_bytes() == wire
    (staged / 'catalog.v1').write_bytes(signed)
    activate(live, staged, trust)
    activate(live, staged, trust)
    assert (live / 'catalog.v1').read_bytes() == signed
    assert struct.unpack_from('<H', signed, 64)[0] == 4
    assert (live / 'objects/sha256' / (hashlib.sha256(original).hexdigest() + '.fpk')).read_bytes() == original
    body = 72 + 4 * 81
    try:
        old_key.public_key().verify(signed[body + 32:body + 96], signed[:body])
    except InvalidSignature:
        pass
    else:
        raise AssertionError('new catalog validates under retired key')
print('PASS: pinned input, retained objects, signature rejection without writes, new-key activation and repeat')
