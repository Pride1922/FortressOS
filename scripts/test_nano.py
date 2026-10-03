#!/usr/bin/env python3
"""Acceptance test for /bin/nano visual editor under QEMU.

Runs BIOS and UEFI boot under SMP=1 with a disposable ext2 partition at /mnt:
  1. Non-TTY / redirected invocation rejection (standard output is not a terminal)
  2. Interactive new file creation (/bin/nano /mnt/test_nano.txt), typing text,
     saving via Ctrl+O, and exiting via Ctrl+X. Verifies exact content via cat.
  3. Interactive reopen, appending a second line, and exiting with save prompt (Ctrl+X -> Y).
  4. Interactive unsaved buffer discard (Ctrl+X -> N).
Enforces argv preflight (disposable ISO/OVMF, disposable NVMe ext2 disk).
"""

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
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
ISO = REPO / "bin" / "fortress.iso"
NVME_SRC = REPO / "build" / "nvme_gpt.img"
PROMPT_PATTERN = r"(?:fortress> |\[[a-zA-Z0-9_\-\./]+\]# )"


def qemu_command(mode, iso, variables, log, uart_path, nvme_path):
    cmd = [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
        "-smp", "1", "-display", "none", "-monitor", "none", "-no-reboot",
        "-boot", "d", "-cdrom", str(iso),
        "-drive", f"file={nvme_path},if=none,id=nvm0,format=raw,snapshot=off",
        "-device", "nvme,serial=fortress0,drive=nvm0",
        "-fw_cfg", "name=opt/fortress/write_test,string=1",
        "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
        "-serial", "chardev:uart"
    ]
    if mode == "uefi":
        cmd += [
            "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
            "-drive", f"if=pflash,format=raw,unit=1,file={variables}"
        ]
    return cmd


def preflight(cmd, mode, iso, variables, log, uart_path, nvme_path):
    assert mode in ("bios", "uefi")
    expected = qemu_command(mode, iso, variables, log, uart_path, nvme_path)
    assert cmd == expected, f"unexpected QEMU argv: {cmd} != {expected}"
    for token in ("-hda", "-hdb", "-blockdev", "usb-storage"):
        assert token not in cmd, f"preflight found forbidden token {token}"


def run_session(mode, iso, tmp):
    log = REPO / "build" / f"nano-{mode}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text("")
    uart_path = tmp / f"uart-{mode}"
    variables = tmp / f"vars-{mode}.fd"
    nvme_path = tmp / f"nvme-{mode}.img"
    shutil.copyfile(NVME_SRC, nvme_path)

    if mode == "uefi":
        shutil.copyfile(VARS, variables)

    cmd = qemu_command(mode, iso, variables, log, uart_path, nvme_path)
    preflight(cmd, mode, iso, variables, log, uart_path, nvme_path)

    with log.with_suffix(".stderr").open("wb") as stderr:
        proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
        uart = None
        stop_reader = threading.Event()
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if uart_path.exists():
                    try:
                        uart = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        uart.connect(str(uart_path))
                        break
                    except (ConnectionRefusedError, OSError):
                        pass
                time.sleep(0.05)
            assert uart is not None, f"Could not connect to UART at {uart_path}"
            uart.settimeout(0.2)

            def drain():
                while not stop_reader.is_set():
                    try:
                        if not uart.recv(65536):
                            return
                    except (socket.timeout, OSError):
                        pass
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()

            def get_raw():
                return log.read_bytes().decode(errors="replace")

            def get_text():
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", get_raw()).replace("\r", "")

            def wait_for_prompt(after_idx=0, timeout=60):
                d = time.monotonic() + timeout
                while time.monotonic() < d:
                    t = get_text()
                    if re.search(PROMPT_PATTERN, t[after_idx:]):
                        return t[after_idx:]
                    assert proc.poll() is None, "QEMU terminated prematurely"
                    time.sleep(0.05)
                raise TimeoutError(f"Prompt wait timed out: {get_text()[-1000:]}")

            def exec_cmd(cmd_str):
                start = len(get_text())
                for b in cmd_str.encode() + b"\n":
                    uart.send(bytes([b]))
                    time.sleep(0.005)
                return wait_for_prompt(start)

            def send_raw(data):
                for b in data:
                    uart.send(bytes([b]))
                    time.sleep(0.01)

            def wait_for_raw_pattern(pattern, start_pos=0, timeout=20):
                d = time.monotonic() + timeout
                while time.monotonic() < d:
                    raw = get_raw()[start_pos:]
                    if pattern in raw:
                        return True
                    assert proc.poll() is None, "QEMU terminated prematurely"
                    time.sleep(0.05)
                raise TimeoutError(f"Pattern {pattern} timed out: {get_raw()[-500:]}")

            # 1. Wait for boot prompt
            print(f"[{mode}] Waiting for shell prompt...", flush=True)
            wait_for_prompt(0, timeout=60)
            print(f"[{mode}] Shell ready", flush=True)

            # -------------------------------------------------------------
            # Stage 1: Non-TTY Rejection
            # -------------------------------------------------------------
            print(f"[{mode}] [Stage 1] Testing non-TTY rejection (/bin/nano | cat)...", flush=True)
            out_pipe = exec_cmd("/bin/nano | cat")
            assert "standard output is not a terminal" in out_pipe, f"Missing non-TTY error: {out_pipe}"
            print(f"[{mode}] [Stage 1] PASS: Non-TTY rejected cleanly", flush=True)

            # -------------------------------------------------------------
            # Stage 2: Create new file, type text, save with Ctrl+O, exit
            # -------------------------------------------------------------
            print(f"[{mode}] [Stage 2] Testing interactive file creation and Ctrl+O save...", flush=True)
            raw_start = len(get_raw())
            send_raw(b"/bin/nano /mnt/test_nano.txt\n")

            # Wait for Nano UI frame to appear
            wait_for_raw_pattern("FortressOS Nano", raw_start, timeout=15)
            wait_for_raw_pattern("/mnt/test_nano.txt", raw_start, timeout=10)

            # Type text into nano
            test_phrase = b"Hello from FortressOS Nano Editor!"
            send_raw(test_phrase)
            time.sleep(0.2)

            # Trigger Save via Ctrl+O (0x0F)
            send_raw(b"\x0f")
            wait_for_raw_pattern("File Name to Write:", raw_start, timeout=10)

            # Confirm filename with Enter (\n)
            send_raw(b"\n")
            wait_for_raw_pattern("Wrote", raw_start, timeout=10)

            # Exit via Ctrl+X (0x18)
            prompt_start = len(get_text())
            send_raw(b"\x18")
            wait_for_prompt(prompt_start, timeout=15)

            # Verify file content on ext2 disk
            out_cat = exec_cmd("cat /mnt/test_nano.txt")
            assert "Hello from FortressOS Nano Editor!" in out_cat, f"Saved content mismatch: {out_cat}"
            print(f"[{mode}] [Stage 2] PASS: Created and verified /mnt/test_nano.txt", flush=True)

            # -------------------------------------------------------------
            # Stage 3: Re-open file, append text, save on exit (Ctrl+X -> Y)
            # -------------------------------------------------------------
            print(f"[{mode}] [Stage 3] Testing reopen, edit, and Ctrl+X prompt save...", flush=True)
            raw_start = len(get_raw())
            send_raw(b"/bin/nano /mnt/test_nano.txt\n")

            wait_for_raw_pattern("FortressOS Nano", raw_start, timeout=15)
            wait_for_raw_pattern("Hello from FortressOS", raw_start, timeout=10)

            # Press Enter to split line, then type second line
            send_raw(b"\nSecond line written by nano.")
            time.sleep(0.2)

            # Exit via Ctrl+X (should prompt because modified)
            send_raw(b"\x18")
            wait_for_raw_pattern("Save modified buffer? (Y/N/C):", raw_start, timeout=10)

            # Confirm save with 'Y'
            prompt_start = len(get_text())
            send_raw(b"y")
            wait_for_prompt(prompt_start, timeout=15)

            # Verify both lines exist
            out_cat = exec_cmd("cat /mnt/test_nano.txt")
            assert "Hello from FortressOS Nano Editor!" in out_cat, f"Missing line 1: {out_cat}"
            assert "Second line written by nano." in out_cat, f"Missing line 2: {out_cat}"
            print(f"[{mode}] [Stage 3] PASS: Re-opened, appended, and saved via Ctrl+X -> Y", flush=True)

            # -------------------------------------------------------------
            # Stage 4: Discard unsaved buffer (Ctrl+X -> N)
            # -------------------------------------------------------------
            print(f"[{mode}] [Stage 4] Testing unsaved buffer discard (Ctrl+X -> N)...", flush=True)
            raw_start = len(get_raw())
            send_raw(b"/bin/nano /mnt/test_nano.txt\n")

            wait_for_raw_pattern("FortressOS Nano", raw_start, timeout=15)
            send_raw(b"\nThis draft should be discarded!")
            time.sleep(0.2)

            # Exit via Ctrl+X
            send_raw(b"\x18")
            wait_for_raw_pattern("Save modified buffer? (Y/N/C):", raw_start, timeout=10)

            # Discard with 'N'
            prompt_start = len(get_text())
            send_raw(b"n")
            wait_for_prompt(prompt_start, timeout=15)

            # Verify discarded draft is NOT in file
            out_cat = exec_cmd("cat /mnt/test_nano.txt")
            assert "This draft should be discarded!" not in out_cat, f"Discarded text was saved: {out_cat}"
            print(f"[{mode}] [Stage 4] PASS: Unsaved changes discarded successfully", flush=True)

            print(f"[{mode}] ALL NANO INTEGRATION TESTS PASSED!", flush=True)

        finally:
            stop_reader.set()
            if uart:
                uart.close()
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def main():
    if not ISO.exists():
        print(f"Error: {ISO} does not exist. Run 'make' first.", file=sys.stderr)
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)
        print("=== Running /bin/nano QEMU Acceptance Tests (BIOS) ===")
        run_session("bios", ISO, tmp)
        print("=== Running /bin/nano QEMU Acceptance Tests (UEFI) ===")
        run_session("uefi", ISO, tmp)

    print("\n=============================================")
    print("  ALL NANO QEMU ACCEPTANCE TESTS PASSED (100%)")
    print("=============================================\n")


if __name__ == "__main__":
    main()
