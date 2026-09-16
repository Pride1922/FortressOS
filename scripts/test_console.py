#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-console-") as tmp:
    exe = str(Path(tmp) / "console-test")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                    "-Itests/host", "-Isrc/include", "-Isrc/drivers",
                    "tests/console_host.c", "-o", exe], cwd=repo, check=True)
    subprocess.run([exe], check=True)
