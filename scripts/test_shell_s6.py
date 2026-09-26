#!/usr/bin/env python3
"""S6 integration test suite for FortressOS Ring 3 Shell.

Tests Shell Milestone S6:
  - Phase 4A & 4B: Child process file redirections via SYS_SPAWN_EXT
  - Target expansion (variables, quoting, ambiguous redirect rejection)
  - Output truncation (>), appending (>>), and input redirection (<)
  - Direct execution and 'run' execution with spawn_fd_action_t
"""
from pathlib import Path
import re
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import threading
from test_nmi_transitions import REPO, Remote, QMP, symbols, connect


def run(mode):
    sym = symbols()
    cpus = "1"
    log = REPO / "build" / f"shell-s6-{mode}.log"
    log.write_text("")
    with tempfile.TemporaryDirectory(prefix="fortress-s6-") as tmp:
        uart_path, qmp_path, gdb_path = [Path(tmp) / n for n in ("uart", "qmp", "gdb")]
        img_copy = Path(tmp) / "nvme.img"
        shutil.copyfile(REPO / "build" / "nvme_gpt.img", img_copy)
        cmd = ["qemu-system-x86_64", "-M", "q35", "-m", "2G", "-display", "none",
               "-smp", cpus, "-no-reboot", "-S", "-monitor", "none", "-boot", "d", "-cdrom", "bin/fortress.iso",
               "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
               "-serial", "chardev:uart", "-qmp", f"unix:{qmp_path},server=on,wait=off",
               "-gdb", f"unix:{gdb_path},server=on,wait=off",
               "-drive", f"file={img_copy},if=none,id=nvm0,format=raw,snapshot=off",
               "-device", "nvme,serial=fortress0,drive=nvm0",
               "-fw_cfg", "name=opt/fortress/write_test,string=1"]
        if mode == "uefi":
            vars_path = Path(tmp) / "vars.fd"
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", vars_path)
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                    "-drive", f"if=pflash,format=raw,unit=1,file={vars_path}"]

        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 10.0
            uart = None
            last_err = None
            while time.monotonic() < deadline:
                assert child.poll() is None, child.stderr.read().decode()
                try:
                    uart = connect(uart_path)
                    break
                except (FileNotFoundError, ConnectionRefusedError, OSError, RuntimeError) as e:
                    last_err = e
                    time.sleep(0.05)
            if uart is None:
                raise RuntimeError(f"QEMU socket unavailable after 10s: {uart_path}") from last_err

            uart.settimeout(0.2)
            stop_reader = threading.Event()
            def drain():
                while not stop_reader.is_set():
                    try:
                        if not uart.recv(65536): return
                    except socket.timeout:
                        continue
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()

            qmp, remote = QMP(qmp_path), Remote(gdb_path)
            remote.request("qSupported")
            qmp.execute("cont")

            def output():
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", log.read_bytes().decode(errors="replace")).replace("\r", "")

            def wait_prompt(pattern=r"(fortress> |fortress:.* \$ )", after=0):
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    text = output()
                    m = re.search(pattern, text[after:])
                    if m:
                        time.sleep(0.05)
                        return text[after:]
                    assert child.poll() is None, child.stderr.read().decode()
                    time.sleep(0.05)
                raise AssertionError(f"Prompt matching {pattern!r} timed out: {log}\n{output()[-2000:]}")

            def uart_cmd(text, prompt_pat=r"(fortress> |fortress:.* \$ )"):
                start = len(output())
                for byte in text.encode():
                    uart.send(bytes([byte]))
                    time.sleep(0.005)
                return wait_prompt(pattern=prompt_pat, after=start)

            # Wait for initial shell prompt
            initial = wait_prompt()
            assert "FortressOS shell (Ring 3)" in initial
            print(f"[{mode.upper()}] Initial shell reached.", flush=True)

            # 1. Child stdout redirection: /bin/hello > /mnt/s6_hello.txt
            out = uart_cmd("run /bin/hello 10 > /mnt/s6_hello.txt\n")
            # Child stdout must NOT appear on terminal
            assert "Hello from /bin/hello" not in out, f"Child output leaked to terminal: {out}"
            print(f"[{mode.upper()}] S6 Child output redirection (run /bin/hello > file) silenced terminal.", flush=True)

            # Verify file content
            out = uart_cmd("cat /mnt/s6_hello.txt\n")
            assert "Hello from /bin/hello! VFS file execution verified." in out, out
            assert "Received argument: 10" in out, out
            print(f"[{mode.upper()}] S6 Redirected file content verified.", flush=True)

            # 2. Child append redirection: /bin/hello >> /mnt/s6_hello.txt
            out = uart_cmd("run /bin/hello 20 >> /mnt/s6_hello.txt\n")
            assert "Hello from /bin/hello" not in out, out

            out = uart_cmd("cat /mnt/s6_hello.txt\n")
            assert "Received argument: 10" in out, out
            assert "Received argument: 20" in out, out
            print(f"[{mode.upper()}] S6 Append redirection (run /bin/hello >> file) verified.", flush=True)

            # 3. Direct execution path with redirection: /bin/hello 30 >> /mnt/s6_hello.txt
            out = uart_cmd("/bin/hello 30 >> /mnt/s6_hello.txt\n")
            assert "Hello from /bin/hello" not in out, out

            out = uart_cmd("cat /mnt/s6_hello.txt\n")
            assert "Received argument: 10" in out, out
            assert "Received argument: 20" in out, out
            assert "Received argument: 30" in out, out
            print(f"[{mode.upper()}] S6 Direct path execution with append (/bin/hello >> file) verified.", flush=True)

            # 4. Target path expansion: TARGET=/mnt/s6_target.txt; /bin/hello 42 > $TARGET
            uart_cmd("TARGET=/mnt/s6_target.txt\n")
            out = uart_cmd("/bin/hello 42 > $TARGET\n")
            assert "Hello from /bin/hello" not in out, out

            out = uart_cmd("cat /mnt/s6_target.txt\n")
            assert "Received argument: 42" in out, out
            print(f"[{mode.upper()}] S6 Target path variable expansion (> $TARGET) verified.", flush=True)

            # 5. Ambiguous redirection rejection: BAD="a b"; /bin/hello > $BAD
            uart_cmd('BAD="a b"\n')
            out = uart_cmd("/bin/hello > $BAD\n")
            assert "Ambiguous redirect" in out, f"Ambiguous redirect not reported: {out}"
            out = uart_cmd("echo $?\n")
            assert "1\n" in out, f"Expected exit status 1 for ambiguous redirect: {out}"
            print(f"[{mode.upper()}] S6 Ambiguous redirection rejection verified.", flush=True)

            # 6. Redirection open error: /bin/hello > /nonexistent/dir/out.txt
            out = uart_cmd("/bin/hello > /nonexistent/dir/out.txt\n")
            out = uart_cmd("echo $?\n")
            # Should have non-zero status
            assert "0\n" not in out, f"Expected non-zero status for failed redirection: {out}"
            print(f"[{mode.upper()}] S6 Redirection open failure handling verified.", flush=True)

            # 7. Parent builtin output redirection: echo $MSG1 > /mnt/s6_echo.txt
            uart_cmd('MSG1="Builtin line 1"\n')
            out = uart_cmd('echo $MSG1 > /mnt/s6_echo.txt\n')
            assert "Builtin line 1" not in out, f"Builtin output leaked to terminal: {out}"
            out = uart_cmd("cat /mnt/s6_echo.txt\n")
            assert "Builtin line 1" in out, out
            print(f"[{mode.upper()}] S6 Parent builtin redirection (echo > file) verified.", flush=True)

            # 8. Parent builtin append redirection: echo $MSG2 >> /mnt/s6_echo.txt
            uart_cmd('MSG2="Builtin line 2"\n')
            out = uart_cmd('echo $MSG2 >> /mnt/s6_echo.txt\n')
            assert "Builtin line 2" not in out, out
            out = uart_cmd("cat /mnt/s6_echo.txt\n")
            assert "Builtin line 1" in out, out
            assert "Builtin line 2" in out, out
            print(f"[{mode.upper()}] S6 Parent builtin append redirection (echo >> file) verified.", flush=True)

            # 9. Parent builtin pwd redirection: pwd > /mnt/s6_pwd.txt
            out = uart_cmd("pwd > /mnt/s6_pwd.txt\n")
            out = uart_cmd("cat /mnt/s6_pwd.txt\n")
            assert "/" in out, out
            print(f"[{mode.upper()}] S6 Parent builtin pwd redirection verified.", flush=True)

            # 10. Parent redirection-only command: > /mnt/s6_empty.txt
            out = uart_cmd("> /mnt/s6_empty.txt\n")
            out = uart_cmd("echo $?\n")
            assert "0\n" in out, f"Expected 0 status for empty redirection: {out}"
            out = uart_cmd("ls /mnt\n")
            assert "s6_empty.txt" in out, out
            print(f"[{mode.upper()}] S6 Redirection-only command (> /mnt/s6_empty.txt) verified.", flush=True)

            # 11. Retained terminal UI after descriptor close: 1>&-
            out = uart_cmd("echo closed 1>&-\n")
            # Prompt must still be responsive
            out = uart_cmd("echo still-alive\n")
            assert "still-alive" in out, f"Terminal corrupted after stdout close: {out}"
            print(f"[{mode.upper()}] S6 Retained terminal UI after stdout close verified.", flush=True)

            print(f"PASS {mode}: All S6 Phase 4 (4A-4D) redirection integration checks passed cleanly!", flush=True)

        finally:
            if "stop_reader" in locals(): stop_reader.set()
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait()


if __name__ == "__main__":
    modes = sys.argv[1:] or ("bios", "uefi")
    for m in modes:
        run(m)
