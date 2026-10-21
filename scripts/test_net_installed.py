#!/usr/bin/env python3
"""Installed ifup persistence on fresh regular-file NVMe fixtures only.

Builds an isolated source snapshot, or accepts one with --workspace.
No shared ISO/image modification, installer invocation or physical I/O.
"""
import argparse
import hashlib
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
import time
import uuid

import test_root_phase0 as fixture
from test_root_phase2 import extract_root


def prepare(workspace, out, password):
    stage = out / 'tree'
    stage.mkdir()
    attrs = {}
    with tarfile.open(workspace / 'bin/initramfs.tar') as src:
        for m in src:
            name = Path(m.name)
            assert not name.is_absolute() and '..' not in name.parts
            assert m.isfile() or m.isdir()
            target = stage / name
            assert target.resolve().is_relative_to(stage.resolve())
            if m.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(src.extractfile(m).read())
            attrs['/' + name.as_posix()] = (m.mode, m.uid, m.gid)
    for name in ('dev', 'run', 'tmp', 'mnt', 'var', 'home', 'home/operator', 'root'):
        (stage / name).mkdir(parents=True, exist_ok=True)
    (stage / 'etc/passwd').write_text('root:x:0:0::/root:/bin/shell\noperator:x:1000:1000::/home/operator:/bin/shell\n')
    (stage / 'etc/group').write_text('root:x:0:\noperator:x:1000:\nwheel:x:10:operator\n')
    (stage / 'etc/shadow').write_text('root:!:::::::\noperator:' + fixture.crypt_hash(password, '$5$' + secrets.token_hex(8)) + ':::::::\n')
    attrs.update({'/etc/shadow': (0o600, 0, 0), '/tmp': (0o1777, 0, 0),
                  '/home/operator': (0o700, 1000, 1000), '/root': (0o700, 0, 0)})
    partuuid, fsuuid = uuid.uuid4(), uuid.uuid4()
    identity = f'install_id={secrets.token_hex(16)}\nroot_partuuid={partuuid}\nfilesystem_uuid={fsuuid}\n'
    (stage / 'etc/fortress-install.v1').write_text('FORTRESS-INSTALL-1\n' + identity + 'payload_version=1\n')
    run = lambda args: fixture.run(args, out / 'prepare.log')
    run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-mno-red-zone',
         '-mgeneral-regs-only', '-fno-pie', '-fno-stack-protector', '-Isrc/include',
         '-c', 'tests/net_cpu_probe_user.c', '-o', str(out / 'cpu-probe.o')])
    run(['nasm', '-f', 'elf64', '-DTOOL_ENTRY=net_cpu_probe_main', 'user/tools/start.asm', '-o', str(out / 'cpu-start.o')])
    run(['ld', '-m', 'elf_x86_64', '-nostdlib', '-static', '-z', 'noexecstack', '-T', 'user/shell.ld',
         str(out / 'cpu-start.o'), str(out / 'cpu-probe.o'), '-o', str(stage / 'bin/net-cpu-probe')])
    attrs['/bin/net-cpu-probe'] = (0o755, 0, 0)
    root = out / 'root.img'
    with root.open('xb') as f:
        f.truncate(fixture.ROOT_BYTES)
    run(['mke2fs', '-q', '-t', 'ext4', '-b', '4096', '-I', '256', '-U', str(fsuuid),
         '-O', fixture.FEATURES, '-E', 'lazy_itable_init=0', '-m', '0', '-g', '4096',
         '-N', '4096', '-J', 'size=4', '-d', str(stage), str(root)])
    commands = ['set_inode_field / mode 040755', 'set_inode_field / uid 0', 'set_inode_field / gid 0']
    for p in sorted(stage.rglob('*')):
        name = '/' + p.relative_to(stage).as_posix()
        mode, uid, gid = attrs.get(name, (0o755 if p.is_dir() else 0o644, 0, 0))
        commands += [f'set_inode_field {name} mode 0{mode | (0o40000 if p.is_dir() else 0o100000):o}',
                     f'set_inode_field {name} uid {uid}', f'set_inode_field {name} gid {gid}']
    command_file = out / 'attrs.commands'
    command_file.write_text('\n'.join(commands) + '\n')
    run(['debugfs', '-w', '-f', str(command_file), str(root)])
    fixture.initialize_journal(root, out)
    run(['e2fsck', '-fn', str(root)])
    bootstrap = out / 'bootstrap.tar'
    with tarfile.open(workspace / 'bin/initramfs.tar') as src, tarfile.open(bootstrap, 'w', format=tarfile.USTAR_FORMAT) as dst:
        for m in src:
            dst.addfile(m, src.extractfile(m) if m.isfile() else None)
        m = tarfile.TarInfo('bootstrap'); m.type = tarfile.DIRTYPE; m.mode = 0o700; dst.addfile(m)
        data = ('FORTRESS-RECOVERY-1\n' + identity + 'password_hash=' + fixture.crypt_hash(secrets.token_hex(16), '$5$rounds=100000$' + secrets.token_hex(8)) + '\n').encode()
        m = tarfile.TarInfo('bootstrap/recovery.v1'); m.mode = 0o600; m.size = len(data); dst.addfile(m, io.BytesIO(data))
    kernel = workspace / 'bin/fortress.elf'
    ready = ('FORTRESS-READY-1\n' + identity + 'payload_version=1\nkernel_sha256=' + fixture.digest(kernel) + '\nbootstrap_sha256=' + fixture.digest(bootstrap) + '\n').encode()
    ready += ('checksum=' + hashlib.sha256(ready).hexdigest() + '\n').encode()
    assert len(ready) < 512
    readiness = out / 'readiness.v1'; readiness.write_bytes(ready.ljust(512, b'\0'))
    config = out / 'limine.conf'
    config.write_text('timeout: 0\n/FortressOS network fixture\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    kernel_cmdline: boot_mode=installed root=PARTUUID=' + str(partuuid) + ' net=10.0.2.15/24,10.0.2.2\n    module_path: boot():/boot/bootstrap.tar\n    module_cmdline: role=bootstrap\n    module_path: boot():/boot/readiness.v1\n    module_cmdline: role=readiness\n')
    esp = out / 'esp.img'
    fixture.create_esp_partition(esp, None, fixture.REPO / 'limine', kernel, bootstrap, config, None)
    run(['mren', '-i', str(esp), '::/boot/initramfs.tar', 'bootstrap.tar'])
    run(['mcopy', '-i', str(esp), str(readiness), '::/boot/readiness.v1'])
    disk, _ = fixture.assemble(out, esp, root, 384, partuuid)
    return disk


def boot(out, disk, firmware, cpus, restart, password, peer_port):
    label = f'{firmware}-smp{cpus}-boot{restart}'
    assert disk.is_file() and disk.resolve().parent == out.resolve()
    argv = ['qemu-system-x86_64', '-accel', 'tcg', '-M', 'q35', '-m', '512M', '-smp', str(cpus),
            '-display', 'none', '-monitor', 'none', '-no-reboot', '-serial', 'stdio',
            '-drive', f'file={disk},if=none,id=target,format=raw', '-device', 'nvme,drive=target,serial=NETINSTALLED,bootindex=1',
            '-netdev', 'user,id=net0', '-device', 'e1000,netdev=net0']
    if firmware == 'uefi':
        variables = out / (label + '.vars.fd')
        shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
        argv += ['-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                 '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    assert all('/dev/' not in a for a in argv)
    assert argv.count('-device') == 2 and '-cdrom' not in argv and '-blockdev' not in argv
    (out / (label + '.argv.json')).write_text(json.dumps(argv, indent=2))
    transcript = bytearray()
    err = (out / (label + '.stderr')).open('wb')
    proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err)
    sel = selectors.DefaultSelector(); sel.register(proc.stdout, selectors.EVENT_READ)
    def text(start=0):
        return re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', transcript[start:].decode(errors='replace')).replace('\r', '')
    def wait(predicate, start=0, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for key, _ in sel.select(.1):
                transcript.extend(key.fileobj.read1(65536))
            (out / (label + '.serial.log')).write_bytes(transcript)
            assert 'KERNEL PANIC' not in text() and '[FATAL]' not in text() and '[PROCESS FAULT]' not in text(), text()[-3000:]
            if predicate(text(start)): return
            assert proc.poll() is None, text()[-3000:]
        raise TimeoutError(text()[-3000:])
    def send(s):
        proc.stdin.write((s + '\n').encode()); proc.stdin.flush()
    prompt = lambda t: re.search(r'(?:^|\n)operator@[^\n]+\$ $', t)
    def command(cmd, expected, sudo=False):
        start = len(transcript); send(cmd)
        if sudo:
            wait(lambda t: t.endswith('sudo password: '), start); send(password)
        wait(lambda t: expected in t and prompt(t), start)
        result = text(start)
        assert 'operation not supported on this CPU' not in result, result
        return result
    try:
        wait(lambda t: 'FortressOS login: ' in t)
        assert '[ROOT] Installed root admitted by PARTUUID' in text()
        send('operator'); wait(lambda t: t.endswith('Password: ')); send(password); wait(prompt)
        command('/sbin/ifconfig', 'inet 10.0.2.15/24')
        command('net-cpu-probe', 'NET CPU BSP accepted PASS')
        if cpus > 1:
            command('net-cpu-probe --worker 1', 'NET CPU AP clients accepted; listener validation PASS')
        config = 'address 10.0.2.50/24\ngateway 10.0.2.2\ndns 1.1.1.1\ndns2 8.8.8.8\n'
        if not restart:
            command('/sbin/ifup', 'cannot open file "/etc/network/network.conf"')
            command('/sbin/ifup 10.0.2.50/24 10.0.2.2', 'permission denied; use sudo ifup')
            command('sudo ifup 10.0.2.50/24 10.0.2.2 1.1.1.1 8.8.8.8', 'gateway 10.0.2.2 applied', sudo=True)
        else:
            command('cat /etc/network/network.conf', config)
            command('sudo ifup', 'address 10.0.2.50/24 gateway 10.0.2.2 applied', sudo=True)
        command('cat /etc/network/network.conf', config)
        command('/sbin/ifconfig', 'dns 1.1.1.1, 8.8.8.8')
        command('ping -c 1 10.0.2.2', 'bytes from 10.0.2.2')
        command('/sbin/ifup --dry-run 10.0.2.60/24 10.0.2.2', 'valid (dry-run)')
        command('/sbin/ifconfig | head -n 2', 'inet 10.0.2.50/24')
        command('sudo ifup 10.0.2.300/24 10.0.2.2', 'invalid', sudo=True)
        command('cat /etc/network/network.conf', config)
        # Success of an ordinary HTTP socket command after the CPU placement fix.
        command(f'wget -q -O - http://10.0.2.2:{peer_port}/network-proof', 'network-socket-proof')
        start = len(transcript); send('sudo shutdown')
        wait(lambda t: t.endswith('sudo password: '), start); send(password)
        wait(lambda t: '[POWER] Shutdown initiated' in t, start)
        assert proc.wait(timeout=20) == 0
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
        sel.close(); err.close()
        (out / (label + '.serial.log')).write_bytes(transcript)
    root = out / (label + '.root.img'); extract_root(disk, root)
    fixture.run(['e2fsck', '-fn', str(root)], out / (label + '.fsck.log'))
    for path, expected in [('/etc/network/network.conf', config.encode()),
                           ('/etc/network/resolv.conf', b'nameserver 1.1.1.1\nnameserver 8.8.8.8\n')]:
        target = out / (label + '.' + Path(path).name)
        fixture.run(['debugfs', '-R', f'dump {path} {target}', str(root)], out / (label + '.audit.log'))
        assert target.read_bytes() == expected, path
        metadata = fixture.run(['debugfs', '-R', f'stat {path}', str(root)], out / (label + '.audit.log'))
        assert 'User:     0' in metadata and 'Group:     0' in metadata and '0644' in metadata, metadata
    print('PASS', label, flush=True)
    return {'case': label, 'result': 'PASS'}


def main():
    import http.server
    import threading
    parser = argparse.ArgumentParser()
    parser.add_argument('--workspace', type=Path)
    parser.add_argument('--cpus', nargs='+', type=int, default=[1, 4])
    args = parser.parse_args()
    assert set(args.cpus) <= {1, 4, 8}
    parent = fixture.REPO / 'build/net-installed'; parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='gate-', dir=parent))
    if args.workspace:
        workspace = args.workspace.resolve()
    else:
        workspace = Path(subprocess.check_output(['python3', 'scripts/create_ext4_guest_workspace.py'], cwd=fixture.REPO, text=True).strip())
        # Real AP entry for this disposable probe; production spawn remains pinned.
        from test_network_smp_phase0 import instrument
        instrument(workspace)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['make', '-j4', 'bin/fortress.elf', 'bin/initramfs.tar'], cwd=workspace,
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    assert workspace.is_relative_to(fixture.REPO / '.codex-remote-attachments')
    manifest = {'scope': 'installed NVMe regular-file ifup/sudo/persistence, BIOS/UEFI; no physical claim', 'result': 'FAIL', 'cases': []}
    class Peer(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            data = b'network-socket-proof\n'
            self.send_response(200); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
        def log_message(self, *args): pass
    peer = http.server.HTTPServer(('127.0.0.1', 0), Peer)
    thread = threading.Thread(target=peer.serve_forever, daemon=True); thread.start()
    try:
        password = secrets.token_hex(16)
        baseline = prepare(workspace, out, password)
        before = fixture.digest(baseline)
        for firmware in ('bios', 'uefi'):
            for cpus in args.cpus:
                disk = out / f'{firmware}-smp{cpus}.img'
                fixture.run(['cp', '--sparse=always', str(baseline), str(disk)], out / 'copy.log')
                for restart in (0, 1):
                    manifest['cases'].append(boot(out, disk, firmware, cpus, restart, password, peer.server_port))
        assert fixture.digest(baseline) == before
        manifest.update(result='PASS', baseline_sha256=before)
    finally:
        peer.shutdown(); peer.server_close(); thread.join(timeout=10)
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(out, flush=True)


if __name__ == '__main__': main()
