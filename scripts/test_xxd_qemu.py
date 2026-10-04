#!/usr/bin/env python3
"""QEMU Integration Test for /bin/xxd."""
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


def test_xxd_qemu():
    print("=== Running xxd QEMU Integration Test ===")
    with tempfile.TemporaryDirectory(prefix="fortress-xxd-qemu-") as tmp:
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

            # Test 1: xxd -l 16 on a file
            print("  [2] Testing: xxd -l 16 /etc/motd")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "xxd -l 16 /etc/motd\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            assert "00000000:" in out, f"Unexpected output: {out}"
            print("  [PASS] xxd -l 16 on file succeeded.")

            # Test 2: xxd -p in a pipeline
            print("  [3] Testing: head -c 8 /etc/motd | xxd -p")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "head -c 8 /etc/motd | xxd -p\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            # 8 bytes should produce 16 hex chars
            lines = [l.strip() for l in out.splitlines() if len(l.strip()) == 16 and all(c in "0123456789abcdefABCDEF" for c in l.strip())]
            assert len(lines) >= 1, f"Expected 16 hex characters in output: {out}"
            print("  [PASS] Pipeline head -c 8 | xxd -p succeeded.")

            # Test 3: xxd -r roundtrip in pipeline
            print("  [4] Testing: head -n 2 /etc/motd | xxd | xxd -r")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "head -n 2 /etc/motd | xxd | xxd -r\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            assert "Welcome to FortressOS" in out, f"Unexpected output in roundtrip: {out}"
            print("  [PASS] Pipeline xxd | xxd -r roundtrip succeeded.")

            # Test 4: xxd --help
            print("  [5] Testing: xxd --help")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "xxd --help\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            assert "Usage: xxd" in out, f"Unexpected output: {out}"
            print("  [PASS] xxd --help succeeded.")

            # Poweroff
            send_str(uart, "poweroff\n")
            proc.wait(timeout=10)
            stop_drain.set()
            print(">>> ALL XXD QEMU INTEGRATION TESTS PASSED <<<")

        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    test_xxd_qemu()
