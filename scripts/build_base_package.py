#!/usr/bin/env python3
"""Build an unsigned bounded fortress-base object; sign with pkg_repository.py.

No keys, publication or devices are accessed. Layout 1 has a fixed namespace map
and preserves mutable configuration. Configuration ABI is explicitly versioned.
"""
import argparse
import hashlib
import io
from pathlib import Path
import struct
import tarfile
from pkg_build import regular
from create_installer_media import elf_load_image

MAX = 128 + 16384 + 8*1024*1024
BANK = 8*1024*1024
APPLICATION_BINARIES = frozenset(("bin/notepad", "bin/files", "bin/terminal", "bin/calc"))


def release(build, abi, platform):
    if not 0 < build < 2**64 or not 0 < abi < 2**64 or len(platform) != 32 or not any(platform):
        raise ValueError('positive build/ABI and nonzero platform required')
    return struct.pack('<8sQQ32sQ', b'FOSBASE1', build, abi, platform, 1)


def bootstrap(data, build, abi, platform):
    out = io.BytesIO()
    seen = set()
    with tarfile.open(fileobj=io.BytesIO(data)) as source, tarfile.open(fileobj=out, mode='w', format=tarfile.USTAR_FORMAT) as target:
        for m in source:
            name = m.name.rstrip('/')
            if name == 'bootstrap' or name.startswith('bootstrap/'):
                raise ValueError('personalized recovery material cannot be published')
            if name in seen or not (m.isfile() or m.isdir()) or m.uid or m.gid:
                raise ValueError('unique root-owned regular files/directories required')
            seen.add(name)
            if name in APPLICATION_BINARIES or name in ('base', 'base/release.v1'):
                continue
            target.addfile(m, source.extractfile(m) if m.isfile() else None)
        m = tarfile.TarInfo('base');m.type = tarfile.DIRTYPE;m.mode = 0o755;target.addfile(m)
        payload = release(build, abi, platform)
        m = tarfile.TarInfo('base/release.v1');m.mode = 0o644;m.size = len(payload)
        target.addfile(m, io.BytesIO(payload))
    value = out.getvalue()
    if len(value) > BANK-2560:
        raise ValueError('bootstrap leaves no room for installation recovery record')
    return value


def encode(kernel, boot, build, minimum, maximum, abi, platform):
    if not 0 < minimum <= maximum < build:
        raise ValueError('predecessors must be older than release')
    # elf_load_image also validates all loadable segment file bounds.
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', kernel)
    if header[0][:7] != b'\x7fELF\x02\x01\x01' or header[2] != 62 or not header[10]:
        raise ValueError('x86-64 ELF required')
    h = bytearray(256)
    struct.pack_into('<8sIIQQQQQ', h, 0, b'FORTBAS1', 1, 256, build, minimum, maximum, len(kernel), len(boot))
    h[56:88] = platform;h[88:120] = hashlib.sha256(kernel).digest();h[120:152] = hashlib.sha256(boot).digest()
    struct.pack_into('<QQ', h, 152, 1, abi)
    value = bytes(h)+kernel+boot
    if not 64 <= len(kernel) <= BANK or len(value) > MAX:
        raise ValueError('base object exceeds catalog v1 bound; no implicit limit increase')
    return value


def validate(data):
    if not 256 <= len(data) <= MAX:
        raise ValueError('base artifact bounds')
    magic,version,size,build,minimum,maximum,kn,bn=struct.unpack_from('<8sIIQQQQQ',data)
    layout,abi=struct.unpack_from('<QQ',data,152)
    platform=data[56:88]
    if (magic!=b'FORTBAS1' or version!=1 or size!=256 or layout!=1 or not abi
            or not 0<minimum<=maximum<build or not any(platform) or any(data[168:256])
            or not 64<=kn<=BANK or not 1024<=bn<=BANK-2560 or bn%512 or len(data)!=256+kn+bn):
        raise ValueError('invalid base header')
    kernel,boot=data[256:256+kn],data[256+kn:]
    if hashlib.sha256(kernel).digest()!=data[88:120] or hashlib.sha256(boot).digest()!=data[120:152]:
        raise ValueError('base payload hash mismatch')
    with tarfile.open(fileobj=io.BytesIO(boot)) as archive:
        members=archive.getmembers()
        if len(members)>510 or len({m.name.rstrip('/') for m in members})!=len(members):
            raise ValueError('ambiguous bootstrap')
        for m in members:
            if not(m.isfile() or m.isdir()) or m.uid or m.gid or m.name.startswith(('/', 'bootstrap/')) or m.name=='bootstrap' or '..' in Path(m.name).parts:
                raise ValueError('unsupported bootstrap entry')
        if archive.extractfile('base/release.v1').read()!=release(build,abi,platform):
            raise ValueError('bootstrap release identity mismatch')
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, default=Path('bin/fortress.elf'))
    p.add_argument('--bootstrap', type=Path, default=Path('bin/initramfs.tar'))
    p.add_argument('--platform-id', required=True)
    p.add_argument('--build', type=int, required=True)
    p.add_argument('--predecessor-min', type=int, required=True)
    p.add_argument('--predecessor-max', type=int, required=True)
    p.add_argument('--base-abi', type=int, default=1)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args();platform = bytes.fromhex(a.platform_id)
    elf_load_image(a.kernel)
    boot = bootstrap(regular(a.bootstrap, BANK), a.build, a.base_abi, platform)
    value = encode(regular(a.kernel, BANK), boot, a.build, a.predecessor_min, a.predecessor_max, a.base_abi, platform)
    validate(value)
    if a.out.is_symlink():raise ValueError('symlink destination refused')
    a.out.parent.mkdir(parents=True, exist_ok=True);a.out.write_bytes(value)
    print(f'{a.out}: {len(value)} bytes, sha256={hashlib.sha256(value).hexdigest()}')


if __name__ == '__main__':main()
