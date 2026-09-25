#!/usr/bin/env python3
"""One host entry point; compile the real editor under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-shell-host-") as tmp:
    exe = str(Path(tmp) / "shell")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                    "-Isrc/include", "-Iuser/shell", "tests/shell_host.c",
                    "user/shell/lineedit.c", "user/shell/lexer.c", "user/shell/parser.c", "-o", exe], cwd=repo, check=True)
    subprocess.run([exe], check=True)
