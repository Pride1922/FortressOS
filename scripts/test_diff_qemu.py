#!/usr/bin/env python3
"""QEMU Integration Test for /bin/diff."""
from pathlib import Path
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from test_nmi_transitions import connect
import re


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


def test_diff_qemu():
    print("=== Running diff QEMU Integration Test ===")
    with tempfile.TemporaryDirectory(prefix="fortress-diff-qemu-") as tmp:
        tmp_dir = Path(tmp)
        uart_path = tmp_dir / "uart"
        log_path = tmp_dir / "serial.log"

        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", "1", "-display", "none",
            "-no-reboot", "-boot", "d", "-cdrom", "bin/fortress.iso",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log_path}",
            "-serial", "chardev:uart"
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
            wait_for_pattern(log_path, "fortress:/ $ ", proc, timeout=35)
            print("  [1] Shell prompt ready.")

            # Test 1: diff identical file
            print("  [2] Testing: diff /etc/motd /etc/motd")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "diff /etc/motd /etc/motd\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = strip_ansi(log_path.read_text(errors="replace")[pos:])
            lines = [l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith("diff") and not l.strip().startswith("fortress:")]
            assert len(lines) == 0, f"Expected no diff output on identical files: {out!r} -> lines: {lines!r}"
            print("  [PASS] diff on identical files succeeded (no output).")

            # Test 2: diff -q brief on different files
            print("  [3] Testing: diff -q /etc/motd /etc/network.conf")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "diff -q /etc/motd /etc/network.conf\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = strip_ansi(log_path.read_text(errors="replace")[pos:])
            assert "Files /etc/motd and /etc/network.conf differ" in out, f"Unexpected brief output: {out}"
            print("  [PASS] diff -q succeeded.")

            # Test 3: diff pipeline with stdin
            print("  [4] Testing: cat /etc/motd | diff /etc/motd -")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "cat /etc/motd | diff /etc/motd -\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = strip_ansi(log_path.read_text(errors="replace")[pos:])
            lines = [l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith("cat") and not l.strip().startswith("fortress:")]
            assert len(lines) == 0, f"Expected no output on identical stdin diff: {out}"
            print("  [PASS] Pipeline cat | diff file - succeeded.")

            # Test 4: diff -u unified diff in pipeline
            print("  [5] Testing: cat /etc/motd | head -n 2 | diff -u /etc/motd -")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "cat /etc/motd | head -n 2 | diff -u /etc/motd -\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            assert "--- /etc/motd" in out, f"Expected --- in unified diff: {out}"
            assert "+++ -" in out, f"Expected +++ in unified diff: {out}"
            assert "@@ -" in out, f"Expected @@ in unified diff: {out}"
            print("  [PASS] Pipeline unified diff -u succeeded.")

            # Test 5: diff --help
            print("  [6] Testing: diff --help")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "diff --help\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            assert "Usage: diff" in out, f"Unexpected output: {out}"
            print("  [PASS] diff --help succeeded.")

            # Poweroff
            send_str(uart, "poweroff\n")
            proc.wait(timeout=10)
            stop_drain.set()
            print(">>> ALL DIFF QEMU INTEGRATION TESTS PASSED <<<")

        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    test_diff_qemu()
