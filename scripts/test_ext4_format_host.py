#!/usr/bin/env python3
"""Run the actual read-only driver against freshly generated Linux fixtures."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
result = subprocess.run([sys.executable, str(root / "scripts/create_ext4_fixtures.py")],
                        cwd=root, text=True, capture_output=True, check=True)
print(result.stdout, end="", flush=True)
evidence = Path(result.stdout.strip().split("Evidence: ")[-1])
subprocess.run([str(root / "build/ext4_format_host"),
                *(str(evidence / f"e4a-{bs}.img") for bs in (1024, 2048, 4096))],
               cwd=root, check=True)
