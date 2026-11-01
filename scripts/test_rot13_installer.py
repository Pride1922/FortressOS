#!/usr/bin/env python3
"""Install the shipped rot13 USB candidate, then upgrade from the real repository.

BIOS and UEFI; only regular-file USB and NVMe disks. Exercises normal installer
authorization/account personalization, preinstalled 1.0, HTTPS upgrade and reboot.
"""
import hashlib
import json
from pathlib import Path
import tempfile

import test_installer as install
import test_pkg_upgrade_qemu as qemu
import test_root_phase0 as fixture
from test_root_phase2 import extract_root

ROOT = fixture.REPO
PASSWORD = 'rot13-qemu-operator'
RECOVERY = 'rot13-qemu-recovery'


def audit(out, disk, label, build, expected_hash, accounts, expected_pkg=None):
    root = out / (label + '.root.img'); extract_root(disk, root)
    fixture.run(['e2fsck', '-fn', str(root)], out / (label + '.fsck.log'))
    def dump(path, leaf):
        destination = out / (label + '.' + leaf)
        fixture.run(['debugfs', '-R', f'dump {path} {destination}', str(root)], out / (label + '.audit.log'))
        return destination.read_bytes()
    executable = dump('/usr/bin/rot13', 'rot13')
    assert hashlib.sha256(executable).hexdigest() == expected_hash
    record = dump('/var/lib/pkg/install.v2', 'install.v2')
    assert len(record) == 8192 and record[:14] == b'FORT-PKG-EXEC1'
    assert hashlib.sha256(record[:8160]).digest() == record[8160:]
    assert int.from_bytes(record[40:48], 'little') == build
    assert int.from_bytes(record[64:68], 'little') == 0 and int.from_bytes(record[68:72], 'little') == 1
    assert dump('/bin/pkg', 'pkg') == (expected_pkg or ROOT / 'build/pkg/pkg.elf').read_bytes()
    for path, content in accounts.items():
        assert dump(path, Path(path).name) == content
    assert dump('/home/operator/upgrade-note', 'note') == b'KEEP_AFTER_UPGRADE\n'
    return {'result': 'PASS', 'build': build, 'linux_fsck': 'PASS', 'executable_sha256': expected_hash,
            'package_record_sha256': hashlib.sha256(record).hexdigest(), 'accounts_and_settings_preserved': True}


def main():
    record = ROOT / 'bin/fortress-rot13-1.0-installer.manifest.json'
    manifest = json.loads(record.read_text())
    image = Path(manifest['image']); media = Path(manifest['media'])
    assert fixture.digest(image) == manifest['image_sha256']
    assert fixture.digest(media / 'fortress-installer.img') == manifest['image_sha256']
    parent = ROOT / 'build/rot13-installer'
    out = Path(tempfile.mkdtemp(prefix='accept-', dir=parent)); out.chmod(0o755)
    print('Evidence:', out, flush=True)
    result = {'result': 'FAIL', 'scope': 'shipped USB installer -> ordinary installed root -> live HTTPS 2.0 upgrade -> reboot',
              'image_sha256': manifest['image_sha256'], 'cases': [], 'physical_writes': False}
    install.ACCEL = 'kvm'; qemu.PASSWORD = PASSWORD
    # The package builds are deterministic and match the retained server objects.
    hashes = {i: fixture.digest(ROOT / f'build/pkg-rot13/rot13-v{i}.elf') for i in (1, 2)}
    try:
        for firmware in ('bios', 'uefi'):
            case = out / firmware; case.mkdir()
            disk = install.install(case, media, firmware, 384, result, source='usb',
                                   disposable_gate=False, operator=PASSWORD, recovery=RECOVERY)
            install.audit(disk, case, ROOT / 'bin/fortress.elf', PASSWORD, RECOVERY, result)
            executable = case / 'preinstalled-rot13'
            fixture.run(['debugfs', '-R', f'dump /usr/bin/rot13 {executable}', str(case / 'installed-root.img')], case / 'before.log')
            assert fixture.digest(executable) == hashes[1], 'installer did not preserve the exact 1.0 executable'
            # Snapshot the personalized credentials and public repository config
            # before the first package operation, using the independent Linux image.
            accounts = {}
            for path in ('/etc/passwd', '/etc/group', '/etc/shadow', '/etc/pkg/trust.conf', '/etc/pkg/repository.conf'):
                target = case / ('before-' + path.replace('/', '-'))
                fixture.run(['debugfs', '-R', f'dump {path} {target}', str(case / 'installed-root.img')], case / 'before.log')
                accounts[path] = target.read_bytes()
            guest = qemu.Guest(case, disk, firmware, 4, 'installed-upgrade', 'kvm')
            try:
                guest.login()
                assert '\nrot13 1.0\n' in guest.command('/usr/bin/rot13 --version')
                assert '\nuryyb\n' in guest.command('echo hello | /usr/bin/rot13')
                guest.command('echo KEEP_AFTER_UPGRADE > /home/operator/upgrade-note')
                guest.root()
                guest.command('/sbin/ifup 10.0.2.15/24 10.0.2.2 10.0.2.3')
                guest.command('/bin/pkg update')
                guest.command('/bin/pkg get rot13')
                assert 'upgrade complete' in guest.command('/bin/pkg upgrade rot13')
                assert '\nrot13 2.0\n' in guest.command('/usr/bin/rot13 --version')
                assert 'package is up to date rot13 v 2.0' in guest.command('/bin/pkg upgrade rot13')
                assert 'Usage:' in guest.command('/usr/bin/rot13 --help')
                guest.shutdown()
            finally:
                guest.close()
            upgraded = audit(case, disk, 'upgraded', 2, hashes[2], accounts)
            guest = qemu.Guest(case, disk, firmware, 4, 'installed-reboot', 'kvm')
            try:
                guest.login()
                assert '\nrot13 2.0\n' in guest.command('/usr/bin/rot13 --version')
                assert '\nuryyb\n' in guest.command('echo hello | /usr/bin/rot13')
                assert '\nKEEP_AFTER_UPGRADE\n' in guest.command('cat /home/operator/upgrade-note')
                guest.root(); guest.command('/bin/pkg status'); guest.shutdown()
            finally:
                guest.close()
            rebooted = audit(case, disk, 'rebooted', 2, hashes[2], accounts)
            result['cases'].append({'firmware': firmware, 'installed_from_usb': True, 'preinstalled_1_0': True,
                                    'upgrade': upgraded, 'reboot': rebooted, 'result': 'PASS'})
        assert fixture.digest(image) == manifest['image_sha256']
        result['result'] = 'PASS'
        manifest.update(result='PASS', qemu_acceptance=str(out / 'manifest.json'),
                        qemu_scope='BIOS/UEFI USB install, SMP=4 live repository upgrade and reboot')
        record.write_text(json.dumps(manifest, indent=2) + '\n')
        print('PASS actual flashable installer and live-repository upgrade:', out, flush=True)
    except BaseException as error:
        result['error'] = type(error).__name__ + ': ' + str(error)
        raise
    finally:
        (out / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
