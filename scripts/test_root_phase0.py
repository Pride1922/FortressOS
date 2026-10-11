#!/usr/bin/env python3
"""Phase 0 Linux fixture audit and BIOS/UEFI NVMe loader smoke.

Only creates fresh regular files below build/root-phase0. No device paths, mounts,
installed-root implementation, installer or physical-storage claim.
"""
import argparse
import binascii
import ctypes
import ctypes.util
import hashlib
import io
import json
from pathlib import Path
import re
import secrets
import selectors
import shutil
import struct
import subprocess
import tarfile
import tempfile
import time
import uuid

from create_boot_img import create_esp_partition, GUID_ESP, GUID_LINUX_FS
from initialize_ext4_journal import crc32c

REPO = Path(__file__).resolve().parents[1]
FEATURES = 'none,extent,filetype,sparse_super,large_file,metadata_csum,has_journal'
MIB = 1024 * 1024
ROOT_BYTES = 256 * MIB
ESP_START, ESP_SECTORS = 4096, 131072
ROOT_START = ESP_START + ESP_SECTORS
COMMANDS = []
LIBCRYPT = ctypes.CDLL(ctypes.util.find_library('crypt'))
LIBCRYPT.crypt.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
LIBCRYPT.crypt.restype = ctypes.c_char_p


def crypt_hash(secret, setting):
    value = LIBCRYPT.crypt(secret.encode('ascii'), setting.encode('ascii'))
    assert value and value.startswith(b'$5$'), 'system libcrypt rejected setting'
    return value.decode('ascii')


def run(argv, log, timeout=120):
    COMMANDS.append([str(x) for x in argv])
    result = subprocess.run(argv, cwd=REPO, capture_output=True, text=True, timeout=timeout)
    log.write_text(log.read_text() + result.stdout + result.stderr if log.exists()
                   else result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{argv[0]} failed ({result.returncode}); see {log}')
    return result.stdout + result.stderr


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sparse_copy(source, dest, offset):
    with source.open('rb') as stream:
        pos = offset
        while chunk := stream.read(MIB):
            if any(chunk):
                dest.seek(pos)
                dest.write(chunk)
            pos += len(chunk)


def initialize_journal(image, out):
    cmds = out / 'journal-init.commands'
    cmds.write_text('journal_open -c -v 3\njournal_close\n')
    run(['debugfs', '-w', '-f', str(cmds), str(image)], out / 'journal.log')
    with image.open('r+b') as stream:
        stream.seek(1024)
        sb = bytearray(stream.read(1024))
        ino = struct.unpack_from('<I', sb, 224)[0]
        text = run(['debugfs', '-R', f'blocks <{ino}>', str(image)], out / 'journal.log')
        # debugfs version diagnostics are on stderr; only the numerical line is a map.
        line = next(x for x in text.splitlines() if re.fullmatch(r'[0-9 ]+', x))
        blocks = [int(x) for x in line.split()]
        assert len(blocks) >= 1024
        stream.seek(blocks[0] * 4096)
        js = bytearray(stream.read(1024))
        assert struct.unpack_from('>II', js) == (0xc03b3998, 4)
        for where, value in ((24, 1), (28, 0), (40, 0x11), (88, 1), (252, 0)):
            struct.pack_into('>I', js, where, value)
        struct.pack_into('>I', js, 252, crc32c(js))
        stream.seek(blocks[0] * 4096)
        stream.write(js)
        struct.pack_into('<I', sb, 96, 0x42)
        struct.pack_into('<H', sb, 58, 1)
        struct.pack_into('<I', sb, 1020, crc32c(sb[:1020]))
        stream.seek(1024)
        stream.write(sb)
    return {'journal_blocks': len(blocks), 'journal_incompat': 0x11}


def assemble(out, esp, root, capacity_mib, root_guid=None):
    disk = out / f'nvme-{capacity_mib}.img'
    sectors = capacity_mib * MIB // 512
    assert ROOT_START + ROOT_BYTES // 512 <= sectors - 33
    dg, eg, rg = [uuid.uuid4() for _ in range(3)]
    if root_guid is not None: rg = uuid.UUID(str(root_guid))
    entries = bytearray(16384)
    for index, (kind, guid, start, count, label) in enumerate([
            (GUID_ESP, eg, ESP_START, ESP_SECTORS, 'Fortress ESP'),
            (GUID_LINUX_FS, rg, ROOT_START, ROOT_BYTES // 512, 'Fortress Root'),
            (uuid.UUID('21686148-6449-6e6f-744e-656564454649').bytes_le,
             uuid.uuid4(), 2048, 2048, 'Fortress BIOS')]):
        struct.pack_into('<16s16sQQQ72s', entries, index * 128, kind, guid.bytes_le,
                         start, start + count - 1, 0, label.encode('utf-16le').ljust(72, b'\0'))
    array_crc = binascii.crc32(entries)

    def header(current, backup, array):
        data = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000,
                         92, 0, 0, current, backup, 34, sectors - 34,
                         dg.bytes_le, array, 128, 128, array_crc))
        struct.pack_into('<I', data, 16, binascii.crc32(data))
        return data.ljust(512, b'\0')

    with disk.open('xb') as stream:
        stream.truncate(sectors * 512)
        mbr = bytearray(512)
        struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\0\x02\0', 0xee,
                         b'\xff\xff\xff', 1, min(sectors - 1, 0xffffffff))
        mbr[510:] = b'\x55\xaa'
        for offset, data in [(0, mbr), (512, header(1, sectors - 1, 2)),
                             (1024, entries), ((sectors - 33) * 512, entries),
                             ((sectors - 1) * 512, header(sectors - 1, 1, sectors - 33))]:
            stream.seek(offset)
            stream.write(data)
        sparse_copy(esp, stream, ESP_START * 512)
        sparse_copy(root, stream, ROOT_START * 512)
    # Explicit partition placement avoids Limine's default GPT-array embedding.
    # A dedicated third GPT partition preserves the full partition arrays.
    # Phase 0 uses the pinned host utility; native deployment is later work.
    run([str(REPO / 'limine/limine'), 'bios-install', str(disk), '3'], out / f'bios-{capacity_mib}.log')
    with disk.open('rb') as stream:
        headers = []
        tables = []
        for lba in (1, sectors - 1):
            stream.seek(lba * 512)
            data = bytearray(stream.read(512))
            size, crc = struct.unpack_from('<II', data, 12)
            struct.pack_into('<I', data, 16, 0)
            assert binascii.crc32(data[:size]) == crc
            array_lba, count, stride, expected = struct.unpack_from('<QIII', data, 72)
            stream.seek(array_lba * 512)
            table = stream.read(count * stride)
            assert binascii.crc32(table) == expected
            tables.append(table)
            headers.append({'lba': lba, 'entries': count, 'array_lba': array_lba})
        assert headers[0]['entries'] == headers[1]['entries']
        assert headers[0]['entries'] == 128 and tables[0] == tables[1]
        for source, start in ((esp, ESP_START), (root, ROOT_START)):
            stream.seek(start * 512)
            h = hashlib.sha256()
            remaining = source.stat().st_size
            while remaining:
                chunk = stream.read(min(MIB, remaining))
                assert chunk
                h.update(chunk)
                remaining -= len(chunk)
            assert h.hexdigest() == digest(source), 'BIOS deployment damaged partition bytes'
    return disk, {'capacity_mib': capacity_mib, 'root_partuuid': str(rg),
                  'disk_guid': str(dg), 'esp_partuuid': str(eg), 'gpt': headers}


def boot(out, disk, mode):
    argv = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg',
            '-smp', '1', '-display', 'none', '-monitor', 'none', '-no-reboot',
            '-net', 'none', '-serial', 'stdio', '-drive',
            f'file={disk},if=none,id=target,format=raw', '-device',
            'nvme,drive=target,serial=ROOTPHASE0,bootindex=1']
    if mode == 'uefi':
        code = Path('/usr/share/OVMF/OVMF_CODE_4M.fd')
        template = Path('/usr/share/OVMF/OVMF_VARS_4M.fd')
        variables = out / 'fresh-vars.fd'
        shutil.copyfile(template, variables)
        argv += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={code}',
                 '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    assert disk.is_file() and disk.resolve().parent == out.resolve()
    assert '-cdrom' not in argv and len([x for x in argv if x == '-device']) == 1
    (out / f'{mode}-argv.json').write_text(json.dumps(argv, indent=2) + '\n')
    before = digest(disk)
    transcript = bytearray()
    with (out / f'{mode}-stderr.log').open('wb') as err:
        proc = subprocess.Popen(argv, cwd=REPO, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=err)
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        try:
            while time.monotonic() < deadline:
                for key, _ in selector.select(.2):
                    transcript.extend(key.fileobj.read1(65536))
                (out / f'{mode}-serial.log').write_bytes(transcript)
                assert b'PANIC' not in transcript
                assert b'[FAIL]' not in transcript, 'kernel failure before firmware-smoke boundary'
                # Stop before the legacy QEMU storage suite, which expects a
                # 32 MiB data fixture. This is a loader/module test, not root boot.
                if b'[ OK ] TarFS initialized:' in transcript:
                    return {'firmware': mode, 'loader_kernel_boot': True,
                            'target_bootstrap_parsed': True,
                            'installed_root_boot': False, 'disk_sha256': before}
                assert proc.poll() is None, 'QEMU exited before kernel boot'
            raise RuntimeError(f'{mode} boot timeout; retained logs in {out}')
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            transcript.extend(proc.stdout.read())
            selector.close()
            (out / f'{mode}-serial.log').write_bytes(transcript)
            assert digest(disk) == before, 'loader smoke changed the NVMe image'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--skip-boot', action='store_true')
    args = parser.parse_args()
    parent = REPO / 'build/root-phase0'
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='fixture-', dir=parent))
    manifest = {'scope': 'Linux fixture and firmware-loader audit only', 'errors': [],
                'commands': COMMANDS, 'boot': [], 'fixture': str(out)}
    try:
        manifest['commit'] = run(['git', 'rev-parse', 'HEAD'], out / 'tools.log').strip()
        for tool in (['gcc', '--version'], ['mke2fs', '-V'], ['qemu-system-x86_64', '--version']):
            run(tool, out / 'tools.log')
        tracked = ['scripts/test_root_phase0.py', 'tests/root_phase0_host.c',
                   'src/fs/ext4.c', 'src/fs/ext4_mount_journal.inc', 'src/fs/ext4_write.inc',
                   'src/fs/vfs.c', 'src/fs/jbd2.c', 'limine/limine.c']
        manifest['source_sha256'] = {name: digest(REPO / name) for name in tracked}
        for name in ('fortress.elf', 'initramfs.tar'):
            source = REPO / 'bin' / name
            shutil.copyfile(source, out / name)
            assert digest(source) == digest(out / name), 'source changed during snapshot'
        stage = out / 'root-tree'
        stage.mkdir()
        metadata = {}
        names = set()
        with tarfile.open(out / 'initramfs.tar') as archive:
            # Explicit extraction; no symlinks, devices, hard links or traversal.
            for member in archive:
                parts = Path(member.name).parts
                assert not member.name.startswith('/') and '..' not in parts
                assert re.fullmatch(r'[A-Za-z0-9_./-]+', member.name)
                normalized = Path(member.name).as_posix()
                assert normalized not in names and len(names) < 512
                assert normalized not in ('bootstrap', 'bootstrap/recovery.v1')
                assert len(normalized) < 256 and all(len(x) < 64 for x in parts)
                names.add(normalized)
                target = stage / member.name
                assert target.resolve().is_relative_to(stage.resolve())
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    assert member.isfile(), f'unsupported payload type: {member.name}'
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with target.open('xb') as dest:
                        shutil.copyfileobj(archive.extractfile(member), dest)
                target.chmod(member.mode)
                metadata['/' + normalized] = (member.mode, member.uid, member.gid)
        for directory, mode in [('home/operator', 0o700), ('root', 0o700),
                                ('var/lib/fortress/accounts', 0o700), ('mnt', 0o755)]:
            path = stage / directory
            path.mkdir(parents=True, exist_ok=True)
            path.chmod(mode)
            metadata['/' + directory] = (mode, 1000 if directory == 'home/operator' else 0,
                                        1000 if directory == 'home/operator' else 0)
        # Fixtures contain locked accounts, never production or known passwords.
        (stage / 'etc/passwd').write_text('root:x:0:0:root:/root:/bin/shell\noperator:x:1000:1000:operator:/home/operator:/bin/shell\n')
        (stage / 'etc/shadow').write_text('root:!:0:0:99999:7:::\noperator:!:0:0:99999:7:::\n')
        (stage / 'etc/shadow').chmod(0o600)
        marker = b'Phase 0 installed-root fixture; root boot not implemented.\n'
        (stage / 'etc/fortress-install.fixture').write_bytes(marker)
        root = out / 'root.img'
        with root.open('xb') as stream:
            stream.truncate(ROOT_BYTES)
        fs_uuid = str(uuid.uuid4())
        run(['mke2fs', '-q', '-t', 'ext4', '-b', '4096', '-I', '256', '-O', FEATURES,
             '-E', 'lazy_itable_init=0', '-m', '0', '-g', '4096', '-N', '4096',
             '-J', 'size=4', '-U', fs_uuid, '-d', str(stage), str(root)], out / 'root-audit.log')
        # DrvFS chmod/chown are not authoritative; set exact on-disk metadata.
        commands = ['set_inode_field / mode 040755', 'set_inode_field / uid 0', 'set_inode_field / gid 0']
        for path in sorted(stage.rglob('*')):
            name = '/' + path.relative_to(stage).as_posix()
            mode, uid, gid = metadata.get(name, (0o755 if path.is_dir() else 0o644, 0, 0))
            if name == '/etc/shadow':
                mode = 0o600
            mode |= 0o40000 if path.is_dir() else 0o100000
            commands += [f'set_inode_field {name} mode 0{mode:o}',
                         f'set_inode_field {name} uid {uid}', f'set_inode_field {name} gid {gid}']
        attrs = out / 'metadata.commands'
        attrs.write_text('\n'.join(commands) + '\n')
        run(['debugfs', '-w', '-f', str(attrs), str(root)], out / 'root-audit.log')
        manifest.update(initialize_journal(root, out))
        with root.open('rb') as stream:
            stream.seek(1024)
            sb = stream.read(1024)
        masks = struct.unpack_from('<III', sb, 92)
        assert masks == (4, 0x42, 0x403)
        run(['dumpe2fs', '-h', str(root)], out / 'root-audit.log')
        run(['e2fsck', '-fn', str(root)], out / 'root-audit.log')
        binary = out / 'root-phase0-host'
        run(['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
             '-Wall', '-Wextra', '-Werror', '-no-pie', '-pthread', '-Itests/ext4_host',
             '-Itests/host', '-Isrc/include', '-Isrc/fs', '-Isrc/drivers', '-Isrc/mm',
             'tests/root_phase0_host.c', 'tests/ext4_fault_disk.c', '-o', str(binary)],
            out / 'actual-code.log')
        for sector in (512, 4096):
            actual = out / f'actual-root-{sector}.img'
            # Preserve the original fixture; actual code mutates only this copy.
            run(['cp', '--sparse=always', str(root), str(actual)], out / 'actual-code.log')
            run([str(binary), str(actual), str(sector)], out / 'actual-code.log')
            run(['e2fsck', '-fn', str(actual)], out / 'actual-code.log')
            probe_dump = out / f'actual-probe-{sector}.dump'
            run(['debugfs', '-R', f'dump /phase0-probe {probe_dump}', str(actual)], out / 'actual-code.log')
            assert probe_dump.read_bytes() == b'phase0-actual-ext4\n'
        for path in ('/bin/login', '/bin/sudo', '/etc/shadow', '/home/operator'):
            text = run(['debugfs', '-R', f'stat {path}', str(root)], out / 'root-audit.log')
            assert 'Inode:' in text
            if path == '/etc/shadow':
                assert '0600' in text
            if path == '/home/operator':
                assert re.search(r'User:\s+1000\s+Group:\s+1000', text)
        dumped = out / 'marker.dump'
        run(['debugfs', '-R', f'dump /etc/fortress-install.fixture {dumped}', str(root)], out / 'root-audit.log')
        assert dumped.read_bytes() == marker
        # Two ephemeral test inputs model user choices; only hashes are retained.
        hashes = []
        for _ in range(2):
            secret = secrets.token_urlsafe(24)
            salt = ''.join(secrets.choice('./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz') for _ in range(16))
            value = crypt_hash(secret, f'$5$rounds=100000${salt}$')
            assert crypt_hash(secret, value) == value
            assert crypt_hash('incorrect', value) != value
            hashes.append(value)
            secret = None
        assert hashes[0] != hashes[1]
        assert hashes[0].split('$')[3] != hashes[1].split('$')[3]
        recovery = out / 'personalized-initramfs.tar'
        original = (out / 'initramfs.tar').read_bytes()
        with tarfile.open(fileobj=io.BytesIO(original)) as archive:
            with tarfile.open(recovery, 'w', format=tarfile.USTAR_FORMAT) as target:
                for member in archive:
                    target.addfile(member, archive.extractfile(member) if member.isfile() else None)
                payload = ('FORTRESS-RECOVERY-1\n' + hashes[0] + '\n').encode()
                directory = tarfile.TarInfo('bootstrap')
                directory.type, directory.mode = tarfile.DIRTYPE, 0o700
                target.addfile(directory)
                info = tarfile.TarInfo('bootstrap/recovery.v1')
                info.mode, info.size = 0o600, len(payload)
                target.addfile(info, io.BytesIO(payload))
        config = out / 'limine.conf'
        config.write_text('timeout: 0\n/FortressOS Phase 0 firmware fixture\n'
                          '    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n'
                          '    module_path: boot():/boot/initramfs.tar\n')
        esp = out / 'esp.img'
        create_esp_partition(esp, None, REPO / 'limine', out / 'fortress.elf',
                             recovery, config, None)
        extracted = out / 'esp-initramfs.tar'
        run(['mcopy', '-i', str(esp), '::boot/initramfs.tar', str(extracted)], out / 'esp-audit.log')
        assert digest(extracted) == digest(recovery)
        run(['mdir', '-i', str(esp), '::EFI/BOOT'], out / 'esp-audit.log')
        loader = out / 'esp-loader.efi'
        run(['mcopy', '-i', str(esp), '::EFI/BOOT/BOOTX64.EFI', str(loader)], out / 'esp-audit.log')
        assert digest(loader) == digest(REPO / 'limine/BOOTX64.EFI')
        run(['fsck.fat', '-n', str(esp)], out / 'esp-audit.log')
        manifest['disks'] = []
        for capacity in (384, 1024):
            disk, record = assemble(out, esp, root, capacity)
            record['sha256'] = digest(disk)
            manifest['disks'].append(record)
        if not args.skip_boot:
            disk = out / 'nvme-384.img'
            for mode in ('bios', 'uefi'):
                manifest['boot'].append(boot(out, disk, mode))
                print(f'PASS {mode}: NVMe-only firmware loader, source absent, image unchanged', flush=True)
        manifest.update(root_bytes=ROOT_BYTES, filesystem_uuid=fs_uuid,
                        feature_masks=list(masks), root_sha256=digest(root),
                        elf_sha256=digest(out / 'fortress.elf'),
                        source_initramfs_sha256=digest(out / 'initramfs.tar'),
                        actual_code_sector_adapters=[512, 4096],
                        personalized_initramfs_sha256=digest(recovery))
        print('PASS Linux profile/fsck/bytes/modes, ESP personalized archive, target-end GPT', flush=True)
    except Exception as error:
        manifest['errors'].append(repr(error))
        raise
    finally:
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(f'Evidence: {out}', flush=True)


if __name__ == '__main__':
    main()
