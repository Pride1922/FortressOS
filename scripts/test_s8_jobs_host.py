#!/usr/bin/env python3
"""S8 Phases 4/5: real parser/executor/jobs/builtins with mocked syscalls."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-jobs-host-") as tmp:
    exe = str(Path(tmp) / "jobs")
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        # pipeline.c's call4() executes a real `syscall` instruction for
        # SYS_TERMATTR, and host syscall 34 is pause(2): this guard routes it to
        # the fixture's shell_termattr_call() instead of blocking the fixture.
        "-DSHELL_TERMATTR_HOST_TEST",
        "-Isrc/include", "-Isrc/fs", "-Iuser/shell", "tests/s8_jobs_host.c",
        *[f"user/shell/{m}.c" for m in
          ("lexer", "parser", "vars", "expand", "redir", "builtins",
           "program", "pipeline", "jobs", "jobctl")],
        "-o", exe], cwd=repo, check=True, timeout=60)
    subprocess.run([exe], check=True, timeout=60,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1",
                        "UBSAN_OPTIONS": "halt_on_error=1"})
