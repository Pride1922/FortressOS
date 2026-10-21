"""Private instrumented PKG nested-stack/max-argv/signal Ring 3 gate."""
from pathlib import Path
import test_pkg_cli_qemu as fixture
from test_pkg_cli_host import SOURCES, VENDORS, INCLUDES


def prepare(work, log, stage):
    import hashlib
    import gzip
    import zlib
    import pkg_artifact_vectors as artifacts
    import pkg_catalog_vectors as catalogs
    m, tar = artifacts.parts(documentation=b'abcd'*8192)
    for name, level, strategy in [('dynamic',6,zlib.Z_DEFAULT_STRATEGY),
                                  ('fixed',6,zlib.Z_FIXED), ('stored',0,zlib.Z_DEFAULT_STRATEGY)]:
        compressor=zlib.compressobj(level,zlib.DEFLATED,31,8,strategy)
        payload=compressor.compress(tar)+compressor.flush()
        assert (payload[10]>>1)&3 == {'dynamic':2,'fixed':1,'stored':0}[name]
        assert gzip.decompress(payload)==tar
        data=artifacts.pack(m,tar,payload)
        (stage/f'docs/stack-{name}.fpk').write_bytes(data)
        entry=catalogs.entry(length=len(data),digest=hashlib.sha256(data).digest())
        (stage/f'docs/stack-{name}.cat').write_bytes(catalogs.catalog(
            entries=[entry],issued=1767225600,expires=4102444800))
    build = work/'build/pkg-stack'
    build.mkdir()
    flags = ['gcc', '-Os', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
             '-fno-builtin', '-fno-pie', '-fno-stack-protector', '-mno-red-zone',
             '-mno-sse', '-mno-mmx', '-msoft-float', '-ffunction-sections',
             '-fdata-sections', '-fstack-usage', *INCLUDES]
    objects = []
    for i, src in enumerate(SOURCES+['src/lib/string.c']+VENDORS):
        obj = build/f'unit-{i}.o'
        extra = ['-include', 'src/lib/crypto/namespace.h'] if src in VENDORS else []
        if src.startswith('user/pkg/'):
            extra += ['-Wframe-larger-than=1024']
        fixture.run(flags+extra+['-finstrument-functions', '-c', src, '-o', str(obj)], work, log)
        objects.append(str(obj))
    fixture.run(flags+['-Wframe-larger-than=1024', '-c', 'tests/pkg_stack_ring3.c',
                      '-o', str(build/'probe.o')], work, log)
    fixture.run(['nasm', '-f', 'elf64', '-DTOOL_ENTRY=pkg_stack_main', 'user/tools/start.asm',
                 '-o', str(build/'start.o')], work, log)
    fixture.run(['ld', '-nostdlib', '-static', '-z', 'noexecstack', '--gc-sections', '-T',
                 'user/shell.ld', str(build/'start.o'), str(build/'probe.o'),
                 'build/pkg-cli/test-policy.o', *objects, '-o', str(build/'probe.elf')], work, log)
    import shutil
    shutil.copyfile(build/'probe.elf', stage/'bin/pkg-stack')
    fixture.run(flags+['-Wframe-larger-than=1024', '-DSTACK_PLAIN',
                      '-DSTACK_SELF="/bin/pkg-stack-plain"', '-c', 'tests/pkg_stack_ring3.c',
                      '-o', str(build/'plain.o')], work, log)
    plain = sorted(str(p) for p in (work/'build/pkg-cli').glob('unit-*.o'))
    fixture.run(['ld', '-nostdlib', '-static', '-z', 'noexecstack', '--gc-sections', '-T',
                 'user/shell.ld', str(build/'start.o'), str(build/'plain.o'),
                 'build/pkg-cli/test-policy.o', *plain, '-o', str(build/'plain.elf')], work, log)
    shutil.copyfile(build/'plain.elf', stage/'bin/pkg-stack-plain')
    return [['/bin/pkg-stack-plain', 'STACK PASS'], ['/bin/pkg-stack', 'STACK PASS']]


if __name__ == '__main__':
    fixture.main(prepare)
