#!/usr/bin/env python3
"""QEMU Integration Test for /bin/base64."""
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


def test_base64_qemu():
    print("=== Running base64 QEMU Integration Test ===")
    with tempfile.TemporaryDirectory(prefix="fortress-base64-qemu-") as tmp:
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
                        if not uart.recv(65536): return
                    except (socket.timeout, OSError):
                        continue
            threading.Thread(target=drain, daemon=True).start()

            def get_text():
                if not log_path.exists():
                    return ""
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", log_path.read_bytes().decode(errors="replace")).replace("\r", "")

            sent_login = False
            def wait_for_prompt(after_idx=0, timeout=40, require_newline=False):
                nonlocal sent_login
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    t = get_text()
                    if not sent_login and "FortressOS login: " in t[after_idx:]:
                        sent_login = True
                        uart.sendall(b"operator\n")
                    chunk = t[after_idx:]
                    if require_newline:
                        nl = chunk.find("\n")
                        if nl != -1 and re.search(PROMPT_PATTERN, chunk[nl+1:]):
                            return chunk
                    else:
                        if re.search(PROMPT_PATTERN, chunk):
                            return chunk
                    time.sleep(0.05)
                raise TimeoutError(f"Prompt not seen within {timeout}s: {t[-500:]}")

            def run_command(command, timeout=20):
                before = len(get_text())
                uart.sendall(command.encode("ascii") + b"\n")
                wait_for_prompt(after_idx=before, timeout=timeout, require_newline=True)
                return get_text()[before:]

            print("  [1] Waiting for initial Ring 3 shell prompt...")
            wait_for_prompt(timeout=40)
            print("  [1] Shell prompt ready.")

            # Test 1: base64 --help
            print("  [2] Testing: /bin/base64 --help")
            out = run_command("/bin/base64 --help")
            assert "Usage: base64" in out, f"Unexpected help output: {out}"
            print("  [PASS] base64 --help verified.")

            # Test 2: echo -n "Hello Fortress" | /bin/base64 -w0
            print("  [3] Testing: encode pipeline")
            out = run_command("echo -n \"Hello Fortress\" | /bin/base64 -w0")
            assert "SGVsbG8gRm9ydHJlc3M=" in out, f"Unexpected encode output: {out}"
            print("  [PASS] encode pipeline verified.")

            # Test 3: decode pipeline
            print("  [4] Testing: decode pipeline")
            out = run_command("echo -n \"SGVsbG8gRm9ydHJlc3M=\" | /bin/base64 -d")
            assert "Hello Fortress" in out, f"Unexpected decode output: {out}"
            print("  [PASS] decode pipeline verified.")

            # Test 4: roundtrip in pipeline
            print("  [5] Testing: roundtrip encode | decode pipeline")
            out = run_command("echo -n \"StreamRoundtrip42\" | /bin/base64 | /bin/base64 -d")
            assert "StreamRoundtrip42" in out, f"Unexpected roundtrip output: {out}"
            print("  [PASS] roundtrip pipeline verified.")

            # Test 5: file encoding
            print("  [6] Testing: file encoding")
            out = run_command("/bin/base64 -w0 /etc/motd")
            assert len(out.strip()) > 10, f"Expected non-empty base64 file output: {out}"
            print("  [PASS] file encoding verified.")

            # Test 6: invalid input error detection
            print("  [7] Testing: invalid input detection without -i")
            out = run_command("echo -n \"invalid!input\" | /bin/base64 -d")
            assert "invalid input" in out, f"Expected invalid input error: {out}"
            out_code = run_command("echo $?")
            assert "1" in out_code, f"Expected exit code 1: {out_code}"
            print("  [PASS] invalid input failure verified.")

            # Test 7: ignore garbage with -i
            print("  [8] Testing: ignore garbage with -i")
            out = run_command("echo -n 'SGVs!bG8g#Rm9y$dHJl%c3M=' | /bin/base64 -d -i")
            assert "Hello Fortress" in out, f"Expected Hello Fortress with -i: {out}"
            print("  [PASS] ignore garbage flag verified.")

            # Clean shutdown
            print("  [9] Testing clean poweroff...")
            uart.sendall(b"sudo poweroff\n")
            proc.wait(timeout=10)
            print("  [PASS] Clean poweroff.")

            print("\n>>> ALL BASE64 QEMU TESTS PASSED (100%) <<<")

        finally:
            stop_drain.set()
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired: proc.kill()


if __name__ == "__main__":
    test_base64_qemu()
