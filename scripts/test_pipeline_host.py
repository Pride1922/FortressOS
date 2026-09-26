#!/usr/bin/env python3
"""ASan/UBSan orchestration tests with actual shell modules and mocked syscalls."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-pipeline-host-") as tmp:
    exe = str(Path(tmp) / "pipeline")
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        "-Isrc/include", "-Isrc/fs", "-Iuser/shell", "tests/pipeline_host.c",
        *[f"user/shell/{module}.c" for module in
          ("lexer", "parser", "vars", "expand", "redir", "builtins", "program", "pipeline")],
        "-o", exe], cwd=repo, check=True)
    subprocess.run([exe], check=True, timeout=30)
