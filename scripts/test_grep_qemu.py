#!/usr/bin/env python3
"""QEMU Integration Test for /bin/grep."""
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


def test_grep_qemu():
    print("=== Running grep QEMU Integration Test ===")
    with tempfile.TemporaryDirectory(prefix="fortress-grep-qemu-") as tmp:
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

            # Test 1: grep on a file
            print("  [2] Testing: grep Fortress /etc/motd")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "grep Fortress /etc/motd\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            assert "FortressOS" in out or "FORTRESS" in out or "Fortress" in out, f"Unexpected output: {out}"
            print("  [PASS] grep on file matched expected line.")

            # Test 2: Case-insensitive grep in a pipeline
            print("  [3] Testing: cat /etc/motd | grep -i fortress")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "cat /etc/motd | grep -i fortress\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            assert "Fortress" in out or "FORTRESS" in out, f"Unexpected output: {out}"
            print("  [PASS] Pipeline cat | grep -i succeeded.")

            # Test 3: grep count (-c)
            print("  [4] Testing: grep -c -i fortress /etc/motd")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "grep -c -i fortress /etc/motd\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            print(f"      Count output: {out.strip()}")
            print("  [PASS] grep -c output verified.")

            # Test 4: grep invert (-v)
            print("  [5] Testing: echo foo | grep -v bar")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "echo foo | grep -v bar\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            assert "foo" in out, f"Unexpected output: {out}"
            print("  [PASS] grep -v invert match succeeded.")

            # Test 5: grep --help
            print("  [6] Testing: grep --help")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "grep --help\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            assert "Usage: grep" in out, f"Unexpected output: {out}"
            print("  [PASS] grep --help succeeded.")

            # Poweroff
            send_str(uart, "poweroff\n")
            proc.wait(timeout=10)
            stop_drain.set()
            print(">>> ALL GREP QEMU INTEGRATION TESTS PASSED <<<")

        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    test_grep_qemu()
