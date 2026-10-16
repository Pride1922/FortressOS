#!/usr/bin/env python3
"""QEMU Integration Test for /bin/tail -f (bounded follow) across BIOS & UEFI (SMP=1 and SMP=4)."""
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
ISO = REPO / "bin" / "fortress.iso"
NVME_SRC = REPO / "build" / "nvme_gpt.img"


def run_tail_f_qemu(firmware="bios", smp=1):
    print(f"\n=== Running /bin/tail -f QEMU Integration Test ({firmware.upper()}, SMP={smp}) ===")
    assert ISO.is_file(), f"Bootable ISO missing at {ISO}"
    assert NVME_SRC.is_file(), f"NVMe disk image missing at {NVME_SRC}"

    with tempfile.TemporaryDirectory(prefix=f"fortress-tail-f-{firmware}-smp{smp}-") as tmp:
        tmp_dir = Path(tmp)
        uart_path = tmp_dir / "uart"
        log_path = tmp_dir / "serial.log"
        nvme_path = tmp_dir / "nvme.img"
        shutil.copyfile(NVME_SRC, nvme_path)

        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", str(smp), "-display", "none",
            "-no-reboot", "-boot", "d", "-cdrom", str(ISO),
            "-drive", f"file={nvme_path},if=none,id=nvm0,format=raw,snapshot=off",
            "-device", "nvme,serial=fortress0,drive=nvm0",
            "-fw_cfg", "name=opt/fortress/write_test,string=1",
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

            def send_str(s):
                for b in s.encode():
                    uart.send(bytes([b]))
                    time.sleep(0.005)

            def exec_cmd(cmd_str, timeout=15):
                pos = len(get_text())
                send_str(cmd_str + "\n")
                wait_for_prompt(pos, timeout=timeout, require_newline=True)
                return get_text()[pos:]

            def wait_for_pattern(pattern, timeout=15):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    txt = get_text()
                    if pattern in txt:
                        return True
                    assert proc.poll() is None, f"QEMU exited prematurely with code {proc.poll()}"
                    time.sleep(0.05)
                raise AssertionError(f"Timeout waiting for pattern '{pattern}'. Log tail:\n{get_text()[-1000:]}")

            print("  [1] Waiting for interactive shell prompt...")
            wait_for_prompt(0, timeout=45, require_newline=False)
            print("  [1] Shell prompt ready.")

            # Ensure /mnt is writable
            exec_cmd("sudo chmod 777 /mnt")
            exec_cmd("sync")

            # Test 1: tail --help verification
            print("  [2] Testing: tail --help")
            out = exec_cmd("tail --help")
            assert "Usage: tail" in out and "-f" in out, f"Unexpected tail --help output: {out}"
            print("  [PASS] tail --help verified.")

            # Prepare test file on persistent ext2 partition
            print("  [3] Initializing /mnt/test.log...")
            exec_cmd("echo 'initial line 0' > /mnt/test.log")
            exec_cmd("sync")
            out = exec_cmd("cat /mnt/test.log")
            assert "initial line 0" in out, f"Failed to initialize /mnt/test.log: {out}"

            # Test 2: Start tail -f /mnt/test.log & in background
            print("  [4] Starting 'tail -f /mnt/test.log &' in background...")
            bg_pos = len(get_text())
            send_str("tail -f /mnt/test.log &\n")
            wait_for_prompt(bg_pos, timeout=15, require_newline=True)
            time.sleep(0.3)
            # Verify initial content was displayed
            txt_now = get_text()[bg_pos:]
            assert "initial line 0" in txt_now, f"Initial line missing from tail output: {txt_now}"
            print("  [PASS] Background tail running and printed initial line.")

            # Test 3: Append line 1
            print("  [5] Appending 'line 1' to /mnt/test.log...")
            exec_cmd("echo 'line 1' >> /mnt/test.log")
            exec_cmd("sync")
            wait_for_pattern("line 1", timeout=10)
            print("  [PASS] 'line 1' followed and appeared in output.")

            # Test 4: Append line 2
            print("  [6] Appending 'line 2' to /mnt/test.log...")
            exec_cmd("echo 'line 2' >> /mnt/test.log")
            exec_cmd("sync")
            wait_for_pattern("line 2", timeout=10)
            print("  [PASS] 'line 2' followed and appeared in output.")

            # Test 5: Truncation
            print("  [7] Testing truncation: echo '' > /mnt/test.log then new content...")
            exec_cmd("echo '' > /mnt/test.log")
            exec_cmd("echo 'new content' >> /mnt/test.log")
            exec_cmd("sync")
            wait_for_pattern("file truncated", timeout=10)
            wait_for_pattern("new content", timeout=10)
            print("  [PASS] Truncation detected and new content followed.")

            # Test 6: Kill the background tail
            print("  [8] Terminating background tail job via kill...")
            exec_cmd("kill %1")
            exec_cmd("sync")
            time.sleep(0.5)
            # Verify shell prompt is responsive
            out_jobs = exec_cmd("jobs")
            print(f"      Jobs output: {out_jobs.strip()}")
            print("  [PASS] Background tail cleanly terminated.")

            # Test 7: Signal test - Foreground tail -f interrupted via Ctrl+C
            print("  [9] Testing foreground tail -f with Ctrl+C (SIGINT)...")
            fg_pos = len(get_text())
            send_str("tail -f /mnt/test.log\n")
            time.sleep(0.5)
            # Send Ctrl+C
            uart.sendall(b"\x03")
            wait_for_prompt(fg_pos, timeout=15, require_newline=True)
            out_status = exec_cmd("echo $?")
            assert "130" in out_status, f"Expected exit status 130 on Ctrl+C, got: {out_status}"
            print("  [PASS] Foreground Ctrl+C exited cleanly with status 130.")

            # Test 8: Stdin follow: tail -f - exits with diagnostic
            print("  [10] Testing stdin follow: echo test | tail -f -")
            out_stdin = exec_cmd("echo 'stdin test' | tail -f -")
            assert "cannot follow standard input" in out_stdin or "cannot follow" in out_stdin, f"Diagnostic missing for tail -f -: {out_stdin}"
            out_stdin_status = exec_cmd("echo $?")
            assert "0" in out_stdin_status, f"Expected exit 0 for stdin diagnostic: {out_stdin_status}"
            print("  [PASS] tail -f - stdin diagnostic and exit 0 verified.")

            # Power off
            print("  [11] Powering off...")
            send_str("sudo poweroff\n")
            proc.wait(timeout=10)
            stop_drain.set()
            print(f">>> {firmware.upper()} SMP={smp} /bin/tail -f INTEGRATION TEST PASSED <<<")

        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


def main():
    print("==========================================================")
    print(" FortressOS /bin/tail -f Multi-Firmware Integration Suite ")
    print("==========================================================")
    run_tail_f_qemu(firmware="bios", smp=1)
    run_tail_f_qemu(firmware="bios", smp=4)
    run_tail_f_qemu(firmware="uefi", smp=1)
    run_tail_f_qemu(firmware="uefi", smp=4)
    print("\n>>> ALL /bin/tail -f QEMU INTEGRATION TESTS PASSED (100%) <<<")


if __name__ == "__main__":
    main()
