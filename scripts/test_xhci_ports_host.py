#!/usr/bin/env python3
"""Run xHCI root port discovery and reset host test with mock transport and sanitizers."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='fortress-xhci-ports-') as tmp:
    exe = str(Path(tmp) / 'xhci-ports-test')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                    '-Isrc/include', '-Isrc/drivers', 'tests/xhci_ports_host.c',
                    'src/drivers/xhci_ports.c', '-o', exe], cwd=repo, check=True)
    subprocess.run([exe], check=True, timeout=15)
