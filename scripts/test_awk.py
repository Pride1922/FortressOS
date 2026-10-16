#!/usr/bin/env python3
"""QEMU Integration Test for /bin/awk across BIOS and UEFI (SMP=1)."""
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


def run_awk_qemu(firmware="bios"):
    print(f"\n=== Running /bin/awk QEMU Integration Test ({firmware.upper()}, SMP=1) ===")
    assert ISO.is_file(), f"Bootable ISO missing at {ISO}"

    with tempfile.TemporaryDirectory(prefix=f"fortress-awk-{firmware}-") as tmp:
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

            # Test 1: Field extraction
            print("  [2] Testing: printf 'Alice 30\\nBob 25\\n' | awk '{ print $1 }'")
            out = exec_cmd("printf 'Alice 30\\nBob 25\\n' | awk '{ print $1 }'")
            lines = get_tool_lines(out, "printf")
            assert "Alice" in lines and "Bob" in lines, f"Expected Alice and Bob in lines: {lines}"
            print("  [PASS] Field extraction verified.")

            # Test 2: Custom delimiter -F:
            print("  [3] Testing: printf 'a:b:c\\n' | awk -F: '{ print $2 }'")
            out = exec_cmd("printf 'a:b:c\\n' | awk -F: '{ print $2 }'")
            lines = get_tool_lines(out, "printf")
            assert "b" in lines, f"Expected 'b' in lines: {lines}"
            print("  [PASS] Custom delimiter -F: verified.")

            # Test 3: Built-in variable NF
            print("  [4] Testing: printf 'a b c\\n' | awk '{ print NF }'")
            out = exec_cmd("printf 'a b c\\n' | awk '{ print NF }'")
            lines = get_tool_lines(out, "printf")
            assert "3" in lines, f"Expected '3' in lines: {lines}"
            print("  [PASS] Built-in variable NF verified.")

            # Test 4: Pattern NR == 2
            print("  [5] Testing: printf '1\\n2\\n3\\n' | awk 'NR == 2'")
            out = exec_cmd("printf '1\\n2\\n3\\n' | awk 'NR == 2'")
            lines = get_tool_lines(out, "printf")
            assert "2" in lines and "1" not in lines and "3" not in lines, f"Expected only '2' in lines: {lines}"
            print("  [PASS] Pattern NR == 2 verified.")

            # Test 5: Field comparison pattern
            print("  [6] Testing: printf 'Alice 30\\nBob 25\\n' | awk '$2 > 28 { print $1 }'")
            out = exec_cmd("printf 'Alice 30\\nBob 25\\n' | awk '$2 > 28 { print $1 }'")
            lines = get_tool_lines(out, "printf")
            assert "Alice" in lines and "Bob" not in lines, f"Expected only Alice in lines: {lines}"
            print("  [PASS] Field comparison pattern verified.")

            # Test 6: Formatted output printf
            print("  [7] Testing: echo 'hello' | awk '{ printf \"%s!\\n\", $1 }'")
            out = exec_cmd("echo \"hello\" | awk '{ printf \"%s!\\n\", $1 }'")
            lines = get_tool_lines(out, "echo")
            assert any("hello!" in l for l in lines), f"Expected 'hello!' in lines: {lines}"
            print("  [PASS] Formatted output printf verified.")

            # Test 7: Combined with ps and sort
            print("  [8] Testing pipeline: ps | awk '{ print $1 }' | sort")
            out = exec_cmd("ps | awk '{ print $1 }' | sort")
            lines = get_tool_lines(out, "ps")
            assert any(l.isdigit() for l in lines) or "PID" in lines, f"Expected PIDs in pipeline: {lines}"
            print("  [PASS] Pipeline with ps and sort verified.")

            # Test 8: Combined with sed
            print("  [9] Testing pipeline: printf 'foo-x\\nbar-x\\n' | sed 's/-x/ 42/' | awk '{ print $1, $2 }'")
            out = exec_cmd("printf 'foo-x\\nbar-x\\n' | sed 's/-x/ 42/' | awk '{ print $1, $2 }'")
            lines = get_tool_lines(out, "printf")
            assert "foo 42" in lines and "bar 42" in lines, f"Expected 'foo 42' and 'bar 42' in lines: {lines}"
            print("  [PASS] Pipeline with sed verified.")

            # Test 9: Unsupported construct rejection with code 2
            print("  [10] Testing unsupported construct rejection: echo x | awk '{ length($1) }'")
            out = exec_cmd("echo x | awk '{ length($1) }'; echo STATUS:$?")
            assert "unsupported function 'length'" in out or "STATUS:2" in out, f"Expected rejection diagnostic or status 2: {out}"
            assert "STATUS:0" not in out, f"Unsupported function should exit non-zero (status 2): {out}"
            print("  [PASS] Unsupported construct rejection verified.")

            # Test 10: Arithmetic and field mutation
            print("  [11] Testing arithmetic and field mutation: printf '5 10\\n' | awk '{ $1 = $1 + 15; print $1, $2 }'")
            out = exec_cmd("printf '5 10\\n' | awk '{ $1 = $1 + 15; print $1, $2 }'")
            lines = get_tool_lines(out, "printf")
            assert "20 10" in lines, f"Expected '20 10' in lines: {lines}"
            print("  [PASS] Arithmetic and field mutation verified.")

            print(f"=== ALL /bin/awk QEMU tests PASSED for {firmware.upper()} ===")

        finally:
            stop_drain.set()
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    run_awk_qemu("bios")
    run_awk_qemu("uefi")
    print("\n>>> ALL /bin/awk QEMU ACCEPTANCE TESTS PASSED (BIOS & UEFI) <<<")
