#!/usr/bin/env python3
"""S3/S4 integration test suite for FortressOS Ring 3 Shell.

Tests:
  S3:
    - Quote-aware lexer/parser: single quotes, double quotes, escapes, empty args, comments
    - Command chaining: ;, &&, ||, and pipeline negation !
    - Working directory: pwd, cd [path], cd -, relative path resolution across syscalls
    - Direct execution: command discovery in /bin, path-based spawn, type builtin, command wrapper
  S4:
    - Configurable prompt templates (prompt cwd, prompt default, status indicators)
    - Persistent history: history save/load to /mnt/.fortress/history
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
    log = REPO / "build" / f"shell-s3-s4-{mode}.log"
    log.write_text("")
    with tempfile.TemporaryDirectory(prefix="fortress-s3s4-") as tmp:
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
            uart = connect(uart_path)
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

            # Wait for shell banner and initial prompt
            initial = wait_prompt()
            assert "FortressOS shell (Ring 3)" in initial
            print(f"[{mode.upper()}] Initial shell reached.", flush=True)

            # --- S3: Quote-aware Lexing & Parsing ---
            # 1. Single quotes preserve literal text including spaces and shell metacharacters
            out = uart_cmd("echo 'hello world && not_a_command'\n")
            assert "hello world && not_a_command" in out, out
            print(f"[{mode.upper()}] S3 Single quotes verified.", flush=True)

            # 2. Double quotes with escape sequences
            out = uart_cmd('echo "escaped \\"quotes\\" and \\nnewline"\n')
            assert 'escaped "quotes" and' in out, out
            print(f"[{mode.upper()}] S3 Double quotes and escapes verified.", flush=True)

            # 3. Concatenated word tokens
            out = uart_cmd('echo foo"bar"baz\n')
            assert "foobarbaz" in out, out
            print(f"[{mode.upper()}] S3 Word part concatenation verified.", flush=True)

            # 4. Comments (#)
            out = uart_cmd("echo visible # this is a comment\n")
            lines = [l.strip() for l in out.splitlines()]
            assert "visible" in lines, lines
            assert not any("this is a comment" in l for l in lines[1:-1]), lines
            print(f"[{mode.upper()}] S3 Comments (#) verified.", flush=True)

            def uart_tab_cmd(prefix, prompt_pat=r"(fortress> |fortress:.* \$ )"):
                for byte in prefix.encode():
                    uart.send(bytes([byte]))
                    time.sleep(0.01)
                time.sleep(0.1)
                return uart_cmd("\n", prompt_pat=prompt_pat)

            # 4b. S4 Tab completion
            out = uart_tab_cmd("pw\t")
            assert "/\n" in out, out
            print(f"[{mode.upper()}] S4 Tab completion (pw<Tab> -> pwd) verified.", flush=True)

            out = uart_tab_cmd("cd /b\t")
            out = uart_cmd("pwd\n")
            assert "/bin\n" in out, out
            out = uart_cmd("cd /\n")
            print(f"[{mode.upper()}] S4 Path Tab completion (cd /b<Tab> -> /bin/) verified.", flush=True)

            # 5. Semicolon chaining
            out = uart_cmd("echo first ; echo second\n")
            assert "first" in out and "second" in out, out
            print(f"[{mode.upper()}] S3 Semicolon operator (;) verified.", flush=True)

            # 6. Pipeline negation (!)
            out = uart_cmd("! true ; echo $?\n")
            assert "1" in out, out
            out = uart_cmd("! false ; echo $?\n")
            assert "0" in out, out
            print(f"[{mode.upper()}] S3 Pipeline negation (!) verified.", flush=True)

            # --- S3: Working Directory Support (cd, pwd, relative paths) ---
            # 7. Initial pwd is /
            out = uart_cmd("pwd\n")
            assert "/\n" in out, out

            # 8. Change to /bin
            out = uart_cmd("cd /bin\n")
            out = uart_cmd("pwd\n")
            assert "/bin\n" in out, out

            # 9. Relative cd ..
            out = uart_cmd("cd ..\n")
            out = uart_cmd("pwd\n")
            assert "/\n" in out, out

            # 10. cd into docs and read relative file
            out = uart_cmd("cd docs\n")
            out = uart_cmd("pwd\n")
            assert "/docs\n" in out, out
            out = uart_cmd("cat readme.txt\n")
            assert "FortressOS Documentation" in out or "Ring 3 shell supports" in out, out
            print(f"[{mode.upper()}] S3 Relative path resolution in cat verified.", flush=True)

            # 11. cd - returns to previous directory
            out = uart_cmd("cd -\n")
            assert "/\n" in out, out
            out = uart_cmd("pwd\n")
            assert "/\n" in out, out
            print(f"[{mode.upper()}] S3 cd - (OLDPWD toggle) verified.", flush=True)

            # 12. cd error handling
            out = uart_cmd("cd /nonexistent_dir_12345\n")
            assert "cd: no such file or directory" in out, out
            out = uart_cmd("cd /docs/readme.txt\n")
            assert "cd: not a directory" in out, out
            print(f"[{mode.upper()}] S3 cd error handling verified.", flush=True)

            # --- S3: Direct Execution & Command Discovery ---
            # 13. Direct execution of /bin/hello without 'run' prefix
            out = uart_cmd("hello\n")
            assert "Hello from /bin/hello! VFS file execution verified." in out, out
            print(f"[{mode.upper()}] S3 Direct execution (hello -> /bin/hello) verified.", flush=True)

            # 14. Direct path execution (/bin/hello)
            out = uart_cmd("/bin/hello direct_arg\n")
            assert "Hello from /bin/hello! VFS file execution verified." in out, out
            assert "Received argument: direct_arg" in out, out
            print(f"[{mode.upper()}] S3 Direct path execution (/bin/hello) verified.", flush=True)

            # 15. Relative path execution (./bin/hello)
            out = uart_cmd("./bin/hello rel_arg\n")
            assert "Hello from /bin/hello! VFS file execution verified." in out, out
            assert "Received argument: rel_arg" in out, out
            print(f"[{mode.upper()}] S3 Relative path execution (./bin/hello) verified.", flush=True)

            # 16. Builtin inspection: type & command
            out = uart_cmd("type cd\n")
            assert "cd is a shell builtin" in out, out
            out = uart_cmd("type hello\n")
            assert "hello is /bin/hello" in out, out
            out = uart_cmd("command true && echo command_ok\n")
            assert "command_ok" in out, out
            print(f"[{mode.upper()}] S3 type and command builtins verified.", flush=True)

            # --- S4: Prompt Customization ---
            # 17. Inspect and configure prompt templates
            out = uart_cmd("prompt\n")
            assert "Prompt template: fortress> " in out, out
            out = uart_cmd("prompt cwd\n", prompt_pat=r"fortress:.* \$ ")
            assert "fortress:/ $" in out, out

            # Test prompt reflection of cwd
            out = uart_cmd("cd /bin\n", prompt_pat=r"fortress:/bin \$ ")
            assert "fortress:/bin $" in out, out

            # Test status indicator in prompt
            out = uart_cmd("false\n", prompt_pat=r"\[1\] fortress:/bin \$ ")
            assert "[1] fortress:/bin $" in out, out
            print(f"[{mode.upper()}] S4 Configurable prompt (fortress:<cwd> $) & status indicator verified.", flush=True)

            # Restore default prompt
            out = uart_cmd("prompt default\n", prompt_pat=r"fortress> ")
            assert "fortress> " in out, out
            out = uart_cmd("cd /\n")

            # --- S4: Persistent History ---
            # 18. Save history to /mnt/.fortress/history
            out = uart_cmd("history save\n")
            assert "History saved to /mnt/.fortress/history" in out, out

            # 19. Clear in-memory history
            out = uart_cmd("history clear\n")
            out = uart_cmd("history\n")
            assert "hello world" not in out, out

            # 20. Load history from persistent storage
            out = uart_cmd("history load\n")
            assert "History loaded from /mnt/.fortress/history" in out, out
            out = uart_cmd("history\n")
            assert "echo 'hello world && not_a_command'" in out, out
            print(f"[{mode.upper()}] S4 Persistent history save/load to /mnt/.fortress/history verified.", flush=True)

            print(f"PASS {mode}: All S3 and S4 integration checks passed cleanly!", flush=True)

        finally:
            if "stop_reader" in locals(): stop_reader.set()
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait()


if __name__ == "__main__":
    modes = sys.argv[1:] or ("bios", "uefi")
    for m in modes:
        run(m)
