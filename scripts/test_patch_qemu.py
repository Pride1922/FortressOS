#!/usr/bin/env python3
"""QEMU Integration Test for /bin/patch."""
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


def test_patch_qemu():
    print("=== Running patch QEMU Integration Test ===")
    with tempfile.TemporaryDirectory(prefix="fortress-patch-qemu-") as tmp:
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

            # Test 1: patch --help
            print("  [2] Testing: patch --help")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "patch --help\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = log_path.read_text(errors="replace")[pos:]
            assert "Usage: patch" in out, f"Unexpected output: {out}"
            print("  [PASS] patch --help succeeded.")

            # Test 2: diff -u pipeline into patch dry-run
            print("  [3] Testing: unified diff pipeline with patch --dry-run")
            pos = len(log_path.read_text(errors="replace"))
            cmd2 = "cat /etc/network.conf | head -n 3 | diff -u /etc/network.conf - | patch --dry-run /etc/network.conf\n"
            send_str(uart, cmd2)
            time.sleep(0.5)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd2) - 5, timeout=10)
            out = strip_ansi(log_path.read_text(errors="replace")[pos:])
            assert "Hunk #1 succeeded" in out, f"Expected Hunk #1 succeeded in dry-run: {out}"
            print("  [PASS] patch --dry-run pipeline succeeded.")

            # Test 3: patch -s -o - stdout streaming
            print("  [4] Testing: patch -s -o - /etc/network.conf streaming")
            pos = len(log_path.read_text(errors="replace"))
            cmd3 = "cat /etc/network.conf | head -n 3 | diff -u /etc/network.conf - | patch -s -o - /etc/network.conf\n"
            send_str(uart, cmd3)
            time.sleep(0.5)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd3) - 5, timeout=10)
            out = strip_ansi(log_path.read_text(errors="replace")[pos:])
            assert "gateway 10.0.2.2" in out, f"Expected gateway in output: {out}"
            assert "dns 10.0.2.3" not in out, f"Expected dns to be stripped: {out}"
            print("  [PASS] patch -s -o - pipeline stream succeeded.")

            # Poweroff
            send_str(uart, "poweroff\n")
            proc.wait(timeout=10)
            stop_drain.set()
            print(">>> ALL PATCH QEMU INTEGRATION TESTS PASSED <<<")

        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    test_patch_qemu()
