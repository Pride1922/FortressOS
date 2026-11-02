"""Shared public repository trust for release media. Never reads private keys."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
DEMO_PUBLIC = '03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8'


def release_trust():
    data = (ROOT / 'assets/pkg/trust.conf').read_bytes()
    lines = data.decode('ascii').splitlines()
    expected = ['version', 'repository', 'root_version', 'platform', 'base_version', 'public_key']
    fields = [line.split('=', 1) for line in lines]
    if [item[0] for item in fields] != expected or any(len(item) != 2 for item in fields):
        raise ValueError('invalid release trust fields')
    values = dict(fields)
    import uuid
    if not uuid.UUID(values['repository']).int or values['version'] != '1':
        raise ValueError('invalid release repository identity')
    for name in ('root_version', 'base_version'):
        if not 0 < int(values[name]) < 2**64 - 1:
            raise ValueError('invalid release trust version')
    for name in ('platform', 'public_key'):
        value = values[name]
        if len(value) != 64 or value != bytes.fromhex(value).hex() or not any(bytes.fromhex(value)):
            raise ValueError('invalid release public identity')
    if values['public_key'] == DEMO_PUBLIC:
        raise ValueError('demo signing key is forbidden in release trust')
    if not data.endswith(b'\n'):
        raise ValueError('release trust requires final newline')
    return data


if __name__ == '__main__':
    Path(sys.argv[1]).write_bytes(release_trust())
