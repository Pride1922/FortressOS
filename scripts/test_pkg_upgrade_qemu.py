#!/usr/bin/env python3
"""Real pkg 1.0 -> 2.0 over HTTP on disposable installed-root QEMU disks.

Retains a bootable 1.0 baseline, public test repository, serial transcripts,
QEMU argv and Linux filesystem/hash oracles. Test credentials/trust belong only
to these images. No physical devices or production repository are accessed.
"""
import argparse
import functools
import hashlib
import http.server
import io
import json
from pathlib import Path
import re
import secrets
import selectors
import shutil
import subprocess
import tarfile
import tempfile
import threading
import time
import uuid

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import pkg_build
import pkg_repository
import test_root_phase0 as fixture
from test_root_phase2 import extract_root

ROOT = fixture.REPO
PASSWORD = 'qemu-pkg-test'
PROMPT = re.compile(r'(?:^|\n)(?:operator|root)@[^\n]+[#$] $')


def prepare(workspace, out, trust, port):
    stage = out / 'tree'
    stage.mkdir()
    attrs = {}
    with tarfile.open(workspace / 'bin/initramfs.tar') as archive:
        for member in archive:
            name = Path(member.name)
            assert not name.is_absolute() and '..' not in name.parts
            assert member.isdir() or member.isfile()
            dest = stage / name
            assert dest.resolve().is_relative_to(stage.resolve())
            if member.isdir():
                dest.mkdir(parents=True, exist_ok=True)
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(archive.extractfile(member).read())
            attrs['/' + name.as_posix()] = (member.mode, member.uid, member.gid)
    for name in ('dev', 'run', 'tmp', 'mnt', 'var', 'home/operator', 'root', 'etc/pkg'):
        (stage / name).mkdir(parents=True, exist_ok=True)
    (stage / 'etc/passwd').write_text('root:x:0:0::/root:/bin/shell\noperator:x:1000:1000::/home/operator:/bin/shell\n')
    (stage / 'etc/group').write_text('root:x:0:\noperator:x:1000:\nwheel:x:10:operator\n')
    (stage / 'etc/shadow').write_text('root:!:::::::\noperator:' + fixture.crypt_hash(PASSWORD, '$5$' + secrets.token_hex(8)) + ':::::::\n')
    (stage / 'etc/pkg/trust.conf').write_text(trust)
    (stage / 'etc/pkg/repository.conf').write_text(f'version=1\nurl=http://10.0.2.2:{port}\n')
    attrs.update({'/etc/shadow': (0o600, 0, 0), '/tmp': (0o1777, 0, 0),
                  '/home/operator': (0o700, 1000, 1000), '/root': (0o700, 0, 0),
                  '/etc/pkg/trust.conf': (0o644, 0, 0), '/etc/pkg/repository.conf': (0o644, 0, 0)})
    partuuid, fsuuid = uuid.uuid4(), uuid.uuid4()
    identity = f'install_id={secrets.token_hex(16)}\nroot_partuuid={partuuid}\nfilesystem_uuid={fsuuid}\n'
    (stage / 'etc/fortress-install.v1').write_text('FORTRESS-INSTALL-1\n' + identity + 'payload_version=1\n')
    run = lambda args: fixture.run(args, out / 'prepare.log')
    root = out / 'root.img'
    with root.open('xb') as stream:
        stream.truncate(fixture.ROOT_BYTES)
    run(['mke2fs', '-q', '-t', 'ext4', '-b', '4096', '-I', '256', '-U', str(fsuuid),
         '-O', fixture.FEATURES, '-E', 'lazy_itable_init=0', '-m', '0', '-g', '4096',
         '-N', '4096', '-J', 'size=4', '-d', str(stage), str(root)])
    commands = ['set_inode_field / mode 040755', 'set_inode_field / uid 0', 'set_inode_field / gid 0']
    for path in sorted(stage.rglob('*')):
        name = '/' + path.relative_to(stage).as_posix()
        mode, uid, gid = attrs.get(name, (0o755 if path.is_dir() else 0o644, 0, 0))
        commands += [f'set_inode_field {name} mode 0{mode | (0o40000 if path.is_dir() else 0o100000):o}',
                     f'set_inode_field {name} uid {uid}', f'set_inode_field {name} gid {gid}']
    attrs_file = out / 'attrs.commands'
    attrs_file.write_text('\n'.join(commands) + '\n')
    run(['debugfs', '-w', '-f', str(attrs_file), str(root)])
    fixture.initialize_journal(root, out)
    run(['e2fsck', '-fn', str(root)])
    bootstrap = out / 'bootstrap.tar'
    with tarfile.open(workspace / 'bin/initramfs.tar') as src, tarfile.open(bootstrap, 'w', format=tarfile.USTAR_FORMAT) as dst:
        for member in src:
            dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        member = tarfile.TarInfo('bootstrap'); member.type = tarfile.DIRTYPE; member.mode = 0o700
        dst.addfile(member)
        data = ('FORTRESS-RECOVERY-1\n' + identity + 'password_hash=' + fixture.crypt_hash(secrets.token_hex(16), '$5$rounds=100000$' + secrets.token_hex(8)) + '\n').encode()
        member = tarfile.TarInfo('bootstrap/recovery.v1'); member.mode = 0o600; member.size = len(data)
        dst.addfile(member, io.BytesIO(data))
    kernel = workspace / 'bin/fortress.elf'
    ready = ('FORTRESS-READY-1\n' + identity + 'payload_version=1\nkernel_sha256=' + fixture.digest(kernel) + '\nbootstrap_sha256=' + fixture.digest(bootstrap) + '\n').encode()
    ready += ('checksum=' + hashlib.sha256(ready).hexdigest() + '\n').encode()
    assert len(ready) < 512
    readiness = out / 'readiness.v1'; readiness.write_bytes(ready.ljust(512, b'\0'))
    config = out / 'limine.conf'
    config.write_text('timeout: 0\n/FortressOS rot13 upgrade test\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    kernel_cmdline: boot_mode=installed root=PARTUUID=' + str(partuuid) + ' net=10.0.2.15/24,10.0.2.2\n    module_path: boot():/boot/bootstrap.tar\n    module_cmdline: role=bootstrap\n    module_path: boot():/boot/readiness.v1\n    module_cmdline: role=readiness\n')
    esp = out / 'esp.img'
    fixture.create_esp_partition(esp, None, workspace / 'limine', kernel, bootstrap, config, None)
    run(['mren', '-i', str(esp), '::/boot/initramfs.tar', 'bootstrap.tar'])
    run(['mcopy', '-i', str(esp), str(readiness), '::/boot/readiness.v1'])
    # assemble uses the pinned Limine utility at fixture.REPO, without touching devices.
    disk, _ = fixture.assemble(out, esp, root, 384, partuuid)
    return disk


class Guest:
    def __init__(self, out, disk, firmware, cpus, label, accel):
        assert disk.is_file() and not disk.is_symlink() and disk.resolve().is_relative_to(out.resolve())
        self.out, self.label = out, label
        argv = ['qemu-system-x86_64', '-accel', accel, '-M', 'q35', '-cpu', 'host' if accel == 'kvm' else 'max',
                '-m', '1024M', '-smp', str(cpus), '-display', 'none', '-monitor', 'none', '-no-reboot',
                '-rtc', 'base=2026-10-31T12:00:00', '-serial', 'stdio',
                '-drive', f'file={disk},if=none,id=target,format=raw', '-device', 'nvme,drive=target,serial=PKGUPGRADE,bootindex=1',
                '-netdev', 'user,id=net0', '-device', 'e1000,netdev=net0']
        if firmware == 'uefi':
            variables = out / (label + '.vars.fd')
            shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
            argv += ['-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                     '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
        assert not any('/dev/' in item for item in argv)
        assert '-cdrom' not in argv and '-blockdev' not in argv and argv.count('-device') == 2
        assert [argv[i + 1] for i, x in enumerate(argv) if x == '-drive' and not argv[i + 1].startswith('if=pflash,')] == [f'file={disk},if=none,id=target,format=raw']
        (out / (label + '.argv.json')).write_text(json.dumps(argv, indent=2) + '\n')
        self.raw = bytearray(); self.counter = 0; self.actions = []
        self.error = (out / (label + '.stderr.log')).open('wb')
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.error)
        self.selector = selectors.DefaultSelector(); self.selector.register(self.process.stdout, selectors.EVENT_READ)

    def text(self, start=0):
        return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', self.raw[start:].decode(errors='replace')).replace('\r', '')

    def wait(self, predicate, start=0, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for key, _ in self.selector.select(.1):
                self.raw.extend(key.fileobj.read1(65536))
            (self.out / (self.label + '.serial.log')).write_bytes(self.raw)
            text = self.text()
            assert not any(x in text for x in ('KERNEL PANIC', '[FATAL]', '[PROCESS FAULT]')), text[-5000:]
            if predicate(self.text(start)):
                return self.text(start)
            assert self.process.poll() is None, text[-5000:]
        raise TimeoutError(self.label + ': ' + self.text()[-5000:])

    def send(self, line):
        self.process.stdin.write((line + '\n').encode()); self.process.stdin.flush()

    def command(self, command, expected=0):
        self.counter += 1
        marker = f'PKGQ_{self.counter}'
        start = len(self.raw)
        self.send(command + '; echo ' + marker + ':$?')
        text = self.wait(lambda t: re.search(r'(?m)^' + marker + r':\d+\s*$', t) and PROMPT.search(t), start)
        code = int(re.search(r'(?m)^' + marker + r':(\d+)\s*$', text)[1])
        self.actions.append({'command': command, 'exit': code})
        assert code == expected, (command, expected, code, text)
        print(self.label, command, 'PASS', flush=True)
        return text

    def login(self):
        self.wait(lambda t: 'FortressOS login: ' in t)
        assert '[ROOT] Installed root admitted by PARTUUID' in self.text(), self.text()[-5000:]
        self.send('operator'); self.wait(lambda t: t.endswith('Password: '))
        self.send(PASSWORD); self.wait(lambda t: PROMPT.search(t))

    def root(self):
        start = len(self.raw); self.send('sudo /bin/shell')
        self.wait(lambda t: t.endswith('sudo password: '), start)
        self.send(PASSWORD); self.wait(lambda t: re.search(r'(?:^|\n)root@[^\n]+# $', t), start)

    def shutdown(self):
        start = len(self.raw); self.send('/sbin/shutdown')
        self.wait(lambda t: '[POWER] Shutdown initiated' in t, start)
        assert self.process.wait(timeout=60) == 0

    def close(self):
        if self.process.poll() is None:
            self.process.kill(); self.process.wait(timeout=10)
        self.selector.close(); self.error.close()
        (self.out / (self.label + '.serial.log')).write_bytes(self.raw)
        (self.out / (self.label + '.actions.json')).write_text(json.dumps(self.actions, indent=2) + '\n')


def audit(out, disk, label, release, workspace):
    root = out / (label + '.root.img'); extract_root(disk, root)
    fixture.run(['e2fsck', '-fn', str(root)], out / (label + '.fsck.log'))
    def dump(path, name):
        dest = out / (label + '.' + name)
        fixture.run(['debugfs', '-R', f'dump {path} {dest}', str(root)], out / (label + '.audit.log'))
        return dest.read_bytes()
    actual = dump('/usr/bin/rot13', 'rot13')
    expected = (workspace / f'build/pkg-rot13/rot13-v{release}.elf').read_bytes()
    assert actual == expected, 'executable differs byte-for-byte'
    database = dump('/var/lib/pkg/install.v2', 'install.v2')
    assert len(database) == 8192 and database[:14] == b'FORT-PKG-EXEC1'
    assert hashlib.sha256(database[:8160]).digest() == database[8160:]
    assert int.from_bytes(database[40:48], 'little') == release
    assert int.from_bytes(database[64:68], 'little') == 0
    assert int.from_bytes(database[68:72], 'little') == 1
    assert dump('/home/operator/pkg-note', 'note') == b'PRESERVE_PACKAGE_UPGRADE\n'
    assert dump('/bin/pkg', 'pkg.elf') == (workspace / 'build/pkg/pkg.elf').read_bytes()
    for name in ('passwd', 'group', 'shadow', 'pkg/trust.conf', 'pkg/repository.conf'):
        assert dump('/etc/' + name, name.replace('/', '-')) == (out / 'tree/etc' / name).read_bytes()
    return {'result': 'PASS', 'build': release, 'executable_sha256': hashlib.sha256(actual).hexdigest(),
            'inventory_sha256': hashlib.sha256(database).hexdigest(), 'linux_fsck': 'PASS', 'settings_preserved': True}


def boot_test(out, disk, firmware, cpus, label, accel, mode):
    guest = Guest(out, disk, firmware, cpus, label, accel)
    try:
        guest.login()
        if mode != 'install':
            version = '1.0' if mode == 'upgrade' else '2.0'
            assert f'\nrot13 {version}\n' in guest.command('/usr/bin/rot13 --version')
            assert '\nuryyb\n' in guest.command('echo hello | /usr/bin/rot13')
            assert '\nPRESERVE_PACKAGE_UPGRADE\n' in guest.command('cat /home/operator/pkg-note')
        guest.root()
        if mode == 'install':
            guest.command('echo PRESERVE_PACKAGE_UPGRADE > /home/operator/pkg-note')
            guest.command('/bin/pkg update')
            guest.command('/bin/pkg get rot13')
            assert 'installation complete' in guest.command('/bin/pkg install rot13')
            assert '\nrot13 1.0\n' in guest.command('/usr/bin/rot13 --version')
        elif mode == 'upgrade':
            guest.command('/bin/pkg update')
            guest.command('/bin/pkg get rot13')
            assert 'upgrade complete' in guest.command('/bin/pkg upgrade rot13')
            assert '\nrot13 2.0\n' in guest.command('/usr/bin/rot13 --version')
            assert 'package is up to date rot13 v 2.0' in guest.command('/bin/pkg upgrade rot13')
            assert 'Usage:' in guest.command('/usr/bin/rot13 --help')
        guest.command('/bin/pkg status')
        assert '\nuryyb\n' in guest.command('echo hello | /usr/bin/rot13')
        guest.shutdown()
    finally:
        guest.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=ROOT)
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--firmware', choices=['bios', 'uefi'], nargs='+', default=['bios', 'uefi'])
    parser.add_argument('--accel', choices=['tcg', 'kvm'], default='tcg')
    parser.add_argument('--port', type=int, default=18080)
    args = parser.parse_args()
    assert set(args.cpus) <= {1, 4, 8} and 1024 <= args.port <= 65535
    workspace = args.workspace.resolve()
    for name in ('bin/fortress.elf', 'bin/initramfs.tar', 'limine/limine'):
        assert (workspace / name).is_file(), f'build first: make -j4 bin/fortress.elf bin/initramfs.tar limine-setup ({name})'
    parent = ROOT / 'build/pkg-upgrade-qemu'; parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='gate-', dir=parent))
    out.chmod(0o755)
    print('Evidence:', out, flush=True)
    manifest = {'result': 'FAIL', 'scope': 'QEMU ordinary installed-root application upgrade, no physical acceptance',
                'source_commit': subprocess.check_output(['git', '-c', 'safe.directory=' + str(ROOT), 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                'accel': args.accel, 'cases': [], 'kernel_sha256': fixture.digest(workspace / 'bin/fortress.elf'),
                'pkg_sha256': fixture.digest(workspace / 'build/pkg/pkg.elf'),
                'runner_sha256': fixture.digest(Path(__file__))}
    # Ephemeral test signer is never written to the image or public tree.
    key = Ed25519PrivateKey.generate()
    repository = bytes(range(16))
    trust = 'version=1\nrepository=' + str(uuid.UUID(bytes=repository)) + '\nroot_version=1\nplatform=' + '42' * 32 + '\nbase_version=1\npublic_key=' + pkg_repository.public(key).hex() + '\n'
    subprocess.run(['sh', 'examples/pkg/rot13/build.sh'], cwd=workspace, check=True)
    public = out / 'repository'; objects = public / 'objects/sha256'; objects.mkdir(parents=True)
    catalogs = []
    now = int(time.time())
    for release in (1, 2):
        artifact = pkg_build.build_spec(workspace / f'examples/pkg/rot13/v{release}.json')
        (objects / (hashlib.sha256(artifact).hexdigest() + '.fpk')).write_bytes(artifact)
        catalog, _ = pkg_repository.catalog(repository, 1, release, min(now, 1793448000) - 86400,
                                            max(now, 1793448000) + 30 * 86400, [artifact], [key])
        (out / f'catalog-v{release}.v1').write_bytes(catalog); catalogs.append(catalog)
    (public / 'catalog.v1').write_bytes(catalogs[0])
    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, format, *values):
            with (out / 'http.log').open('a') as stream:
                stream.write(format % values + '\n')
    server = http.server.ThreadingHTTPServer(('127.0.0.1', args.port), functools.partial(Handler, directory=str(public)))
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    try:
        disk = prepare(workspace, out, trust, args.port)
        baseline = out / 'fortress-rot13-1.0.img'; disk.rename(baseline)
        boot_test(out, baseline, 'bios', 1, 'install-v1', args.accel, 'install')
        manifest['baseline_oracle'] = audit(out, baseline, 'install-v1', 1, workspace)
        manifest['baseline_sha256'] = fixture.digest(baseline)
        # Keep the installed image immutable; every update begins from its own copy.
        (public / '.catalog.new').write_bytes(catalogs[1]); (public / '.catalog.new').replace(public / 'catalog.v1')
        (out / 'README.txt').write_text(f'TEST IMAGE ONLY. Login operator / {PASSWORD}\n'
            f'Baseline: {baseline.name}, rot13 1.0 installed through the real pkg CLI.\n'
            f'Serve repository/: python3 -m http.server {args.port} --bind 127.0.0.1 --directory {public}\n'
            'Attach a COPY of the baseline as NVMe to QEMU with user networking and e1000.\n'
            'Then: pkg update; pkg get rot13; sudo pkg upgrade rot13\n'
            'Verify: /usr/bin/rot13 --version; echo hello | /usr/bin/rot13\n'
            'Repository contains signed 2.0 and retains the 1.0 object. Test public trust only.\n')
        for firmware in args.firmware:
            for cpus in args.cpus:
                label = f'{firmware}-smp{cpus}'
                copy = out / (label + '.img'); shutil.copyfile(baseline, copy)
                boot_test(out, copy, firmware, cpus, label + '-upgrade', args.accel, 'upgrade')
                oracle = audit(out, copy, label + '-upgrade', 2, workspace)
                boot_test(out, copy, firmware, cpus, label + '-reboot', args.accel, 'reboot')
                reboot_oracle = audit(out, copy, label + '-reboot', 2, workspace)
                manifest['cases'].append({'firmware': firmware, 'cpus': cpus, 'result': 'PASS', 'upgrade': oracle, 'reboot': reboot_oracle})
                (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        assert fixture.digest(baseline) == manifest['baseline_sha256'], 'baseline image changed'
        manifest['result'] = 'PASS'
        print('PASS QEMU package upgrade and reboot:', out, flush=True)
    except BaseException as error:
        manifest['error'] = type(error).__name__ + ': ' + str(error)
        raise
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=10)
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
