#!/usr/bin/env python3
"""QEMU Integration Test for /bin/free."""
from pathlib import Path
import os
import re
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

PROMPT_PATTERN = r"(?:fortress> |(?:\[-?\d+\] )?fortress:[^\r\n]* \$ |\[[a-zA-Z0-9_\-\./]+\]# |[A-Za-z0-9_-]+@[A-Za-z0-9-]+:[^\r\n]*[#$] )"


def test_free_qemu():
    print("=== Running free QEMU Integration Test ===")
    with tempfile.TemporaryDirectory(prefix="fortress-free-qemu-") as tmp:
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
                        if nl != -1 and re.search(PROMPT_PATTERN, chunk[nl+1:]):
                            return txt
                    else:
                        if re.search(PROMPT_PATTERN, chunk):
                            return txt
                    assert proc.poll() is None, f"QEMU exited prematurely with code {proc.poll()}"
                    time.sleep(0.05)
                full = get_text()
                raise AssertionError(f"Timeout waiting for prompt after pos {start_pos}. Tail:\n{full[-1000:]}")

            def exec_cmd(cmd_str, timeout=10):
                pos = len(get_text())
                uart.sendall(cmd_str.encode() + b"\n")
                wait_for_prompt(pos, timeout=timeout, require_newline=True)
                return get_text()[pos:]

            print("  [1] Waiting for interactive shell prompt...")
            wait_for_prompt(0, timeout=40, require_newline=False)
            print("  [1] Shell prompt ready.")

            # Test 1: free (default human output)
            print("  [2] Testing: free (default human units)")
            out = exec_cmd("free")
            assert "total" in out and "used" in out and "free" in out, f"Missing header columns: {out}"
            assert "Mem:" in out, f"Missing Mem: row in output: {out}"
            assert "Swap:" in out, f"Missing Swap: row in output: {out}"
            assert "Gi" in out or "Mi" in out, f"Expected human unit suffix (Gi or Mi): {out}"
            print("  [PASS] free default output verified.")

            # Test 2: free -m (mebibytes)
            print("  [3] Testing: free -m")
            out = exec_cmd("free -m")
            m = re.search(r"Mem:\s+(\d+)\s+(\d+)\s+(\d+)", out)
            assert m is not None, f"Could not parse numeric Mem row in free -m: {out}"
            total_m = int(m.group(1))
            assert 1800 <= total_m <= 2048, f"Unexpected total memory {total_m} MiB in 2G VM: {out}"
            print(f"  [PASS] free -m verified (total={total_m} MiB).")

            # Test 3: free -k (kibibytes)
            print("  [4] Testing: free -k")
            out = exec_cmd("free -k")
            m_k = re.search(r"Mem:\s+(\d+)\s+(\d+)\s+(\d+)", out)
            assert m_k is not None, f"Could not parse numeric Mem row in free -k: {out}"
            total_k = int(m_k.group(1))
            assert total_k > 1800000, f"Unexpected total memory {total_k} KiB: {out}"
            print(f"  [PASS] free -k verified (total={total_k} KiB).")

            # Test 4: free --help
            print("  [5] Testing: free --help")
            out = exec_cmd("free --help")
            assert "usage: free" in out.lower(), f"Unexpected help output: {out}"
            assert "--human" in out or "-h" in out, f"Missing --human in help: {out}"
            print("  [PASS] free --help verified.")

            # Test 5: Pipeline free | grep Mem
            print("  [6] Testing pipeline: free | grep Mem")
            out = exec_cmd("free | grep Mem")
            assert "Mem:" in out, f"Expected Mem: in filtered pipeline: {out}"
            print("  [PASS] Pipeline free | grep Mem verified.")

            # Poweroff
            print("  [7] Powering off...")
            uart.sendall(b"sudo poweroff\n")
            proc.wait(timeout=10)
            stop_drain.set()
            print(">>> ALL FREE QEMU INTEGRATION TESTS PASSED <<<")

        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    test_free_qemu()
