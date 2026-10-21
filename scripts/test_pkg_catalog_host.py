"""Build actual Fortress crypto and package code with sanitizer instrumentation.
No release targets, shared crypto outputs or production keys are touched.
"""
import pathlib
import subprocess
import sys

BUILD = pathlib.Path('build/pkg-format')


def run(args):
    subprocess.run(args, check=True, timeout=60)


def main():
    run([sys.executable, 'scripts/pkg_catalog_vectors.py'])
    flags = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
             '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-I.', '-Isrc/include', '-Isrc/fs', '-Isrc/kernel',
             '-Iuser/tools', '-Isrc/lib/crypto/upstream/src']
    objects = []
    for source, name in [('src/lib/crypto/upstream/src/monocypher.c', 'catalog-mono'),
                         ('src/lib/crypto/upstream/src/optional/monocypher-ed25519.c', 'catalog-ed')]:
        obj = str(BUILD/(name+'.o'))
        run(flags+['-include', 'src/lib/crypto/namespace.h', '-c', source, '-o', obj])
        objects.append(obj)
    sources = ['tests/pkg_catalog_host.c', 'user/pkg/catalog.c', 'user/pkg/format.c',
               'user/pkg/manifest.c', 'user/pkg/verify.c', 'user/tools/digest.c',
               'user/tools/gzip.c', 'src/lib/crypto/hash.c', 'src/lib/crypto/curve.c']
    run(flags+['-no-pie', '-Wl,--gc-sections', *sources, *objects, '-o', str(BUILD/'catalog-host')])
    run([str(BUILD/'catalog-host')])
    run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
         '-mno-red-zone', '-mno-sse', '-mno-mmx', '-msoft-float', '-fstack-usage',
         '-Wframe-larger-than=1024', '-Isrc/include', '-c', 'user/pkg/catalog.c',
         '-o', str(BUILD/'catalog.o')])


if __name__ == '__main__':
    main()
