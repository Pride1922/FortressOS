#!/usr/bin/env python3
"""Actual stack caller with PMM/VMM adapters under ASan/UBSan; no SMP claim."""
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent
source = (REPO / "src/kernel/thread.c").read_text()
begin = source.index("static int kstack_alloc(")
end = source.index("static void runqueue_push_cpu_locked", begin)
with tempfile.TemporaryDirectory(prefix="fortress-kstack-host-") as directory:
    temporary = Path(directory)
    (temporary / "kstack_impl.h").write_text(source[begin:end])
    executable = temporary / "kstack"
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-no-pie",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DTEST_SMP_MEMORY", "-pthread",
                    "-Isrc/include", "-Isrc/kernel", "-Isrc/mm", "-Isrc/arch/x86_64", "-I" + str(temporary),
                    "tests/kstack_batch_host.c", "-o", str(executable)], cwd=REPO, check=True, timeout=60)
    subprocess.run([str(executable)], check=True, timeout=30)
