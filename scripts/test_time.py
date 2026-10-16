#!/usr/bin/env python3
"""QEMU Integration Test for /bin/date and /bin/sleep."""
from pathlib import Path
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from test_nmi_transitions import connect

PROMPT_PATTERN = r"(?:fortress> |(?:\[-?\d+\] )?fortress:[^\r\n]* \$ |\[[a-zA-Z0-9_\-\./]+\]# |[A-Za-z0-9_-]+@[A-Za-z0-9-]+:[^\r\n]*[#$] )"


def test_time_qemu():
    print("=== Running Date & Sleep QEMU Integration Test ===")
    with tempfile.TemporaryDirectory(prefix="fortress-time-qemu-") as tmp:
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
                        if not uart.recv(4096):
                            return
                    except (socket.timeout, OSError):
                        continue

            threading.Thread(target=drain, daemon=True).start()

            def get_text():
                if not log_path.exists():
                    return ""
                raw = log_path.read_text(errors="replace")
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", raw).replace("\r", "")

            sent_login = False
            def wait_for_prompt(start_pos=0, timeout=40, require_newline=False):
                nonlocal sent_login
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    txt = get_text()
                    chunk = txt[start_pos:]
                    if not sent_login and "FortressOS login: " in chunk:
                        sent_login = True
                        uart.sendall(b"operator\n")
                    if require_newline:
                        nl = chunk.find("\n")
                        if nl != -1 and re.search(r"(?:" + PROMPT_PATTERN + r")\s*\Z", chunk[nl+1:]):
                            return txt
                    else:
                        if re.search(r"(?:" + PROMPT_PATTERN + r")\s*\Z", chunk):
                            return txt
                    assert proc.poll() is None, f"QEMU exited prematurely with code {proc.poll()}"
                    time.sleep(0.05)
                full = get_text()
                raise AssertionError(f"Timeout waiting for prompt after pos {start_pos}. Tail:\n{full[-1000:]}")

            def exec_cmd(cmd_str, timeout=10):
                pos = len(get_text())
                t0 = time.monotonic()
                uart.sendall(cmd_str.encode() + b"\n")
                wait_for_prompt(pos, timeout=timeout, require_newline=True)
                elapsed = time.monotonic() - t0
                return get_text()[pos:], elapsed

            print("  [1] Waiting for interactive shell prompt...")
            wait_for_prompt(0, timeout=40, require_newline=False)
            print("  [1] Shell prompt ready.")

            # Test 1: date default
            print("  [2] Testing: date (default UTC format)")
            out, _ = exec_cmd("date")
            print(f"      Output: {out.strip()}")
            assert "UTC" in out, f"Expected UTC in date output: {out}"
            assert re.search(r"20\d{2}", out), f"Expected year >= 2000 in date output: {out}"
            print("  [PASS] date default verified.")

            # Test 2: date -I (ISO-8601)
            print("  [3] Testing: date -I (ISO-8601)")
            out, _ = exec_cmd("date -I")
            print(f"      Output: {out.strip()}")
            assert re.search(r"20\d{2}-\d{2}-\d{2}", out), f"Expected YYYY-MM-DD in date -I output: {out}"
            print("  [PASS] date -I verified.")

            # Test 3: date +%s (Unix epoch seconds)
            print("  [4] Testing: date +%s")
            out, _ = exec_cmd("date +%s")
            print(f"      Output: {out.strip()}")
            m = re.search(r"\b(\d{10,})\b", out)
            assert m, f"Expected 10+ digit epoch in date +%s: {out}"
            epoch = int(m.group(1))
            assert epoch >= 1726500000, f"Epoch too small: {epoch}"
            print(f"  [PASS] date +%s verified ({epoch}s).")

            # Test 4: date -R (RFC-2822)
            print("  [5] Testing: date -R")
            out, _ = exec_cmd("date -R")
            print(f"      Output: {out.strip()}")
            assert "+0000" in out or "UTC" in out, f"Expected RFC-2822 timezone in date -R: {out}"
            print("  [PASS] date -R verified.")

            # Test 5: sleep 1 (timing accuracy)
            print("  [6] Testing: sleep 1")
            out, elapsed = exec_cmd("sleep 1", timeout=15)
            print(f"      Output: {repr(out)}")
            print(f"      Elapsed time: {elapsed:.2f}s")
            assert elapsed >= 0.8, f"sleep 1 returned too fast: {elapsed}s"
            assert elapsed <= 4.0, f"sleep 1 took too long: {elapsed}s"
            print("  [PASS] sleep 1 timing verified.")

            # Test 6: sleep compound arguments
            print("  [7] Testing: sleep 0.5 0.5")
            out, elapsed = exec_cmd("sleep 0.5 0.5", timeout=15)
            print(f"      Elapsed time: {elapsed:.2f}s")
            assert elapsed >= 0.8, f"sleep 0.5 0.5 returned too fast: {elapsed}s"
            print("  [PASS] sleep compound arguments verified.")

            # Test 7: sleep error on missing operands
            print("  [8] Testing: sleep error handling")
            out, _ = exec_cmd("sleep")
            assert "missing operand" in out.lower() or "usage" in out.lower(), f"Expected missing operand error: {out}"
            print("  [PASS] sleep missing operand verified.")

            # Clean shutdown
            print("  [9] Shutting down guest...")
            uart.sendall(b"sudo poweroff\n")
            proc.wait(timeout=10)
            stop_drain.set()
            print("=== Date & Sleep Integration Test PASSED (100%) ===")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    test_time_qemu()
