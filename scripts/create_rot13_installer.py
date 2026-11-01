#!/usr/bin/env python3
"""Build a flashable installer with rot13 1.0 already installed in its root seed.

Uses only the existing, explicitly public demo repository identity/key. Refuses
other trust policies. Private host adapters/signing material never enter media.
No physical device is opened; publication is a new regular file under bin/.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import pkg_build
import pkg_repository as repo
from create_installer_media import build, digest, ROOT
import test_root_phase0 as fixture

URL = 'https://fortress.ameyerocha.com'
V1_HASH = '2dc44c5cdaf4954b9f828c87ac584af723582c524edc50f12c227a88e4814d94'
PUBLIC_KEY = '03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8'


def download(url, maximum):
    data = subprocess.check_output(['curl', '-fsSL', '--max-time', '60', '--max-filesize', str(maximum), url])
    assert len(data) <= maximum
    return data


def seed(templates, artifact, catalog, out, now):
    work = out / 'host-work'; work.mkdir()
    for name in ('src', 'user', 'tests'):
        shutil.copytree(ROOT / name, work / name, ignore=shutil.ignore_patterns('target', '__pycache__'))
    # Existing host adapter fixes the clock. Change only its private source copy
    # to the signed release time; the actual kernel keeps its RTC clock path.
    source = work / 'src/fs/pkg_executor.inc'; text = source.read_text()
    assert text.count('now.tv_sec = 1730000000;') == 1
    source.write_text(text.replace('now.tv_sec = 1730000000;', f'now.tv_sec = {now};'))
    # The seed deliberately leaves descriptor-growth padding free for the
    # installer's exclusive resize. Ordinary first-fit allocation would consume
    # it before installation. Only this host producer skips those free blocks;
    # shipped allocation and expansion code are unchanged.
    source = work / 'src/fs/ext4_mutate.inc'; text = source.read_text()
    needle = 'for (uint32_t bit=0;bit<count;bit++) if (!e4_bit(map,bit)) {'
    assert text.count(needle) == 1
    source.write_text(text.replace(needle, needle + '\n            if (g<2 && bit>=2 && bit<=64) continue;'))
    flags = ['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-pthread',
             '-DHOST_TEST', '-DTOOL_HOST_TEST', '-DFORTRESS_DAC_ENFORCED',
             '-I.', '-Itests', '-Itests/ext4_host', '-Itests/host', '-Isrc/include',
             '-Isrc/fs', '-Isrc/drivers', '-Isrc/mm', '-Isrc/lib', '-Isrc/kernel',
             '-Isrc/arch/x86_64', '-Isrc/net', '-Iuser/tools', '-Isrc/lib/crypto/upstream/src']
    def run(argv):
        with (out / 'seed.log').open('ab') as log:
            subprocess.run(list(map(str, argv)), cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    objects = []
    for i, name in enumerate(('src/lib/crypto/upstream/src/monocypher.c', 'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
        obj = out / f'mono{i}.o'
        run(flags + ['-include', 'src/lib/crypto/namespace.h', '-c', name, '-o', obj]); objects.append(obj)
    units = ['tests/pkg_template_seed_host.c', 'tests/ext4_fault_disk.c', 'src/fs/permission_values.c',
             'src/kernel/creds.c', 'src/lib/account_redo.c', 'src/fs/pkg_plan.c', 'user/pkg/catalog.c',
             'user/pkg/verify.c', 'user/pkg/format.c', 'user/pkg/manifest.c', 'user/pkg/repository.c',
             'user/tools/gzip.c', 'user/tools/digest.c', 'src/lib/crypto/hash.c', 'src/lib/crypto/curve.c']
    binary = out / 'seed-host'; run(flags + units + objects + ['-o', binary])
    seeded = out / 'seeded-root.img'
    run([binary, templates / 'root.img', artifact, catalog, seeded])
    fixture.run(['e2fsck', '-fn', str(seeded)], out / 'seed-fsck.log')
    for name in ('passwd', 'group', 'shadow', 'pkg/trust.conf', 'pkg/repository.conf'):
        dumped = out / ('preserved-' + name.replace('/', '-'))
        fixture.run(['debugfs', '-R', f'dump /etc/{name} {dumped}', str(seeded)], out / 'seed-audit.log')
        assert dumped.read_bytes() == (templates / 'tree/etc' / name).read_bytes()
    # Exercise the unchanged actual installer expansion on a copy before media
    # publication. This catches padding consumption even on a clean filesystem.
    probe = out / 'expand-probe.img'; shutil.copyfile(seeded, probe)
    expand = out / 'expand-host'
    run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-no-pie', '-Itests/host', '-Isrc/include', 'tests/install_expand_host.c', 'src/lib/install_expand.c', '-o', expand])
    run([expand, probe, str(320 * 1024 * 1024)])
    fixture.run(['e2fsck', '-fn', str(probe)], out / 'expand-fsck.log')
    seeded.replace(templates / 'root.img')
    wire = bytearray((templates / 'payload.v1').read_bytes())
    wire[80:112] = bytes.fromhex(digest(templates / 'root.img'))
    wire[224:] = hashlib.sha256(wire[:224]).digest()
    (templates / 'payload.v1').write_bytes(wire)
    record = json.loads((templates / 'manifest.json').read_text())
    (templates / 'unseeded-manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    record['roles']['root.img']['sha256'] = digest(templates / 'root.img')
    record['package_seed'] = {'name': 'rot13', 'build': 1, 'artifact_sha256': V1_HASH, 'adapter': str(out)}
    (templates / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')


def main():
    parent = ROOT / 'build/rot13-installer'; parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='build-', dir=parent)); out.chmod(0o755)
    print('Build evidence:', out, flush=True)
    now = int(time.time())
    kernel, bootstrap, installer = ROOT / 'bin/fortress.elf', ROOT / 'bin/initramfs.tar', ROOT / 'build/tool-install-multi.elf'
    result = subprocess.run(['python3', 'scripts/create_installer_payload.py', '--kernel', str(kernel), '--bootstrap', str(bootstrap)],
                            cwd=ROOT, capture_output=True, text=True, check=True, timeout=600)
    templates = Path(result.stdout.strip().splitlines()[-1]); templates.chmod(0o755)
    trust = (templates / 'tree/etc/pkg/trust.conf').read_text()
    expected = 'version=1\nrepository=00010203-0405-0607-0809-0a0b0c0d0e0f\nroot_version=1\nplatform=' + '42' * 32 + '\nbase_version=1\npublic_key=' + PUBLIC_KEY + '\n'
    assert trust == expected, 'only the existing public demo repository is supported'
    assert (templates / 'tree/etc/pkg/repository.conf').read_text() == f'version=1\nurl={URL}\n'
    artifact = download(URL + '/objects/sha256/' + V1_HASH + '.fpk', 9 * 1024 * 1024)
    assert hashlib.sha256(artifact).hexdigest() == V1_HASH
    pkg_build.verify(artifact, sanitize=True)
    (out / 'rot13-v1.fpk').write_bytes(artifact)
    # This seed is already publicly known from the repository's demo identity.
    # No production signing key is selected, generated, persisted or provisioned.
    key = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
    assert repo.public(key).hex() == PUBLIC_KEY
    wire, _ = repo.catalog(bytes(range(16)), 1, 1, now - 86400, now + 7 * 86400, [artifact], [key])
    (out / 'catalog-v1.v1').write_bytes(wire)
    current = download(URL + '/catalog.v1', 256 * 1024)
    count, signers = struct.unpack_from('<HH', current, 64)
    body = 72 + 81 * count
    assert len(current) == body + 96 * signers and signers == 1
    assert current[16:32] == bytes(range(16)) and struct.unpack_from('<QQ', current, 32) == (1, 2)
    assert current[body:body + 32] == hashlib.sha256(repo.public(key)).digest()
    key.public_key().verify(current[body + 32:], current[:body])
    assert count == 1 and current[72] == 5 and current[73:78] == b'rot13'
    assert struct.unpack_from('<Q', current, 105)[0] == 2
    (out / 'published-catalog-v2.v1').write_bytes(current)
    seed(templates, out / 'rot13-v1.fpk', out / 'catalog-v1.v1', out, now)
    media = build(templates, kernel, bootstrap, installer, combined=False, publish=False)
    media.chmod(0o755)
    destination = ROOT / 'bin/fortress-rot13-1.0-installer.img'
    assert not destination.exists() and not destination.is_symlink(), 'refusing to replace an existing candidate'
    shutil.copyfile(media / 'fortress-installer.img', destination)
    manifest = {'result': 'BUILD_PASS_RUNTIME_PENDING', 'image': str(destination), 'image_sha256': digest(destination),
                'templates': str(templates), 'media': str(media), 'seed_evidence': str(out),
                'kernel_sha256': digest(kernel), 'pkg_sha256': digest(ROOT / 'build/pkg/pkg.elf'),
                'preinstalled': 'rot13 1.0 / build 1', 'repository': URL,
                'trust': 'existing public demo policy preserved', 'physical_writes': False}
    (ROOT / 'bin/fortress-rot13-1.0-installer.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2), flush=True)


if __name__ == '__main__':
    main()
