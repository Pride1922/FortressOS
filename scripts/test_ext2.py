#!/usr/bin/env python3
"""Host parser tests using the real ext2/VFS C sources and e2fsprogs fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fortress-ext2-") as tmp:
    tmp = Path(tmp)
    exe = tmp / "test"
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        "-Itests/host", "-Isrc/include", "-Isrc/drivers", "-Isrc/mm", "-Isrc/fs",
        "tests/ext2_host.c", "-o", str(exe),
    ], cwd=repo, check=True)
    root = tmp / "root"
    root.mkdir()
    (root / "hello.txt").write_text("Hello from FortressOS ext2 NVMe partition!\n")
    (root / "nested").mkdir()
    (root / "nested" / "note.txt").write_text("Nested ext2 directory works.\n")
    (root / "large.bin").write_bytes(bytes((i * 17 + 3) & 255 for i in range(400000)))
    with (root / "sparse.bin").open("wb") as sparse:
        sparse.seek(20000)
        sparse.write(b"END")
    for bs, inode_size in ((1024, 128), (2048, 128), (4096, 128), (4096, 256)):
        image = tmp / f"ext2-{bs}-{inode_size}.img"
        with image.open("wb") as f:
            f.truncate(16 * 1024 * 1024)
        subprocess.run(["mke2fs", "-q", "-t", "ext2", "-b", str(bs), "-I", str(inode_size),
                        "-O", "none,filetype,sparse_super,large_file", "-d", str(root),
                        "-F", str(image)], check=True)
        for ss in (512, 4096):
            subprocess.run([str(exe), str(image), str(ss)], check=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
