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
                    "-Isrc/include", "-Isrc/fs", "-Iuser/shell", "tests/shell_host.c",
                    "user/shell/lineedit.c", "user/shell/lexer.c", "user/shell/parser.c",
                    "user/shell/vars.c", "user/shell/alias.c", "user/shell/expand.c",
                    "user/shell/redir.c", "-o", exe], cwd=repo, check=True)
    subprocess.run([exe], check=True)
    io_obj = str(Path(tmp) / "io.o")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-ffreestanding",
                    "-DSHELL_IO_HOST_TEST", "-Dputs=shell_puts",
                    "-Isrc/include", "-Isrc/fs", "-Iuser/shell",
                    "-c", "user/shell/io.c", "-o", io_obj], cwd=repo, check=True)
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                    "-Isrc/include", "tests/shell_io_host.c", io_obj, "-o", exe],
                   cwd=repo, check=True)
    subprocess.run([exe], check=True)
    print("PASS shell I/O: short writes, partial failure, zero progress and closed stderr")
