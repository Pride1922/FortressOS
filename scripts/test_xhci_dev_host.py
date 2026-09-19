#!/usr/bin/env python3
"""Run xHCI device addressing and descriptor parsing host test with sanitizers."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='fortress-xhci-dev-') as tmp:
    exe = str(Path(tmp) / 'xhci-dev-test')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                    '-Isrc/include', '-Isrc/drivers', 'tests/xhci_dev_host.c',
                    'src/drivers/xhci_dev.c', '-o', exe], cwd=repo, check=True)
    subprocess.run([exe], check=True, timeout=15)
