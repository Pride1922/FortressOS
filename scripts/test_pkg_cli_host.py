"""Actual CLI with mocked syscalls, plus isolated freestanding ELF build.
No initramfs, ISO, disk or default all target. Expects catalog host dependencies.
"""
import hashlib
import json
import pathlib
import struct
import subprocess

BUILD = pathlib.Path('build/pkg-cli')
SOURCES = ['user/pkg/format.c', 'user/pkg/manifest.c', 'user/pkg/verify.c',
           'user/pkg/catalog.c', 'user/pkg/cli.c', 'user/tools/digest.c',
           'user/tools/gzip.c', 'user/tools/common.c', 'src/lib/crypto/hash.c',
           'src/lib/crypto/curve.c']
INCLUDES = ['-I.', '-Isrc/include', '-Isrc/fs', '-Isrc/kernel', '-Iuser/tools',
            '-Isrc/lib/crypto/upstream/src']
VENDORS = ['src/lib/crypto/upstream/src/monocypher.c',
           'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c']


def run(args):
    subprocess.run(args, check=True, timeout=60)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    inputs = SOURCES+VENDORS+['tests/pkg_cli_host.c', 'user/pkg/cli.h',
                              'user/pkg/catalog.h', 'user/pkg/verify.h', 'user/pkg/format.h',
                              'user/tools/start.asm', 'user/shell.ld', 'src/lib/string.c']
    before = {p: hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest() for p in inputs}
    flags = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
             '-ffunction-sections', '-fdata-sections', *INCLUDES]
    run(flags+['-DTOOL_HOST_TEST', '-no-pie', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-Wl,--gc-sections', 'tests/pkg_cli_host.c',
               *SOURCES, 'build/pkg-format/catalog-mono.o', 'build/pkg-format/catalog-ed.o',
               '-o', str(BUILD/'host')])
    run([str(BUILD/'host')])
    objects = []
    free = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Os', '-ffreestanding',
            '-fno-builtin', '-fno-pie', '-fno-stack-protector', '-mno-red-zone',
            '-mno-sse', '-mno-mmx', '-msoft-float', '-ffunction-sections', '-fdata-sections',
            '-fstack-usage', *INCLUDES]
    for i, source in enumerate(SOURCES+['src/lib/string.c']+VENDORS):
        obj = str(BUILD/f'unit-{i}.o')
        extra = ['-include', 'src/lib/crypto/namespace.h'] if source in VENDORS else []
        if source.startswith('user/pkg/'):
            extra += ['-Wframe-larger-than=1024']
        run(free+extra+['-c', source, '-o', obj])
        objects.append(obj)
    run(['nasm', '-f', 'elf64', '-DTOOL_ENTRY=pkg_main', 'user/tools/start.asm', '-o', str(BUILD/'start.o')])
    run(['ld', '-m', 'elf_x86_64', '-nostdlib', '-static', '-z', 'noexecstack', '--gc-sections',
         '-T', 'user/shell.ld', str(BUILD/'start.o'), *objects, '-o', str(BUILD/'pkg.elf')])
    assert not subprocess.check_output(['nm', '-u', str(BUILD/'pkg.elf')]).strip()
    elf = (BUILD/'pkg.elf').read_bytes()
    assert elf[:7] == b'\x7fELF\x02\x01\x01'
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    phnum = struct.unpack_from('<H', elf, 56)[0]
    pages = 0
    for i in range(phnum):
        kind, flags, offset, addr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff+56*i)
        assert kind not in (2, 3)
        if kind == 1 and memsz:
            assert flags & 3 != 3 and offset+filesz <= len(elf) and filesz <= memsz
            pages += ((addr+memsz+4095)//4096)-(addr//4096)
    assert pages <= 1024
    after = {p: hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest() for p in inputs}
    assert after == before, 'source changed during CLI gate'
    (BUILD/'manifest.json').write_text(json.dumps({
        'sources': before, 'elf_sha256': hashlib.sha256(elf).hexdigest(),
        'elf_bytes': len(elf), 'mapped_pages': pages,
        'scope': 'mock syscall host PASS; private ELF link/structure PASS; Ring3 NOT RUN',
    }, indent=2)+'\n')
    print(f'Private pkg.elf linked: {len(elf)} bytes, {pages} mapped pages; Ring3 NOT RUN')


if __name__ == '__main__':
    main()
