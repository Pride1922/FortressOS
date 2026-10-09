#!/usr/bin/env python3
"""Atomically refresh only the dedicated generated EXT4 test-image artifact.
Generic image generation still refuses existing EXT4 paths and device paths.
"""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parent.parent

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso-root',type=Path,default=ROOT/'build/iso_root')
    parser.add_argument('--limine-dir',type=Path,default=ROOT/'limine')
    args=parser.parse_args()
    directory=ROOT/'bin';directory.mkdir(exist_ok=True)
    output=directory/'fortress-ext4-test.img'
    if output.is_symlink() or (output.exists() and not output.is_file()):
        raise RuntimeError('Test image destination must be a regular build artifact')
    with tempfile.TemporaryDirectory(prefix='ext4-image-',dir=directory) as temporary:
        candidate=Path(temporary)/'image.img'
        subprocess.run([sys.executable,str(ROOT/'scripts/create_boot_img.py'),str(candidate),
                        '--filesystem','ext4-nojournal','--iso-root',str(args.iso_root),
                        '--limine-dir',str(args.limine_dir)],cwd=ROOT,check=True)
        candidate.replace(output)
    print(f'EXT4 no-journal workbench image: {output}; default fortress.img uses journaled EXT4.')

if __name__=='__main__':main()
