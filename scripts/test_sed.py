#!/usr/bin/env python3
"""QEMU Integration Test for /bin/sed across BIOS and UEFI (SMP=1)."""
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


def run_sed_qemu(firmware="bios"):
    print(f"\n=== Running /bin/sed QEMU Integration Test ({firmware.upper()}, SMP=1) ===")
    assert ISO.is_file(), f"Bootable ISO missing at {ISO}"

    with tempfile.TemporaryDirectory(prefix=f"fortress-sed-{firmware}-") as tmp:
        tmp_dir = Path(tmp)
        uart_path = tmp_dir / "uart"
        log_path = tmp_dir / "serial.log"

        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", "1", "-display", "none",
            "-no-reboot", "-boot", "d", "-cdrom", str(ISO),
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log_path}",
            "-serial", "chardev:uart"
        ]

        if NVME_SRC.is_file():
            nvme_path = tmp_dir / "nvme.img"
            shutil.copyfile(NVME_SRC, nvme_path)
            qemu_cmd += [
                "-drive", f"file={nvme_path},if=none,id=nvm0,format=raw,snapshot=off",
                "-device", "nvme,serial=fortress0,drive=nvm0",
                "-fw_cfg", "name=opt/fortress/write_test,string=1",
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

            def get_tool_lines(raw_out, cmd_prefix=""):
                res = []
                for l in raw_out.splitlines():
                    s = l.strip()
                    if not s:
                        continue
                    if "@fortress" in s or "fortress:" in s or s.endswith("$") or s.endswith("#"):
                        continue
                    if cmd_prefix and cmd_prefix in s:
                        continue
                    res.append(s)
                return res

            print("  [1] Waiting for interactive shell prompt...")
            wait_for_prompt(0, timeout=40, require_newline=False)
            print("  [1] Shell prompt ready.")

            # Test 1: Basic substitution
            print("  [2] Testing: echo 'hello world' | sed 's/world/there/'")
            out = exec_cmd("echo \"hello world\" | sed 's/world/there/'")
            lines = get_tool_lines(out, "echo")
            assert any("hello there" in l for l in lines), f"Unexpected output for basic substitution: {lines}"
            print("  [PASS] Basic substitution verified.")

            # Test 2: Global substitution
            print("  [3] Testing: echo 'aaa' | sed 's/a/b/g'")
            out = exec_cmd("echo \"aaa\" | sed 's/a/b/g'")
            lines = get_tool_lines(out, "echo")
            assert any("bbb" in l for l in lines), f"Unexpected output for global substitution: {lines}"
            print("  [PASS] Global substitution verified.")

            # Test 3: Line addressing with -n and p
            print("  [4] Testing: printf 'line1\\nline2\\nline3\\n' | sed -n '2p'")
            out = exec_cmd("printf 'line1\\nline2\\nline3\\n' | sed -n '2p'")
            lines = get_tool_lines(out, "printf")
            assert "line2" in lines, f"Expected line2 in output lines: {lines}"
            assert "line1" not in lines and "line3" not in lines, f"Unexpected lines printed with -n 2p: {lines}"
            print("  [PASS] Line addressing with -n 2p verified.")

            # Test 4: Anchor ^ match
            print("  [5] Testing: printf 'foo\\nbar\\nbaz\\n' | sed 's/^/prefix-/'")
            out = exec_cmd("printf 'foo\\nbar\\nbaz\\n' | sed 's/^/prefix-/'")
            lines = get_tool_lines(out, "printf")
            assert "prefix-foo" in lines and "prefix-bar" in lines and "prefix-baz" in lines, f"Unexpected prefix output: {lines}"
            print("  [PASS] Anchor ^ match verified.")

            # Test 5: Range delete 1,2d
            print("  [6] Testing: printf 'a\\nb\\nc\\n' | sed '1,2d'")
            out = exec_cmd("printf 'a\\nb\\nc\\n' | sed '1,2d'")
            lines = get_tool_lines(out, "printf")
            assert "c" in lines, f"Expected 'c' in output lines: {lines}"
            assert "a" not in lines and "b" not in lines, f"Lines 'a' and 'b' should be deleted: {lines}"
            print("  [PASS] Range delete 1,2d verified.")

            # Test 6: Capture group \1
            print("  [7] Testing: echo 'hello' | sed 's/\\(hel\\)lo/\\1p/'")
            out = exec_cmd("echo \"hello\" | sed 's/\\(hel\\)lo/\\1p/'")
            lines = get_tool_lines(out, "echo")
            assert any("help" in l for l in lines), f"Expected 'help' in output: {lines}"
            print("  [PASS] Capture group \\1 verified.")

            # Test 7: Pipeline composition with other tools
            print("  [8] Testing pipeline: printf 'x\\nx\\ny\\n' | sed 's/x/z/' | sort | uniq")
            out = exec_cmd("printf 'x\\nx\\ny\\n' | sed 's/x/z/' | sort | uniq")
            lines = get_tool_lines(out, "printf")
            assert "y" in lines and "z" in lines, f"Expected 'y' and 'z' in pipeline output: {lines}"
            assert lines.count("z") == 1, f"Expected exactly one 'z' after uniq: {lines}"
            print("  [PASS] Pipeline composition verified.")

            # Test 8: Unsupported command rejection
            print("  [9] Testing unsupported command rejection: echo x | sed 'a test'")
            out = exec_cmd("echo x | sed 'a test'; echo STATUS:$?")
            assert "unsupported command 'a'" in out or "STATUS:2" in out or "STATUS:1" in out, f"Expected rejection diagnostic or non-zero status: {out}"
            assert "STATUS:0" not in out, f"Unsupported command should exit non-zero: {out}"
            print("  [PASS] Unsupported command rejection verified.")

            # Test 9: In-place editing if /mnt is writable
            print("  [10] Testing in-place file edit: sed -i")
            exec_cmd("echo \"alpha beta gamma\" > /tmp/sed_test.txt")
            exec_cmd("sed -i 's/beta/BETA/' /tmp/sed_test.txt")
            out = exec_cmd("cat /tmp/sed_test.txt")
            assert "alpha BETA gamma" in out, f"Expected in-place modification in /tmp/sed_test.txt: {out}"
            print("  [PASS] In-place file editing verified.")

            print(f"=== ALL /bin/sed QEMU tests PASSED for {firmware.upper()} ===")

        finally:
            stop_drain.set()
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    run_sed_qemu("bios")
    run_sed_qemu("uefi")
    print("\n>>> ALL /bin/sed QEMU ACCEPTANCE TESTS PASSED (BIOS & UEFI) <<<")
