#!/usr/bin/env python3
"""Production journaled EXT4 memory audit; disposable copy, no physical claim."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_usb_persistence import run_qemu_session, send_command, check_offline_ext2

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def journal_info(disk, output, expected_file=None):
    with tempfile.TemporaryDirectory(prefix="fortress-memory-ext4-") as directory:
        part = Path(directory) / "data.ext4"
        with disk.open("rb") as stream:
            stream.seek(133120 * 512)
            part.write_bytes(stream.read(131072 * 512))
        result = subprocess.run(["dumpe2fs", "-h", str(part)], check=True,
                                capture_output=True, text=True, timeout=30)
        features = next(line for line in result.stdout.splitlines()
                        if line.startswith("Filesystem features:"))
        assert "has_journal" in features.split() and "extent" in features.split()
        output.write_text(result.stdout + result.stderr)
        if expected_file is not None:
            extracted = Path(directory) / "memory-check.txt"
            audit = subprocess.run(["debugfs", "-R", f"dump /memory-check.txt {extracted}", str(part)],
                                   capture_output=True, text=True, check=True, timeout=30)
            output.with_suffix(".bytes.txt").write_text(audit.stdout + audit.stderr)
            if expected_file:
                assert extracted.read_bytes() == b"memory-ext4-persistence\n"
            else:
                assert not extracted.exists() and "File not found" in audit.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpus", type=int, choices=(1, 4, 8), default=4)
    args = parser.parse_args()
    out = args.output.resolve()
    assert out.is_relative_to(ROOT / "build") and out != ROOT / "build"
    out.mkdir(exist_ok=False)
    source = ROOT / "bin/fortress.img"
    original = digest(source)
    records = []
    try:
        for firmware in ("bios", "uefi"):
            disk = out / f"{firmware}.img"
            shutil.copyfile(source, disk)
            journal_info(disk, out / f"{firmware}-before.dumpe2fs.txt")

            def write(qmp, child, log):
                text = log.read_text(errors="replace")
                assert "[MEMORY STORAGE] PASS ten warmed" in text
                assert "[USB E4-B] Selected filesystem: ext4 journaled" in text
                send_command(qmp, child, log, "echo memory-ext4-persistence > /mnt/memory-check.txt\n", " $ ")
                send_command(qmp, child, log, "sync\n", "Filesystem synced.")
                send_command(qmp, child, log, "shutdown\n", "Shutdown initiated")

            def read(qmp, child, log):
                send_command(qmp, child, log, "cat /mnt/memory-check.txt\n", "memory-ext4-persistence")
                send_command(qmp, child, log, "rm /mnt/memory-check.txt\n", " $ ")
                send_command(qmp, child, log, "sync\n", "Filesystem synced.")
                send_command(qmp, child, log, "shutdown\n", "Shutdown initiated")

            for boot, action in enumerate((write, read), 1):
                log = out / f"{firmware}-boot{boot}.log"
                run_qemu_session(firmware, disk, log, action, "memory-ext4",
                                 cpus=args.cpus, memory_probe=(boot == 1))
                check_offline_ext2(disk)  # Common Linux checker handles EXT4 too.
                journal_info(disk, out / f"{firmware}-boot{boot}.dumpe2fs.txt", expected_file=(boot == 1))
            records.append({"firmware": firmware, "cpus": args.cpus, "status": "PASS"})
            print(f"PASS journaled EXT4 memory {firmware} SMP={args.cpus}", flush=True)
    finally:
        assert digest(source) == original, "Shipped image changed during disposable test"
        (out / "result.json").write_text(json.dumps({"source_sha256": original,
            "status": "PASS" if len(records) == 2 else "FAIL", "cases": records,
            "scope": "Kernel synchronous VFS memory audit plus Ring 3 persistence; no performance or hardware claim"}, indent=2) + "\n")


if __name__ == "__main__":
    main()
