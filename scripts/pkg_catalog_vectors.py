"""Candidate signed catalog vectors. Public deterministic TEST keys only.
Independent Ed25519 implementation: Python cryptography/OpenSSL.
"""
import hashlib
import json
import pathlib
import struct
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

OUT = pathlib.Path('build/pkg-format')
KEYS = [Ed25519PrivateKey.from_private_bytes(bytes([i])*32) for i in [1, 2, 3]]
PUB = [k.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw) for k in KEYS]
UUID = bytes(range(16))
NOW = 1792540800
ARTIFACT = (OUT/'fixture.fpk').read_bytes()


def entry(name='demo', build=1, length=len(ARTIFACT), digest=hashlib.sha256(ARTIFACT).digest()):
    b = name.encode()
    return bytes([len(b)]) + b.ljust(32, b'\0') + struct.pack('<QQ', build, length) + digest


def catalog(entries=None, signers=(0, 1), revision=5, root=1, issued=NOW-1, expires=NOW+60,
            uuid=UUID, domain=b'FORT-PKG-CAT-v1\0', reserved=0):
    if entries is None:
        entries = [entry()]
    body = (struct.pack('<16s16sQQQQHHI', domain, uuid, root, revision, issued, expires,
                        len(entries), len(signers), reserved) + b''.join(entries))
    signatures = sorted((hashlib.sha256(PUB[i]).digest(), KEYS[i].sign(body)) for i in signers)
    for i in signers:
        KEYS[i].public_key().verify(KEYS[i].sign(body), body)
    return body + b''.join(k+s for k, s in signatures)


def policy(root=1, floor=0, now=NOW, floor_hash=bytes(32), threshold=2, keys=PUB, uuid=UUID):
    return struct.pack('<16sQQQ32sII256s', uuid, root, floor, now, floor_hash,
                       threshold, len(keys), b''.join(keys).ljust(256, b'\0'))


def main():
    good = catalog()
    assert good == catalog()
    cases = []

    def add(label, data=good, rules=None, expected=False, chain=2):
        cases.append((label, data, policy() if rules is None else rules, expected, chain))

    add('valid-2-of-3', expected=True, chain=1)
    add('valid-three-signers', catalog(signers=(0, 1, 2)), expected=True, chain=1)
    add('same-revision-identical', rules=policy(floor=5, floor_hash=hashlib.sha256(good).digest()), expected=True, chain=1)
    add('revision-rollback', rules=policy(floor=6))
    add('same-revision-different', rules=policy(floor=5, floor_hash=b'X'*32))
    add('wrong-root', catalog(root=2))
    add('wrong-repo', catalog(uuid=b'X'*16))
    add('wrong-role', catalog(domain=b'FORT-PKG-ROOT-v1'))
    add('future-issued', catalog(issued=NOW+1))
    add('expired', catalog(expires=NOW))
    add('clock-unavailable', rules=policy(now=0))
    add('clock-upper-bound', rules=policy(now=4102444801))
    add('zero-revision', catalog(revision=0))
    add('no-signatures', catalog(signers=()))
    add('threshold-failure', catalog(signers=(0,)))
    add('duplicate-signer', catalog(signers=(0, 0)))
    add('unknown-signer', rules=policy(keys=[PUB[0], PUB[2]]))
    add('duplicate-trusted-key', rules=policy(keys=[PUB[0], PUB[0]]))
    add('zero-trusted-key', rules=policy(keys=[PUB[0], bytes(32)]))
    add('invalid-threshold', rules=policy(threshold=0))
    add('threshold-over-key-count', rules=policy(threshold=4))
    add('no-keys', rules=policy(keys=[]))
    add('zero-uuid', rules=policy(uuid=bytes(16)))
    add('reserved', catalog(reserved=1))
    add('empty-catalog', catalog(entries=[]))
    add('duplicate-entry', catalog(entries=[entry(), entry()]))
    add('unsorted', catalog(entries=[entry('z'), entry('demo')]))
    add('bad-name', catalog(entries=[entry('../bad')]))
    add('zero-build', catalog(entries=[entry(build=0)]))
    add('bad-length', catalog(entries=[entry(length=2**64-1)]))
    add('name-padding', catalog(entries=[entry()[:5]+b'X'+entry()[6:]]))
    add('max-packages', catalog(entries=[entry(f'a{i:03}') for i in range(256)]), expected=True)
    add('too-many-packages', catalog(entries=[entry(f'a{i:03}') for i in range(257)]))
    b = bytearray(good); b[-1] ^= 1
    add('bad-signature', b)
    b = bytearray(good); b[72+49] ^= 1
    add('tampered-entry', b)
    body_len = 72+81
    add('signatures-unsorted', good[:body_len]+good[body_len+96:]+good[body_len:body_len+96])
    add('trailing-garbage', good+b'X')
    add('build-binding', catalog(entries=[entry(build=2)]), expected=True, chain=0)
    add('package-binding', catalog(entries=[entry('other')]), expected=True, chain=0)
    add('artifact-hash-binding', catalog(entries=[entry(digest=b'X'*32)]), expected=True, chain=0)
    with (OUT/'catalog-vectors.bin').open('wb') as f:
        for _, data, rules, expected, chain in cases:
            f.write(struct.pack('<BBI', expected, chain, len(data)))
            f.write(rules)
            f.write(data)
    (OUT/'catalog-vectors.json').write_text(json.dumps({
        'test_public_keys': [p.hex() for p in PUB],
        'catalog_sha256': hashlib.sha256(good).hexdigest(),
        'cases': [dict(name=label, accepted=expected, chain=chain) for label, _, _, expected, chain in cases],
    }, indent=2)+'\n')
    print(f'Generated {len(cases)} signed catalog vectors (TEST keys only)')


if __name__ == '__main__':
    main()
