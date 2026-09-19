#!/usr/bin/env python3
"""Run the actual xHCI reset state machine with a host mock and sanitizers."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='fortress-xhci-reset-') as tmp:
    exe = str(Path(tmp) / 'xhci-reset-test')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                    '-Isrc/include', '-Isrc/drivers', 'tests/xhci_reset_host.c',
                    'src/drivers/xhci_reset.c', '-o', exe], cwd=repo, check=True)
    subprocess.run([exe], check=True, timeout=15)
