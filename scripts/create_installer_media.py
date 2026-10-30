#!/usr/bin/env python3
"""Separate installer media, created exclusively in regular-file workspaces.

No target credentials are embedded. Standard media requires the disposable QEMU
gate; separate bound media requires its compiled exact hardware fingerprint.
The live rescue entry carries no payload roles.
"""
import argparse
import binascii
import hashlib
import json
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tempfile
import tarfile
import uuid
import installer_log_volume as log_volume

ROOT = Path(__file__).resolve().parents[1]
ROLES = ('esp.img', 'root.img', 'bios.img', 'mbr.bin', 'payload.v1')
SIZES = (64 * 1024**2, 256 * 1024**2, 1024**2, 440, 256)


def elf_load_image(path):
    """Compare runtime ELF content when removing private debug sections."""
    data = Path(path).read_bytes()
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    assert header[0][:6] == b'\x7fELF\x02\x01' and header[2] == 62
    assert header[9] == 56 and header[10] > 0
    segments = []
    for i in range(header[10]):
        kind, flags, offset, va, pa, size, memory, alignment = struct.unpack_from(
            '<IIQQQQQQ', data, header[5] + i * 56)
        if kind == 1:
            assert size <= memory and offset + size <= len(data)
            segments.append((flags, va, pa, size, memory, alignment,
                             hashlib.sha256(data[offset:offset+size]).hexdigest()))
    assert segments
    return header[4], segments


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def regular(path):
    assert not path.is_symlink() and stat.S_ISREG(path.stat().st_mode), path


def build(templates, kernel, bootstrap, installer, preview=False, inspect=False, bound_digest=None, multi=True,
          combined=False, publish=True):
    assert not (preview and inspect)
    if preview or inspect or bound_digest:
        multi = False
    assert not combined or multi
    live_guid=uuid.uuid4() if combined else None
    if multi:
        kernel_bytes = kernel.read_bytes()
        assert b"NVMe/SATA disk selection installer ready" in kernel_bytes or b"Installer disposable backend ready" in kernel_bytes
        assert b"opt/fortress/installer_disposable" not in kernel_bytes or b"NVMe/SATA disk selection installer ready" in kernel_bytes
    interface_only=preview or inspect
    if bound_digest:
        assert not interface_only and len(bound_digest)==64
        fingerprint=bytes.fromhex(bound_digest);assert len(fingerprint)==32
        kernel_bytes=kernel.read_bytes()
        assert b'Drive-bound physical installer ready' in kernel_bytes
        assert b'opt/fortress/installer_disposable' not in kernel_bytes
        assert fingerprint in kernel_bytes
    templates = templates.resolve()
    assert templates.is_relative_to((ROOT / 'build/root-phase4').resolve())
    paths = [templates / name for name in ROLES]
    for path, size in zip(paths, SIZES):
        regular(path)
        assert path.stat().st_size == size
    wire = paths[-1].read_bytes()
    assert struct.unpack_from('<8sII4Q', wire) == (b'FOSPAY01', 1, 256, *SIZES[:4])
    assert wire[176:224] == bytes(48) and wire[224:] == hashlib.sha256(wire[:224]).digest()
    for i, path in enumerate(paths[:4]):
        assert wire[48+i*32:80+i*32] == bytes.fromhex(digest(path))
    for path in (kernel, bootstrap, installer):
        regular(path)
    if interface_only:
        assert b'opt/fortress/installer_disposable' not in kernel.read_bytes()
        assert b'Installation is disabled in this image.' in installer.read_bytes()
    if inspect:
        assert b'NVMe identity-only inspection ready' in kernel.read_bytes()
        assert b'Actual hardware identity; no synthetic target.' in installer.read_bytes()
    stem = 'fortress' if combined else 'fortress-nvme-inspect' if inspect else 'fortress-installer-preview' if preview else 'fortress-installer'
    media_mib = 64 if interface_only else 512
    normal = {str(p): digest(p) for p in (ROOT/'bin/fortress.iso', ROOT/'bin/fortress.img') if p.exists()}
    out = Path(tempfile.mkdtemp(prefix='media-', dir=ROOT/'build/root-phase4'))
    sources = paths + [kernel, bootstrap, installer] + [ROOT/'limine'/name for name in
        ('limine', 'limine-bios.sys', 'limine-bios-cd.bin', 'limine-uefi-cd.bin', 'BOOTX64.EFI')]
    sources += [Path(__file__).resolve()]
    if multi:sources += [Path(log_volume.__file__).resolve()]
    hashes = {str(p): digest(p) for p in sources}
    manifest = {'result': 'FAIL', 'scope': 'actual NVMe identity-only inspection; installation disabled' if inspect else 'interface preview; synthetic target; installation disabled' if preview else 'separate media packaging; runtime installation not accepted',
                'sources': hashes, 'normal_artifacts': normal, 'commands': []}
    if bound_digest:manifest['bound_inspector_digest']=bound_digest
    if multi:manifest['profile']='explicit user-selected NVMe/SATA; no fixed drive binding'
    def run(argv):
        command = list(map(str, argv))
        manifest['commands'].append(command)
        with (out/'producer.log').open('ab') as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    try:
        tree = out/'iso'; (tree/'boot/limine').mkdir(parents=True); (tree/'EFI/BOOT').mkdir(parents=True)
        (tree/'payload').mkdir()
        shutil.copyfile(kernel, tree/'boot/fortress.elf')
        # Debug sections can push the installer-only bootstrap over the
        # kernel/archive 8 MiB admission limit. Strip only this private copy;
        # preserve the original build and verify every loadable byte/entry.
        runtime_installer = out/'installer-runtime.elf'
        run(['objcopy', '--strip-debug', installer, runtime_installer])
        assert elf_load_image(installer) == elf_load_image(runtime_installer)
        # Private bootstrap staging; never inject the installer into live media.
        with tarfile.open(bootstrap) as source,tarfile.open(tree/'boot/initramfs.tar','w',format=tarfile.USTAR_FORMAT) as archive:
            for member in source:
                assert member.name.rstrip('/')!='bin/install'
                archive.addfile(member,source.extractfile(member) if member.isfile() else None)
            info=tarfile.TarInfo('bin/install');info.mode=0o755;info.uid=info.gid=0;info.size=runtime_installer.stat().st_size
            with runtime_installer.open('rb') as stream:archive.addfile(info,stream)
        # Leave room for the recovery record added by personalization. The
        # installed bootstrap slot and boot validator both remain at 8 MiB.
        assert (tree/'boot/initramfs.tar').stat().st_size<=8*1024**2-2560, \
            'installer bootstrap exceeds the 8 MiB runtime limit; image not published'
        for path in ([] if interface_only else paths):
            shutil.copyfile(path, tree/'payload'/path.name)
        for name in ('limine-bios.sys', 'limine-bios-cd.bin', 'limine-uefi-cd.bin'):
            shutil.copyfile(ROOT/'limine'/name, tree/'boot/limine'/name)
        shutil.copyfile(ROOT/'limine/BOOTX64.EFI', tree/'EFI/BOOT/BOOTX64.EFI')
        log_hash = hashlib.sha256(log_volume.metadata()).hexdigest() if multi else None
        def entry(label, mode, payload):
            text = f'/{label}\n    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    kernel_cmdline: boot_mode={mode}\n    module_path: boot():/boot/initramfs.tar\n    module_cmdline: role=bootstrap\n'
            if multi and mode == 'installer':
                text = text.replace('boot_mode=installer\n', f'boot_mode=installer installer_log_sha256={log_hash}\n')
            if payload:
                for name, role in zip(ROLES, ('esp', 'root', 'bios', 'mbr', 'manifest')):
                    text += f'    module_path: boot():/payload/{name}\n    module_cmdline: role=install-{role}\n'
            return text
        if combined:
            live=entry('FortressOS (Live USB)','live',False).replace('boot_mode=live\n',
                f'boot_mode=live usb_data=PARTUUID={str(live_guid).upper()} usb_data_mode=rw\n')
            config='timeout: 5\n'+live+entry('FortressOS (Installer)','installer',True)
        else:
            config = 'timeout: 5\n' + (entry('FortressOS NVMe inspection - installation disabled', 'installer', False) if inspect else entry('FortressOS installer PREVIEW - installation disabled', 'installer', False) if preview else entry('FortressOS Installer', 'installer', True) + entry('FortressOS Live Rescue', 'live', False))
        for name in ('limine.conf', 'boot/limine.conf', 'boot/limine/limine.conf', 'EFI/BOOT/limine.conf'):
            (tree/name).write_text(config)
        iso = out/(stem+'.iso')
        if not combined:
            run(['xorriso', '-as', 'mkisofs', '-b', 'boot/limine/limine-bios-cd.bin', '-no-emul-boot',
                 '-boot-load-size', '4', '-boot-info-table', '--efi-boot', 'boot/limine/limine-uefi-cd.bin',
                 '-efi-boot-part', '--efi-boot-image', '--protective-msdos-label', tree, '-o', iso])
            run([ROOT/'limine/limine', 'bios-install', iso])
        # Independent 512MiB FAT32 media volume; payload files do not fit the
        # installed target's 64MiB ESP. Dedicated BIOS slot preserves GPT arrays.
        esp = out/'media-esp.img'
        with esp.open('xb') as stream: stream.truncate(media_mib*1024**2)
        run(['mformat', '-i', esp, '-F', '::'])
        for directory in ('boot', 'boot/limine', 'EFI', 'EFI/BOOT', 'payload'):
            run(['mmd', '-i', esp, '::'+directory])
        for path in sorted(tree.rglob('*')):
            if path.is_file(): run(['mcopy', '-i', esp, path, '::'+path.relative_to(tree).as_posix()])
        # mformat does not populate backup FSInfo on this tool version.
        with esp.open('r+b') as stream:
            bpb = stream.read(512)
            fi, backup = struct.unpack_from('<HH', bpb, 48)
            stream.seek(fi*512); info = stream.read(512)
            stream.seek((backup+fi)*512); stream.write(info)
        run(['fsck.fat', '-n', esp])
        sectors = (media_mib+8+(8 if multi else 0)+(64 if combined else 0))*2048; entries = bytearray(16384)
        volumes = [
            ('c12a7328-f81f-11d2-ba4b-00a0c93ec93b', 4096, media_mib*2048, 'Installer media'),
            ('21686148-6449-6e6f-744e-656564454649', 2048, 2048, 'BIOS boot')]
        if multi:
            volumes.append(('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7', 514*2048, log_volume.SECTORS, 'Installer logs'))
            log_image=out/'media-log.img';log_volume.create(log_image)
            run(['fsck.fat','-n',log_image])
            extracted=out/'audit-fortress-install.log'
            run(['mcopy','-i',log_image,'::fortress-install.log',extracted])
            assert extracted.stat().st_size==log_volume.FILE_BYTES
        if combined:
            from create_boot_img import create_data_partition
            live_image=out/'live-data.ext4';create_data_partition(live_image,'ext4')
            live_start=(media_mib+2+8)*2048
            volumes.append(('0fc63daf-8483-4772-8e79-3d69d8477de4',live_start,64*2048,'Live USB data'))
        for i, (kind, start, count, label) in enumerate(volumes):
            struct.pack_into('<16s16sQQQ72s', entries, i*128, uuid.UUID(kind).bytes_le,
                (live_guid if combined and i==len(volumes)-1 else uuid.uuid4()).bytes_le,
                start, start+count-1, 0, label.encode('utf-16le').ljust(72, b'\0'))
        disk_guid = uuid.uuid4()
        def header(at, other, array):
            data = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0,
                at, other, 34, sectors-34, disk_guid.bytes_le, array, 128, 128, binascii.crc32(entries)))
            struct.pack_into('<I', data, 16, binascii.crc32(data)); return data.ljust(512, b'\0')
        image = out/(stem+'.img')
        with image.open('xb') as stream:
            stream.truncate(sectors*512); mbr = bytearray(512)
            struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\0\x02\0', 0xee, b'\xff'*3, 1, sectors-1)
            mbr[510:] = b'\x55\xaa'
            for at, data in ((0,mbr),(512,header(1,sectors-1,2)),(1024,entries),
                             ((sectors-33)*512,entries),((sectors-1)*512,header(sectors-1,1,sectors-33))):
                stream.seek(at); stream.write(data)
            stream.seek(4096*512)
            with esp.open('rb') as source: shutil.copyfileobj(source, stream, 1024**2)
            if multi:
                stream.seek(514*1024**2)
                with log_image.open('rb') as source:shutil.copyfileobj(source,stream,1024**2)
            if combined:
                stream.seek(live_start*512)
                with live_image.open('rb') as source:shutil.copyfileobj(source,stream,1024**2)
        run([ROOT/'limine/limine', 'bios-install', image, '2'])
        with image.open('rb') as stream:
            for at in (1, sectors-1):
                stream.seek(at*512); h = bytearray(stream.read(512)); expected = struct.unpack_from('<I',h,16)[0]
                struct.pack_into('<I',h,16,0); assert binascii.crc32(h[:92]) == expected
                array, count, stride, checksum = struct.unpack_from('<QIII',h,72)
                assert count == stride == 128
                stream.seek(array*512); actual = stream.read(count*stride)
                assert actual == entries and binascii.crc32(actual) == checksum
            stream.seek(4096*512); h = hashlib.sha256()
            for _ in range(media_mib): h.update(stream.read(1024**2))
            assert h.hexdigest() == digest(esp)
        # Independently extract every transport payload from the FAT volume.
        for path in ([] if interface_only else paths):
            extracted = out/('audit-'+path.name)
            run(['mcopy','-i',esp,'::payload/'+path.name,extracted])
            assert digest(extracted) == digest(path)
        assert all(digest(Path(p)) == value for p,value in hashes.items())
        assert all(digest(Path(p)) == value for p,value in normal.items())
        artifacts=(image,) if combined else (iso,image)
        manifest['artifacts'] = {p.name: {'bytes':p.stat().st_size,'sha256':digest(p)} for p in artifacts}
        manifest['media_disk_guid'] = str(disk_guid); manifest['result'] = 'PASS'
        if multi:manifest['usb_log']={'volume_lba':514*2048,'data_lba':log_volume.DATA_LBA,'file_bytes':log_volume.FILE_BYTES,'metadata_sha256':log_hash,'path':'/fortress-install.log'}
        if combined:
            manifest['live_data']={'partuuid':str(live_guid),'lba':live_start,'sectors':64*2048}
            manifest['boot_choices']=['FortressOS (Live USB)','FortressOS (Installer)']
        if not publish:return out
        # Publish only complete audited regular files; symlink destinations reject.
        for path in (iso,image):
            destination = ROOT/'bin'/path.name
            assert not destination.is_symlink()
            if destination.exists(): regular(destination)
            temporary = ROOT/'bin'/(path.name+'.new')
            with temporary.open('xb') as stream, path.open('rb') as source: shutil.copyfileobj(source,stream,1024**2)
            temporary.replace(destination)
        dell_image = ROOT / 'bin/fortress-installer-dell5590.img'
        if dell_image.exists():
            shutil.copyfile(ROOT / 'bin' / (stem + '.img'), dell_image)
        destination=ROOT/'bin'/(stem+'-build.log')
        assert not destination.is_symlink()
        if destination.exists():regular(destination)
        shutil.copyfile(out/'producer.log',destination)
    finally:
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    destination=ROOT/'bin'/(stem+'-manifest.json')
    assert not destination.is_symlink()
    if destination.exists():regular(destination)
    shutil.copyfile(out/'manifest.json',destination)
    print(out)
    return out


def main():
    parser = argparse.ArgumentParser(description="Create FortressOS Full Installer Image")
    parser.add_argument('--templates', type=Path)
    parser.add_argument('--preview', action='store_true', help='Require interface-only tool and kernel without disposable write backend')
    parser.add_argument('--inspect', action='store_true', help='Identity-only hardware inspection; sector/deploy callbacks absent')
    parser.add_argument('--multi', action='store_true', default=True, help='Full installer with NVMe disk inventory and selection (default)')
    parser.add_argument('--bound-digest', help='Require a separate physical-profile kernel containing this inspector SHA-256 identity')
    parser.add_argument('--kernel', type=Path, default=ROOT/'bin/fortress.elf')
    parser.add_argument('--bootstrap', type=Path, default=ROOT/'bin/initramfs.tar')
    parser.add_argument('--installer', type=Path, default=ROOT/'build/tool-install.elf')
    args = parser.parse_args()
    if args.templates is None:
        result = subprocess.run(['python3',str(ROOT/'scripts/create_installer_payload.py'),
            '--kernel',str(args.kernel),'--bootstrap',str(args.bootstrap)],cwd=ROOT,
            capture_output=True,text=True,check=True,timeout=600)
        args.templates = Path(result.stdout.strip().splitlines()[-1])
    build(args.templates,args.kernel,args.bootstrap,args.installer,args.preview,args.inspect,args.bound_digest,multi=not (args.preview or args.inspect or args.bound_digest))


if __name__ == '__main__': main()
