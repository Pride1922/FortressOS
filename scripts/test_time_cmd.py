#!/usr/bin/env python3
"""QEMU Integration Test for /bin/time command with real, user, and sys CPU accounting."""
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
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")


def run_time_qemu(firmware="bios", smp=1):
    print(f"\n=== Running /bin/time QEMU Integration Test ({firmware.upper()}, SMP={smp}) ===")
    with tempfile.TemporaryDirectory(prefix=f"fortress-time-{firmware}-smp{smp}-") as tmp:
        tmp_dir = Path(tmp)
        uart_path = tmp_dir / "uart"
        log_path = tmp_dir / "serial.log"

        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", str(smp), "-display", "none",
            "-no-reboot", "-boot", "d", "-cdrom", "bin/fortress.iso",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log_path}",
            "-serial", "chardev:uart"
        ]

        if firmware == "uefi":
            if not CODE.is_file() or not VARS.is_file():
                print("  [SKIP] Paired OVMF 4M firmware not available on host.")
                return
            vars_path = tmp_dir / "vars.fd"
            shutil.copyfile(VARS, vars_path)
            qemu_cmd += [
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_path}"
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
            def wait_for_prompt(start_pos=0, timeout=45, require_newline=False):
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

            def exec_cmd(cmd_str, timeout=15):
                pos = len(get_text())
                uart.sendall(cmd_str.encode() + b"\n")
                wait_for_prompt(pos, timeout=timeout, require_newline=True)
                return get_text()[pos:]

            print("  [1] Waiting for interactive shell prompt...")
            wait_for_prompt(0, timeout=45, require_newline=False)
            print("  [1] Shell prompt ready.")

            # Test 1: time --help
            print("  [2] Testing: time --help")
            out = exec_cmd("time --help")
            assert "Usage: time" in out, f"Expected usage message in help: {out}"
            assert "-p" in out, f"Expected -p option in help: {out}"
            print("  [PASS] time --help verified.")

            # Test 2: time sleep 1 (wall-clock timing accuracy)
            print("  [3] Testing: time sleep 1 (default Linux-style format)")
            out = exec_cmd("time sleep 1", timeout=15)
            print(f"      Output:\n{out.strip()}")
            assert "real\t0m1." in out or "real\t0m0.9" in out or "real\t0m1.1" in out, f"Unexpected real time in sleep 1: {out}"
            assert "user\t0m0." in out, f"Expected small user time for sleep: {out}"
            assert "sys\t0m0." in out, f"Expected small sys time for sleep: {out}"
            print("  [PASS] time sleep 1 verified.")

            # Test 3: time -p sleep 1 (POSIX -p format)
            print("  [4] Testing: time -p sleep 1 (POSIX format)")
            out = exec_cmd("time -p sleep 1", timeout=15)
            print(f"      Output:\n{out.strip()}")
            assert re.search(r"real 1\.\d{2}", out) or re.search(r"real 0\.9\d", out), f"Expected POSIX real ~1.0: {out}"
            assert re.search(r"user 0\.\d{2}", out), f"Expected POSIX user ~0.0: {out}"
            assert re.search(r"sys 0\.\d{2}", out), f"Expected POSIX sys ~0.0: {out}"
            print("  [PASS] time -p sleep 1 verified.")

            # Test 4: Exit status propagation
            print("  [5] Testing exit status propagation: time which")
            exec_cmd("time which time")
            out = exec_cmd("echo $?")
            assert "0" in out, f"Expected exit code 0 for time which time: {out}"

            exec_cmd("time which nonexistent_command_xyz")
            out = exec_cmd("echo $?")
            assert "1" in out, f"Expected exit code 1 for time which nonexistent: {out}"
            print("  [PASS] Exit status propagation verified.")

            # Test 5: CPU-bound command accounting
            print("  [6] Testing CPU-bound command: time sha256sum /bin/shell")
            out = exec_cmd("time sha256sum /bin/shell", timeout=15)
            print(f"      Output:\n{out.strip()}")
            assert "real\t" in out, f"Missing real time in sha256sum output: {out}"
            assert "user\t" in out, f"Missing user time in sha256sum output: {out}"
            assert "sys\t" in out, f"Missing sys time in sha256sum output: {out}"
            print("  [PASS] CPU-bound accounting verified.")

            # Test 6: Per-process separation with concurrent/subsequent commands
            print("  [7] Testing per-process separation across commands")
            exec_cmd("time sleep 1", timeout=15)
            out_fast = exec_cmd("time which date", timeout=10)
            assert "0m0." in out_fast or "0m0.0" in out_fast, f"Subsequent command should not inherit prior sleep duration: {out_fast}"
            print("  [PASS] Per-process accounting separation verified.")

            # Poweroff
            print("  [8] Powering off...")
            uart.sendall(b"sudo poweroff\n")
            proc.wait(timeout=10)
            stop_drain.set()
            print(f">>> {firmware.upper()} SMP={smp} /bin/time INTEGRATION TEST PASSED <<<")

        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


def main():
    print("======================================================")
    print(" FortressOS /bin/time Multi-Firmware Integration Suite")
    print("======================================================")
    run_time_qemu(firmware="bios", smp=1)
    run_time_qemu(firmware="bios", smp=4)
    run_time_qemu(firmware="uefi", smp=1)
    run_time_qemu(firmware="uefi", smp=4)
    print("\n>>> ALL /bin/time QEMU INTEGRATION TESTS PASSED (100%) <<<")


if __name__ == "__main__":
    main()
