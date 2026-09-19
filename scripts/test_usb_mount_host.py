#!/usr/bin/env python3
"""Run USB mount and cmdline parsing host unit test with sanitizers."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='fortress-usb-mount-') as tmp:
    exe = str(Path(tmp) / 'usb-mount-test')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                    '-Isrc/include', '-Isrc/drivers', '-Isrc/fs', '-Isrc/lib', '-Isrc/mm',
                    'tests/usb_mount_host.c', '-o', exe], cwd=repo, check=True)
    subprocess.run([exe], check=True, timeout=15)
