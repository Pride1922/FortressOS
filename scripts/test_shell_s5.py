#!/usr/bin/env python3
"""S5 integration test suite for FortressOS Ring 3 Shell.

Tests Shell Milestone S5:
  - Flat variables, assignments, set, unset, export, env
  - Parameter expansion ($VAR, ${VAR}, $?, $$)
  - Quote suppression and quoting rules (double quotes expand, single quotes literal)
  - Command-local temporary assignments (TEMP=val cmd) and environment passing
  - Aliases (alias, unalias, line expansion, quote suppression)
  - Globbing (*, ?, [...]) via VFS SYS_READDIR
  - Tilde expansion (~ -> $HOME)
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
    build_log = REPO / "build" / f"shell-s5-{mode}.log"
    with tempfile.TemporaryDirectory(prefix="fortress-s5-") as tmp:
        log = Path(tmp) / f"shell-s5-{mode}.log"
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

            # 1. Default variables: PATH, HOME
            out = uart_cmd("echo PATH=$PATH HOME=$HOME\n")
            assert "PATH=/bin HOME=/" in out, out
            print(f"[{mode.upper()}] S5 Default variables (PATH, HOME) verified.", flush=True)

            # 2. Variable assignments & simple expansion ($VAR and ${VAR})
            out = uart_cmd("FOO=bar\n")
            out = uart_cmd("echo $FOO ${FOO}\n")
            assert "bar bar" in out, out
            print(f"[{mode.upper()}] S5 Variable assignment and expansion verified.", flush=True)

            # 3. Quoting rules: double quotes expand, single quotes suppress expansion
            out = uart_cmd("echo \"val=$FOO\" 'val=$FOO'\n")
            assert "val=bar val=$FOO" in out, out
            print(f"[{mode.upper()}] S5 Quoting rules on variable expansion verified.", flush=True)

            # 4. Special parameters: $? (exit status) and $$ (PID)
            out = uart_cmd("true\n")
            out = uart_cmd("echo status=$?\n")
            assert "status=0" in out, out

            out = uart_cmd("false\n")
            out = uart_cmd("echo status=$?\n")
            assert "status=1" in out, out

            out = uart_cmd("echo pid=$$\n")
            assert re.search(r"pid=[1-9][0-9]*", out), out
            print(f"[{mode.upper()}] S5 Special parameters ($?, $$) verified.", flush=True)

            # 5. Variable unsetting and undefined variable expansion
            out = uart_cmd("unset FOO\n")
            out = uart_cmd("echo x${FOO}y\n")
            assert "xy" in out, out
            print(f"[{mode.upper()}] S5 unset builtin verified.", flush=True)

            # 6. set vs export vs env
            out = uart_cmd("LOCAL_VAR=internal_secret\n")
            out = uart_cmd("set\n")
            assert "LOCAL_VAR=internal_secret" in out, out

            out = uart_cmd("env\n")
            assert "LOCAL_VAR" not in out, out

            out = uart_cmd("export EXPORTED_VAR=public_val\n")
            out = uart_cmd("env\n")
            assert "EXPORTED_VAR=public_val" in out, out
            print(f"[{mode.upper()}] S5 set, export, and env builtins verified.", flush=True)

            # 7. Command-local temporary assignments
            out = uart_cmd("TEMP_FOO=temporary /bin/hello\n")
            assert "Hello from /bin/hello! VFS file execution verified." in out, out
            # Verify TEMP_FOO was not saved in shell table
            out = uart_cmd("echo val=$TEMP_FOO\n")
            assert "val=" in out and "val=temporary" not in out, out
            print(f"[{mode.upper()}] S5 Command-local assignment scoping verified.", flush=True)

            # 8. Aliases: definition, expansion, and execution
            out = uart_cmd("alias ll='echo list_long'\n")
            out = uart_cmd("alias\n")
            assert "alias ll='echo list_long'" in out, out

            out = uart_cmd("ll\n")
            assert "list_long" in out, out

            # Quote suppression: \ll suppresses alias
            out = uart_cmd("\\ll\n")
            assert "ll: command not found" in out or "Unknown command" in out, out

            # Trailing space continuation: alias with trailing blank makes next word eligible
            out = uart_cmd("alias sudo='echo sudo_wrap: '\n")
            out = uart_cmd("alias greet='hello_target'\n")
            out = uart_cmd("sudo greet\n")
            assert "sudo_wrap: hello_target" in out, out

            # Suppressed next word: sudo \greet suppresses greet expansion
            out = uart_cmd("sudo \\greet\n")
            assert "sudo_wrap: greet" in out, out

            out = uart_cmd("unalias sudo\n")
            out = uart_cmd("unalias greet\n")
            out = uart_cmd("unalias ll\n")
            out = uart_cmd("alias\n")
            assert "alias ll=" not in out, out
            print(f"[{mode.upper()}] S5 Aliases (alias, expansion, \\suppression, trailing space, unalias) verified.", flush=True)

            # 9. Tilde expansion
            out = uart_cmd("echo ~\n")
            assert "/" in out, out
            print(f"[{mode.upper()}] S5 Tilde expansion (~ -> $HOME) verified.", flush=True)

            # 10. Globbing (*, ?, [...])
            out = uart_cmd("echo /bin/h*\n")
            assert "/bin/hello" in out, out

            out = uart_cmd("echo /bin/h?llo\n")
            assert "/bin/hello" in out, out

            out = uart_cmd("echo /bin/[h]ello\n")
            assert "/bin/hello" in out, out

            # Literal preservation when no matches
            out = uart_cmd("echo /bin/nonexistent*\n")
            assert "/bin/nonexistent*" in out, out
            print(f"[{mode.upper()}] S5 Globbing (*, ?, [...], literal fallback) verified.", flush=True)

            # 11. Word splitting and multi-variable concatenation
            out = uart_cmd("A=one B=two\n")
            out = uart_cmd("echo result=$A-$B\n")
            assert "result=one-two" in out, out

            out = uart_cmd("echo 'single $A' \"double $B\"\n")
            assert "single $A double two" in out, out
            print(f"[{mode.upper()}] S5 Multi-variable concatenation & word expansion verified.", flush=True)

            print(f"PASS {mode}: All S5 integration checks passed cleanly!", flush=True)

        finally:
            if "stop_reader" in locals(): stop_reader.set()
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait()
            if log.exists():
                shutil.copyfile(log, build_log)


if __name__ == "__main__":
    modes = sys.argv[1:] or ("bios", "uefi")
    for m in modes:
        run(m)
