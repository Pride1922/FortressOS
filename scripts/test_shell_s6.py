#!/usr/bin/env python3
"""S6 integration test suite for FortressOS Ring 3 Shell.

Tests Shell Milestone S6:
  - Phase 4A & 4B: Child process file redirections via SYS_SPAWN_EXT
  - Target expansion (variables, quoting, ambiguous redirect rejection)
  - Output truncation (>), appending (>>), and input redirection (<)
  - Direct execution and 'run' execution with spawn_fd_action_t
  - Phase C: RO/Tainted storage assertions, execution suppression,
    file preservation, chained status propagation, and prompt recovery
"""
from contextlib import contextmanager
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


PROMPT_PATTERN = r"(?:fortress> |fortress:[^\n]* \$ )"


def ready_prompt(text, pattern=PROMPT_PATTERN, *, submitted=False):
    """Match the final prompt, excluding ANSI line-editor command redraws.

    The session uses the ANSI editor: horizontal scrolling redraws with CR and
    cursor escapes, while accepting Enter emits LF. Normalization removes CR
    and escapes but retains that submission boundary. A prompt before it is
    still part of input echo, not completion. Plain-mode editing is not used
    by these sessions (its redraws themselves emit LF).
    """
    start = 0
    if submitted:
        newline = text.find("\n")
        if newline < 0:
            return None
        start = newline + 1
    return re.compile(r"(?:" + pattern + r")\Z").search(text, start)


@contextmanager
def qemu_session(mode, fw_cfgs=None, log_suffix="", *, iso_path="bin/fortress.iso",
                 log_prefix="shell-s6", disk_audit=None):
    if fw_cfgs is None:
        fw_cfgs = ["name=opt/fortress/write_test,string=1"]
    cpus = "1"
    log = REPO / "build" / f"{log_prefix}-{mode}{log_suffix}.log"
    log.write_text("")
    with tempfile.TemporaryDirectory(prefix=f"fortress-s6-{mode}-") as tmp:
        uart_path, qmp_path, gdb_path = [Path(tmp) / n for n in ("uart", "qmp", "gdb")]
        img_copy = Path(tmp) / "nvme.img"
        shutil.copyfile(REPO / "build" / "nvme_gpt.img", img_copy)
        cmd = ["qemu-system-x86_64", "-M", "q35", "-m", "2G", "-display", "none",
               "-smp", cpus, "-no-reboot", "-S", "-monitor", "none", "-boot", "d", "-cdrom", str(iso_path),
               "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
               "-serial", "chardev:uart", "-qmp", f"unix:{qmp_path},server=on,wait=off",
               "-gdb", f"unix:{gdb_path},server=on,wait=off",
               "-drive", f"file={img_copy},if=none,id=nvm0,format=raw,snapshot=off",
               "-device", "nvme,serial=fortress0,drive=nvm0"]
        for cfg in fw_cfgs:
            cmd += ["-fw_cfg", cfg]
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

            def wait_prompt(pattern=PROMPT_PATTERN, after=0, *, submitted=False):
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    text = output()
                    m = ready_prompt(text[after:], pattern, submitted=submitted)
                    if m:
                        # Return the snapshot whose boundary was checked, not
                        # a later read with a potentially different suffix.
                        return text[after:]
                    assert child.poll() is None, child.stderr.read().decode()
                    time.sleep(0.05)
                raise AssertionError(f"Prompt matching {pattern!r} timed out: {log}\n{output()[-2000:]}")

            def uart_cmd(text, prompt_pat=PROMPT_PATTERN):
                start = len(output())
                for byte in text.encode():
                    uart.send(bytes([byte]))
                    time.sleep(0.005)
                return wait_prompt(pattern=prompt_pat, after=start, submitted=True)

            def command_body(command):
                out = uart_cmd(command + "\n")
                prompt = ready_prompt(out, submitted=True)
                assert prompt, repr(out)
                return out[out.index("\n") + 1:prompt.start()]

            # Wait for initial shell prompt
            initial = wait_prompt()
            assert "FortressOS shell (Ring 3)" in initial

            yield (command_body, uart_cmd, wait_prompt)

        finally:
            if "stop_reader" in locals(): stop_reader.set()
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait()

        if disk_audit is not None:
            disk_audit(img_copy)


def run(mode):
    # =========================================================================
    # Session 1: Standard Writable ext2 (Phase 4A-4D & dual-stream regression)
    # =========================================================================
    print(f"[{mode.upper()}] Starting Session 1: Standard Writable Redirections...", flush=True)
    with qemu_session(mode, ["name=opt/fortress/write_test,string=1"]) as (command_body, uart_cmd, wait_prompt):
        # 1. Child stdout redirection: /bin/hello > /mnt/s6_hello.txt
        out = uart_cmd("run /bin/hello 10 > /mnt/s6_hello.txt\n")
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
        out = uart_cmd("echo still-alive\n")
        assert "still-alive" in out, f"Terminal corrupted after stdout close: {out}"
        print(f"[{mode.upper()}] S6 Retained terminal UI after stdout close verified.", flush=True)

        # A/B: lexical order changes the destination of stderr.
        out = command_body("run /bin/dual_stream > /mnt/both.txt 2>&1")
        assert "STDOUT_DATA" not in out and "STDERR_DATA" not in out, out
        assert command_body("cat /mnt/both.txt") == "STDOUT_DATA\nSTDERR_DATA\n"
        out = command_body("run /bin/dual_stream 2>&1 > /mnt/only_stdout.txt")
        assert "STDERR_DATA\n" in out and "STDOUT_DATA" not in out, out
        assert command_body("cat /mnt/only_stdout.txt") == "STDOUT_DATA\n"

        # C: verify creation and preservation on a second stderr append.
        for count in (1, 2):
            out = command_body("run /bin/dual_stream 2>> /mnt/err_app.txt")
            assert "STDOUT_DATA\n" in out and "STDERR_DATA" not in out, out
            assert command_body("cat /mnt/err_app.txt") == "STDERR_DATA\n" * count
        out = command_body("run /bin/dual_stream 2> /mnt/err_app.txt")
        assert "STDOUT_DATA\n" in out and "STDERR_DATA" not in out, out
        assert command_body("cat /mnt/err_app.txt") == "STDERR_DATA\n"

        # D/E: closed stderr is tolerated; cat without operands reads stdin.
        out = command_body("run /bin/dual_stream 2>&-")
        assert "STDOUT_DATA\n" in out and "STDERR_DATA" not in out, out
        assert command_body("echo $?") == "0\n"
        assert command_body("cat < /mnt/both.txt") == "STDOUT_DATA\nSTDERR_DATA\n"

        # A parent setup failure after closing stderr cannot recurse, run
        # the builtin, or lose its failure status while restoring fds.
        uart_cmd('SKIP_MARKER=must-not-execute\n')
        out = command_body("echo $SKIP_MARKER 2>&- > /nonexistent/dir/out")
        assert "must-not-execute" not in out, out
        assert command_body("echo $?") == "1\n"
        assert command_body("echo responsive") == "responsive\n"
        print(f"[{mode.upper()}] S6 dual-stream order, stderr append/truncate, closed stderr, input and failure recovery verified.", flush=True)

    # =========================================================================
    # Session 2: Phase C Read-Only ext2 Mount (no write_test fw_cfg key)
    # =========================================================================
    print(f"[{mode.upper()}] Starting Session 2: Phase C Read-Only ext2 Storage Assertions...", flush=True)
    with qemu_session(mode, fw_cfgs=[], log_suffix="-ro") as (command_body, uart_cmd, wait_prompt):
        # 1. Pre-check: real ext2 fixture has hello.txt intact
        assert command_body("cat /mnt/hello.txt") == "Hello from FortressOS ext2 NVMe partition!\n"

        # 2. Child redirection on existing RO target: fails with EROFS, child suppressed, file preserved
        out = command_body("run /bin/dual_stream > /mnt/hello.txt")
        assert out == "Read-only filesystem.\n", f"Unexpected RO error: {out!r}"
        assert command_body("echo $?") == "1\n"
        assert command_body("cat /mnt/hello.txt") == "Hello from FortressOS ext2 NVMe partition!\n"

        # 3. Child redirection creating new file on RO mount: fails with EROFS
        out = command_body("run /bin/dual_stream > /mnt/new_ro_file.txt")
        assert out == "Read-only filesystem.\n", f"Unexpected RO error: {out!r}"
        assert command_body("echo $?") == "1\n"

        # 4. Parent builtin redirection on RO target: fails with EROFS, builtin skipped, file preserved
        out = command_body("echo overwrite > /mnt/hello.txt")
        assert out == "Read-only filesystem.\n", f"Unexpected RO error: {out!r}"
        assert command_body("echo $?") == "1\n"
        assert command_body("cat /mnt/hello.txt") == "Hello from FortressOS ext2 NVMe partition!\n"

        # 5. Parent builtin append on RO target: fails with EROFS, file preserved
        out = command_body("echo append >> /mnt/hello.txt")
        assert out == "Read-only filesystem.\n", f"Unexpected RO error: {out!r}"
        assert command_body("echo $?") == "1\n"
        assert command_body("cat /mnt/hello.txt") == "Hello from FortressOS ext2 NVMe partition!\n"

        # 6. Redirection-only on RO target: fails with EROFS, file preserved
        out = command_body("> /mnt/hello.txt")
        assert out == "Read-only filesystem.\n", f"Unexpected RO error: {out!r}"
        assert command_body("echo $?") == "1\n"
        assert command_body("cat /mnt/hello.txt") == "Hello from FortressOS ext2 NVMe partition!\n"

        # 7. Closed stderr with RO failure: tolerates closed stderr without crash, sets status 1, file intact
        out = command_body("echo overwrite 2>&- > /mnt/hello.txt")
        assert out == "", f"Expected empty output on closed stderr: {out!r}"
        assert command_body("echo $?") == "1\n"
        assert command_body("cat /mnt/hello.txt") == "Hello from FortressOS ext2 NVMe partition!\n"

        # 8. Command chaining with RO failure: || recovers, && halts
        out = command_body("echo test > /mnt/hello.txt || echo recovered")
        assert out == "Read-only filesystem.\nrecovered\n", f"Unexpected chaining output: {out!r}"
        assert command_body("echo $?") == "0\n"

        out = command_body("echo test > /mnt/hello.txt && echo should-not-run")
        assert out == "Read-only filesystem.\n", f"Unexpected chaining output: {out!r}"
        assert command_body("echo $?") == "1\n"

        # 9. Prompt recovery
        assert command_body("echo prompt-alive") == "prompt-alive\n"
        print(f"[{mode.upper()}] S6 Phase C Read-only ext2 assertions verified.", flush=True)

    # =========================================================================
    # Session 3: Phase C Tainted ext2 Storage (opt/fortress/taint_test active)
    # =========================================================================
    print(f"[{mode.upper()}] Starting Session 3: Phase C Tainted ext2 Storage Assertions...", flush=True)
    taint_cfgs = ["name=opt/fortress/write_test,string=1", "name=opt/fortress/taint_test,string=1"]
    with qemu_session(mode, fw_cfgs=taint_cfgs, log_suffix="-tainted") as (command_body, uart_cmd, wait_prompt):
        # 1. Pre-taint file-preservation check: existing file intact
        assert command_body("cat /mnt/hello.txt") == "Hello from FortressOS ext2 NVMe partition!\n"

        # 2. Child redirection on tainted mount: fails with EIO, child suppressed
        out = command_body("run /bin/dual_stream > /mnt/taint_child.txt")
        assert out == "I/O error.\n", f"Unexpected tainted error: {out!r}"
        assert command_body("echo $?") == "1\n"
        assert "STDOUT_DATA" not in out and "STDERR_DATA" not in out

        # 3. Parent builtin redirection on tainted mount: fails with EIO, file preserved
        out = command_body("echo overwrite > /mnt/hello.txt")
        assert out == "I/O error.\n", f"Unexpected tainted error: {out!r}"
        assert command_body("echo $?") == "1\n"
        # Post-taint canary check
        assert command_body("cat /mnt/hello.txt") == "Hello from FortressOS ext2 NVMe partition!\n"

        # 4. Parent builtin append on tainted mount: fails with EIO, file preserved
        out = command_body("echo append >> /mnt/hello.txt")
        assert out == "I/O error.\n", f"Unexpected tainted error: {out!r}"
        assert command_body("echo $?") == "1\n"
        assert command_body("cat /mnt/hello.txt") == "Hello from FortressOS ext2 NVMe partition!\n"

        # 5. Redirection-only on tainted mount: fails with EIO, file preserved
        out = command_body("> /mnt/hello.txt")
        assert out == "I/O error.\n", f"Unexpected tainted error: {out!r}"
        assert command_body("echo $?") == "1\n"
        assert command_body("cat /mnt/hello.txt") == "Hello from FortressOS ext2 NVMe partition!\n"

        # 6. Command chaining with tainted failure: || recovers, && halts
        out = command_body("echo fail > /mnt/hello.txt || echo recovered")
        assert out == "I/O error.\nrecovered\n", f"Unexpected chaining output: {out!r}"
        assert command_body("echo $?") == "0\n"

        out = command_body("echo fail > /mnt/hello.txt && echo should-not-run")
        assert out == "I/O error.\n", f"Unexpected chaining output: {out!r}"
        assert command_body("echo $?") == "1\n"

        # 7. Prompt recovery
        assert command_body("echo prompt-alive") == "prompt-alive\n"
        print(f"[{mode.upper()}] S6 Phase C Tainted ext2 assertions verified.", flush=True)

    print(f"PASS {mode}: All S6 Phase 4 & Phase C integration checks passed cleanly!", flush=True)


if __name__ == "__main__":
    modes = sys.argv[1:] or ("bios", "uefi")
    for m in modes:
        run(m)
