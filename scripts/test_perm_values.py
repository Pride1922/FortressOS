"""Independent value-rule oracle. No filesystem, kernel or hardware claim."""
from pathlib import Path
import hashlib
import itertools
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def access(mode, mask, identity, capability, directory, readonly):
    if readonly and mask & 2:
        return -30
    if not directory and mask & 1 and not mode & 0o111:
        return -13
    classes = ((mode // 64) % 8, (mode // 8) % 8, (mode // 8) % 8, mode % 8)
    allowed = {bit for bit in (1, 2, 4) if classes[identity] & bit}
    requested = {bit for bit in (1, 2, 4) if mask & bit}
    if requested <= allowed or capability == 1:
        return 0
    if capability == 2 and requested <= ({1, 4} if directory else {4}):
        return 0
    return -13


def create(mode, umask, parent_setgid, member, fsetid, directory):
    result = sum(bit for bit in (1 << n for n in range(12))
                 if mode & bit and not umask & bit)
    if parent_setgid and directory:
        result |= 0o2000
    # Without parent inheritance, the selected group is the caller's egid.
    selected_group_member = not parent_setgid or member != 2
    if not directory and not selected_group_member and not fsetid:
        result &= ~0o2000
    return result


def main():
    parent = ROOT / 'build/permissions-phase2'
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='values-', dir=parent))
    matrix = bytearray(access(*case) & 255 for case in itertools.product(
        range(4096), range(1, 8), range(4), range(3), range(2), range(2)))
    (out / 'access.bin').write_bytes(matrix)
    creates = bytearray()
    for case in itertools.product(range(4096), (0, 1, 7, 0o22, 0o27, 0o77, 0o700, 0o777),
                                  range(2), range(3), range(2), range(2)):
        creates.extend(create(*case).to_bytes(2, 'little'))
    (out / 'create.bin').write_bytes(creates)
    masks = bytearray()
    for mask, mode, directory in itertools.product(range(512),
            (0, 0o777, 0o1777, 0o2777, 0o6777, 0o7777, 0xffffffff), range(2)):
        masks.extend(create(mode, mask, 1, 2, 0, directory).to_bytes(2, 'little'))
    (out / 'umask.bin').write_bytes(masks)
    chmod = bytearray()
    for mode, member, caps, owner, readonly in itertools.product(
            range(4096), range(2), range(4), range(2), range(2)):
        error = -30 if readonly else -1 if not owner and caps not in (1, 3) else 0
        new_mode = mode if member or caps in (2, 3) else mode & ~0o2000
        chmod.append(error & 255)
        chmod.extend((0o100000 | new_mode).to_bytes(2, 'little'))
    (out / 'chmod.bin').write_bytes(chmod)
    command = ['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
               '-Wall', '-Wextra', '-Werror', '-no-pie', '-Isrc/include', '-Isrc/fs',
               'tests/perm_values_host.c', 'src/fs/permission_values.c',
               'src/kernel/creds.c', '-o', str(out / 'values')]
    subprocess.run(command, cwd=ROOT, check=True)
    run = subprocess.run([str(out / 'values'), str(out / 'access.bin'),
                          str(out / 'create.bin'), str(out / 'umask.bin'), str(out / 'chmod.bin')],
                         cwd=ROOT, text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=120)
    (out / 'result.log').write_text(run.stdout)
    print(run.stdout, end='', flush=True)
    run.check_returncode()
    files = ('src/fs/permission_values.c', 'src/fs/permission_values.h',
             'src/kernel/creds.c', 'tests/perm_values_host.c', 'scripts/test_perm_values.py')
    manifest = {'compile': command, 'access_cases': len(matrix),
                'create_cases': len(creates) // 2, 'umask_cases': len(masks) // 2,
                'chmod_cases': len(chmod) // 3,
                'sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                           for name in files},
                'scope': 'pure value decisions; filesystem integration has separate tests'}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'PASS permission value gates; evidence {out}', flush=True)


if __name__ == '__main__':
    main()
