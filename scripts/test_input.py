#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-input-host-") as tmp:
    exe = str(Path(tmp) / "input-test")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                    "-Isrc/include", "-Isrc/drivers", "tests/input_host.c",
                    "src/drivers/keyboard.c", "-o", exe], cwd=repo, check=True)
    subprocess.run([exe], check=True)
