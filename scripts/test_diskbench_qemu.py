#!/usr/bin/env python3
"""QEMU Integration Test for /bin/diskbench."""
from pathlib import Path
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import re

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from test_nmi_transitions import connect


def strip_ansi(s):
    return re.sub(r'\x1b\[[0-9;?]*[a-zA-Z]', '', s)


def send_str(uart, s):
    for b in s.encode():
        uart.send(bytes([b]))
        time.sleep(0.005)


def wait_for_pattern(log_path, pattern, child, start_pos=0, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log_path.exists():
            txt = log_path.read_text(errors="replace").replace("\r", "")
            if pattern in txt[start_pos:]:
                return txt
        assert child.poll() is None, f"QEMU exited prematurely with code {child.poll()}"
        time.sleep(0.05)
    full = log_path.read_text(errors="replace") if log_path.exists() else ""
    raise AssertionError(f"Timeout waiting for {pattern!r} after pos {start_pos}. Log tail:\n{full[-1000:]}")


def test_diskbench_qemu():
    print("=== Running diskbench QEMU Integration Test ===")
    build_dir = REPO / "build"
    nvme_img = build_dir / "nvme_gpt.img"
    assert nvme_img.exists(), f"NVMe disk image missing: {nvme_img}"

    with tempfile.TemporaryDirectory(prefix="fortress-diskbench-qemu-") as tmp:
        uart_path = Path(tmp) / "uart"
        log_path = REPO / "build" / "diskbench_serial.log"
        if log_path.exists(): log_path.unlink()

        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", "1", "-display", "none",
            "-no-reboot", "-boot", "d", "-cdrom", "bin/fortress.iso",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log_path}",
            "-serial", "chardev:uart",
            "-drive", f"file={nvme_img},if=none,id=nvm0,format=raw,snapshot=on",
            "-device", "nvme,serial=fortress0,drive=nvm0",
            "-fw_cfg", "name=opt/fortress/write_test,string=1"
        ]

        proc = subprocess.Popen(qemu_cmd, cwd=REPO)
        try:
            uart = connect(uart_path)
            stop_drain = threading.Event()
            def drain():
                uart.settimeout(0.2)
                while not stop_drain.is_set():
                    try:
                        if not uart.recv(4096): return
                    except (socket.timeout, OSError):
                        continue
            threading.Thread(target=drain, daemon=True).start()

            print("  [1] Waiting for interactive shell prompt...")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, timeout=40)
            print("  [1] Shell prompt ready.")

            # Test 1: diskbench --help
            print("  [2] Testing: diskbench --help")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "diskbench --help\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = strip_ansi(log_path.read_text(errors="replace")[pos:])
            assert "Usage: diskbench" in out, f"Unexpected output: {out}"
            assert "-w SIZE" in out, f"Missing -w option in help: {out}"
            assert "-n COUNT" in out, f"Missing -n option in help: {out}"
            print("  [PASS] diskbench --help verified.")

            # Test 2: Standard run on /mnt (ext2 default)
            print("  [3] Testing: diskbench -w 128K -n 20 /mnt")
            pos = len(log_path.read_text(errors="replace"))
            cmd2 = "diskbench -w 128K -n 20 /mnt\n"
            send_str(uart, cmd2)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd2) - 5, timeout=20)
            out_std = strip_ansi(log_path.read_text(errors="replace")[pos:])
            print(f"      Standard Output:\n{out_std.strip()}")
            assert "=== diskbench: /mnt ===" in out_std, f"Missing header: {out_std}"
            assert "Filesystem:   ext2" in out_std, f"Missing ext2 detection: {out_std}"
            assert "Write: 131072 bytes in" in out_std, f"Missing write test: {out_std}"
            assert "Read:  131072 bytes in" in out_std, f"Missing read test: {out_std}"
            assert "Meta:  20 files created & unlinked in" in out_std, f"Missing meta test: {out_std}"
            assert "Summary:" in out_std, f"Missing summary section: {out_std}"
            print("  [PASS] Standard diskbench run on /mnt succeeded.")

            # Test 3: Comparison-friendly output (-c)
            print("  [4] Testing: diskbench -c -w 128K -n 20 /mnt")
            pos = len(log_path.read_text(errors="replace"))
            cmd3 = "diskbench -c -w 128K -n 20 /mnt\n"
            send_str(uart, cmd3)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd3) - 5, timeout=20)
            out_cmp = strip_ansi(log_path.read_text(errors="replace")[pos:])
            print(f"      Comparison Output:\n{out_cmp.strip()}")
            assert "diskbench context mount=/mnt fs=ext2" in out_cmp, f"Missing context: {out_cmp}"
            assert "diskbench write bytes=131072" in out_cmp, f"Missing write metric: {out_cmp}"
            assert "diskbench read bytes=131072" in out_cmp, f"Missing read metric: {out_cmp}"
            assert "diskbench meta files=20" in out_cmp, f"Missing meta metric: {out_cmp}"
            print("  [PASS] Comparison-friendly output (-c) verified.")

            # Test 4: Silent / summary-only output (-s)
            print("  [5] Testing: diskbench -s -w 64K -n 10 /mnt")
            pos = len(log_path.read_text(errors="replace"))
            cmd4 = "diskbench -s -w 64K -n 10 /mnt\n"
            send_str(uart, cmd4)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd4) - 5, timeout=20)
            out_s = strip_ansi(log_path.read_text(errors="replace")[pos:])
            print(f"      Silent Output:\n{out_s.strip()}")
            assert "=== diskbench" not in out_s, f"Silent mode should not print banner: {out_s}"
            assert "write: 65536 bytes in" in out_s, f"Missing write line: {out_s}"
            assert "read:  65536 bytes in" in out_s, f"Missing read line: {out_s}"
            assert "meta:  10 files in" in out_s, f"Missing meta line: {out_s}"
            print("  [PASS] Silent/summary mode (-s) verified.")

            # Test 5: Verify cleanup (no leftover diskbench-tmp directory)
            print("  [6] Checking filesystem cleanup in /mnt...")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "ls /mnt\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out_ls = strip_ansi(log_path.read_text(errors="replace")[pos:])
            assert "diskbench-tmp" not in out_ls, f"Temporary directory not cleaned up:\n{out_ls}"
            print("  [PASS] Cleanup confirmed — no leftover test files.")

            # Test 6: Check /mnt-journaled availability
            print("  [7] Testing /mnt-journaled (if available)...")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "diskbench -w 64K -n 5 /mnt-journaled\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out_jnl = strip_ansi(log_path.read_text(errors="replace")[pos:])
            if "cannot create test file in /mnt-journaled" in out_jnl or "cannot" in out_jnl:
                print("      - /mnt-journaled not present on test fixture (expected for default ext2 NVMe).")
            else:
                print(f"      - /mnt-journaled benchmark output:\n{out_jnl.strip()}")

            print("\n>>> ALL DISKBENCH QEMU TESTS PASSED <<<")

        finally:
            stop_drain.set()
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    test_diskbench_qemu()
