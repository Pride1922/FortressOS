#!/usr/bin/env python3
"""Real IRQ1/IRQ4 input, sleeping syscall continuation and shell lifecycle tests.

QMP input-send-event drives QEMU's PS/2 device; no guest input-buffer writes.
GDB reads only inspect scheduler state and resource counters while paused.
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


def scheduler_symbols(remote, sym):
    import struct
    values = struct.unpack("<3Q", remote.memory(sym["scheduler_debug_bsp"], 24))
    sym.update(zip(("g_blocked_threads", "g_current_thread", "g_stack_slots_bitmap"), values))


def offsets(tmp):
    source = Path(tmp) / "offsets.c"
    source.write_text('''#include <stdio.h>
#include "thread.h"
#include "vfs.h"
int main(void) {
 printf("%zu %zu %zu %zu %zu %zu %zu %zu", offsetof(tcb_t, state), offsetof(tcb_t, total_ticks),
        offsetof(tcb_t, fd_table), offsetof(tcb_t, wait_channel),
        offsetof(file_t, node), offsetof(file_t, flags), offsetof(file_t, ref_count),
        offsetof(tcb_t, fd_flags));
}''')
    exe = str(Path(tmp) / "offsets")
    subprocess.run(["gcc", "-Isrc/kernel", "-Isrc/include", "-Isrc/fs", str(source), "-o", exe], cwd=REPO, check=True)
    return list(map(int, subprocess.check_output([exe]).split()))


def run(mode):
    sym = symbols()
    cpus = os.environ.get("SHELL_TEST_CPUS", "1")
    log = REPO / "build" / f"shell-{mode}-{cpus}cpu.log"
    log.write_text("")
    with tempfile.TemporaryDirectory(prefix="fortress-input-") as tmp:
        (state_offset, ticks_offset, fds_offset, channel_offset,
         node_offset, flags_offset, refs_offset, fd_flags_offset) = offsets(tmp)
        uart_path, qmp_path, gdb_path = [Path(tmp) / n for n in ("uart", "qmp", "gdb")]
        cmd = ["qemu-system-x86_64", "-M", "q35", "-m", "2G", "-display", "none",
               "-smp", os.environ.get("SHELL_TEST_CPUS", "1"), "-no-reboot", "-S", "-monitor", "none", "-boot", "d", "-cdrom", "bin/fortress.iso",
               "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
               "-serial", "chardev:uart", "-qmp", f"unix:{qmp_path},server=on,wait=off",
               "-gdb", f"unix:{gdb_path},server=on,wait=off",
               "-drive", "file=build/nvme_gpt.img,if=none,id=nvm0,format=raw,snapshot=on",
               "-device", "nvme,serial=fortress0,drive=nvm0"]
        if mode == "uefi":
            vars_path = Path(tmp) / "vars.fd"
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", vars_path)
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                    "-drive", f"if=pflash,format=raw,unit=1,file={vars_path}"]
        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            uart = connect(uart_path)
            # Drain continuously: per-byte socket writes can exhaust host socket
            # packet buffers long before the serial log reaches 64 KiB.
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
            # A socket connect can return before QEMU handles GDB attachment.
            # Attachment stops the VM: finish a protocol round trip before
            # QMP resumes it, or a late attach can leave firmware paused.
            remote.request("qSupported")
            qmp.execute("cont")

            def output():
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", log.read_bytes().decode(errors="replace")).replace("\r", "")

            def wait_prompt(after=0):
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    text = output()
                    if "\nfortress> " in text[after:]:
                        time.sleep(0.1)  # Let read enter the blocked list.
                        return text[after:]
                    assert child.poll() is None, child.stderr.read().decode()
                    time.sleep(0.05)
                qmp.execute("stop")
                qmp.execute("screendump", {"filename": str(REPO / "build" / "shell-timeout.png"), "format": "png"})
                try:
                    print("debug serial available:", remote.memory(sym["serial_available"], 1).hex(), flush=True)
                except AssertionError as error:
                    print(f"Kernel not mapped at timeout: {error}", flush=True)
                raise AssertionError(f"Shell prompt timed out: {log}\n{output()[-2000:]}")

            def uart_command(text):
                start = len(output())
                # Pace below the 16-byte UART FIFO; this is not a paste-speed test.
                for byte in text.encode():
                    uart.send(bytes([byte]))
                    time.sleep(0.005)
                    output()
                return wait_prompt(start)

            def edit_command(text, expect="edit> "):
                start = len(output())
                for byte in text.encode():
                    uart.send(bytes([byte]))
                    time.sleep(0.005)
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    text_out = output()
                    if expect in text_out[start:]:
                        time.sleep(0.05)
                        return text_out[start:]
                    assert child.poll() is None, child.stderr.read().decode()
                    time.sleep(0.05)
                raise AssertionError(f"Expected {expect!r} timed out: {log}\n{output()[-1000:]}")

            def key(code, shift=False):
                events = []
                if shift:
                    events.append({"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "shift"}}})
                events += [{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": code}}} for down in (True, False)]
                if shift:
                    events.append({"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}})
                qmp.execute("input-send-event", {"events": events})
                time.sleep(0.03)

            def keyboard_command(text):
                start = len(output())
                for c in text:
                    code = {" ": "spc", "\n": "ret", "\b": "backspace", "/": "slash", ".": "dot"}.get(c, c.lower())
                    key(code, c.isupper())
                return wait_prompt(start)

            def snapshot():
                qmp.execute("stop")
                try:
                    def u64(address): return int.from_bytes(remote.memory(address, 8), "little")
                    scheduler_symbols(remote, sym)
                    blocked = u64(sym["g_blocked_threads"])
                    assert blocked, "stdin reader must sleep, not yield/poll"
                    assert remote.memory(blocked + 16, 6) == b"shell\0"
                    assert int.from_bytes(remote.memory(blocked + state_offset, 4), "little") == 2
                    assert u64(blocked + channel_offset) == sym["g_input"]
                    assert u64(sym["g_current_thread"]) != blocked
                    standard = [u64(blocked + fds_offset + fd * 8) for fd in range(3)]
                    assert all(standard) and len(set(standard)) == 3, "Missing/aliased standard handles"
                    for handle, mode in zip(standard, (0, 1, 1)):
                        assert u64(handle + node_offset) == sym["g_terminal_node"], "Non-terminal standard handle"
                        assert int.from_bytes(remote.memory(handle + flags_offset, 4), "little") == mode
                        assert int.from_bytes(remote.memory(handle + refs_offset, 4), "little") == 1, "Leaked file reference"
                    assert remote.memory(blocked + fds_offset + 3 * 8, 29 * 8) == bytes(29 * 8), "Leaked descriptor"
                    assert remote.memory(blocked + fd_flags_offset, 32 * 4) == bytes(32 * 4), "Unexpected descriptor flags"
                    return {"pid": u64(blocked + 8), "ticks": u64(blocked + ticks_offset),
                            "timer": u64(sym["g_timer_ticks"]), "free": u64(sym["free_pages"]),
                            "slots": u64(sym["g_stack_slots_bitmap"])}
                finally:
                    qmp.execute("cont")

            text = wait_prompt()
            assert "[INPUT] PS/2 set 2 -> set 1, IRQ1 ready" in text
            assert "[INPUT] COM1 RX IRQ4 ready" in text
            assert "[FAIL]" not in text, text[-2000:]
            first = snapshot()
            time.sleep(0.3)
            second = snapshot()
            assert first["ticks"] == second["ticks"] and second["timer"] > first["timer"]
            uart_command("layout us\n")   # keyboard-driven tests below use US-layout QMP keycodes
            assert "Show commands" in keyboard_command("helx\bp\n")
            assert "Hello\nfortress> " in keyboard_command("echo Hello\n")
            assert "shell\n" in keyboard_command("ls /bin\n")
            assert "Welcome to FortressOS" in keyboard_command("cat /etc/motd\n")
            assert "No such file" in uart_command("cat /missing\r\n")
            assert "Not a regular file" in uart_command("cat /bin\n")
            assert "Unknown command" in uart_command("invalid\n")
            assert "x" * 200 in uart_command("echo " + "x" * 200 + "\n")
            assert "Ring 3 shell supports" in uart_command("cat /docs/readme.txt\n")
            assert "hello.txt" in uart_command("ls /mnt\n")
            assert "Hello from FortressOS ext2" in uart_command("cat /mnt/hello.txt\n")
            assert "Active keyboard layout: US QWERTY" in uart_command("layout\n")
            assert "Keyboard layout set to Belgian AZERTY" in uart_command("layout azerty\n")
            assert "Active keyboard layout: Belgian AZERTY" in uart_command("layout\n")
            assert "Keyboard layout set to US QWERTY" in uart_command("layout us\n")
            assert "Usage: edit /path" in uart_command("edit\n")
            assert "Not a regular file" in uart_command("edit /bin\n")
            assert "[EDIT] Loaded" in edit_command("edit /docs/readme.txt\n")
            assert "FortressOS Documentation" in edit_command("p\n")
            edit_command("a\nNew in-memory line\n.\n")
            assert "New in-memory line" in edit_command("p\n")
            assert "Modified: yes" in edit_command("stats\n")
            assert "Deleted line 1" in edit_command("d 1\n")
            assert "In-memory changes discarded" in edit_command("q\n", expect="fortress> ")
            assert "[EDIT] New buffer" in edit_command("edit /missing.txt\n")
            assert "Read-only filesystem." in edit_command("w\n")
            assert "fortress> " in edit_command("q\n", expect="fortress> ")

            # VFS Program Execution & Argument Passing
            before_run = snapshot()
            hello_out = keyboard_command("run /bin/hello\n")
            assert "Hello from /bin/hello! VFS file execution verified." in hello_out
            assert "[PROCESS] Exit status" not in hello_out
            assert "fortress> " in hello_out

            hello_arg_out = uart_command("run /bin/hello 42\n")
            assert "Hello from /bin/hello! VFS file execution verified." in hello_arg_out
            assert "Received argument: 42" in hello_arg_out
            assert "[PROCESS] Exit status 42" in hello_arg_out
            assert "fortress> " in hello_arg_out

            # Shell status query ($?) and command chaining (&&, ||)
            assert "42\nfortress> " in uart_command("echo $?\n")
            assert "0\nfortress> " in uart_command("echo $?\n")
            assert "Chained AND\nfortress> " in uart_command("run /bin/hello && echo Chained AND\n")
            and_skip = uart_command("run /bin/hello 42 && echo ShouldNotPrint\n")
            assert "ShouldNotPrint" not in and_skip[and_skip.find("\n") + 1:]
            assert "Chained OR\nfortress> " in uart_command("run /bin/hello 42 || echo Chained OR\n")
            or_skip = uart_command("run /bin/hello || echo ShouldNotPrint\n")
            assert "ShouldNotPrint" not in or_skip[or_skip.find("\n") + 1:]

            assert "Usage: run /path" in uart_command("run\n")
            assert "No such file or directory" in uart_command("run /missing\n")
            assert "Not a regular file" in uart_command("run /bin\n")
            hello_str_out = uart_command("run /bin/hello world\n")
            assert "Hello from /bin/hello! VFS file execution verified." in hello_str_out
            assert "Received argument: world" in hello_str_out
            assert "[PROCESS] Exit status" not in hello_str_out
            assert "fortress> " in hello_str_out
            assert "0\nfortress> " in uart_command("echo $?\n")

            # Null/empty string argument test
            hello_empty_out = uart_command('run /bin/hello ""\n')
            assert "Hello from /bin/hello! VFS file execution verified." in hello_empty_out
            assert "Received argument: \n" in hello_empty_out
            assert "[PROCESS] Exit status" not in hello_empty_out
            assert "fortress> " in hello_empty_out

            # MAX_SPAWN_ARGS (32) boundary tests: exactly 32 args succeeds
            args_31 = " ".join(f"a{i}" for i in range(31))
            run_32 = uart_command(f"run /bin/hello {args_31}\n")
            assert "Hello from /bin/hello! VFS file execution verified." in run_32
            assert "Received argument: a0" in run_32
            assert "[PROCESS] Exit status" not in run_32
            assert "fortress> " in run_32

            # 33 arguments exceeds MAX_SPAWN_ARGS: rejected by shell/kernel
            args_32 = " ".join(f"a{i}" for i in range(32))
            run_33 = uart_command(f"run /bin/hello {args_32}\n")
            assert "Too many arguments (max 32)" in run_33
            assert "fortress> " in run_33

            # S0-S2: editing through the actual raw-input syscall and terminal.
            assert "\nabc\n" in uart_command("echo ac\x1b[Db\n")
            assert "\nright\n" in uart_command("echo wrong\x17right\n")
            assert "\nright\n" in uart_command("\x1b[A\n")
            assert "\nDRAFT\n" in uart_command("echo DRAFT\x1b[A\x1b[B\n")
            assert "\nalpha\n" in uart_command("echo alpha\n")
            assert "\nalpha\n" in uart_command("\x12alpha\n\n")
            assert "\nrescued\n" in uart_command("echo rescued\x12alpha\x07\n")
            assert "\nabcdef\n" in uart_command("echo abcXdef\x1b[D\x1b[D\x1b[D\x1b[D\x1b[3~\n")
            # Inspect real framebuffer cells and logical cursor, not just log substrings.
            for byte in b"echo ac\x1b[Db":
                uart.send(bytes([byte])); time.sleep(0.02)
            time.sleep(0.2)
            qmp.execute("stop")
            console = remote.memory(sym["g_console"], 64)
            col = int.from_bytes(console[48:56], "little")
            row = int.from_bytes(console[56:64], "little")
            cells = remote.memory(sym["cells"] + row * 512 * 12, 30 * 12)
            assert bytes(cells[8::12]).startswith(b"fortress> echo abc"), bytes(cells[8::12])
            assert col == 17, col
            qmp.execute("cont")
            assert "\nabc\n" in uart_command("\n")
            # Standalone ESC expires without polling; next command still runs.
            uart.send(b"\x1b")
            time.sleep(0.3)
            assert "\ntimeout-ok\n" in uart_command("echo timeout-ok\n")
            # A bracketed paste never executes its embedded newlines.
            start_paste = len(output())
            for byte in b"\x1b[200~echo PASTED\n\x1b[201~":
                uart.send(bytes([byte])); time.sleep(0.02)
            time.sleep(0.2)
            assert "\nPASTED\n" not in output()[start_paste:]
            uart.send(b"\n"); time.sleep(0.2)
            assert "\nPASTED\n" not in output()[start_paste:]
            assert "\nPASTED\n" in uart_command("\n")
            assert "\nABI-editor-sentinel\n" in uart_command("echo ABI-editor-sentinel\n")
            kernel_log = uart_command("dmesg\n")
            assert "ABI-editor-sentinel" not in kernel_log, "User output polluted dmesg"
            assert "echo alpha" in uart_command("history\n")
            uart_command("history clear\n")
            assert "echo alpha" not in uart_command("history\n")

            # Zero-leak audit across process spawn/wait
            after_run = snapshot()
            assert after_run["free"] == before_run["free"], f"Page frame leak after run: {before_run['free']} -> {after_run['free']}"
            assert after_run["slots"] == before_run["slots"], f"Stack slot leak after run: {before_run['slots']:#x} -> {after_run['slots']:#x}"

            for _ in range(3):
                assert "FortressOS shell (Ring 3)" in uart_command("exit\n")
                current = snapshot()
                assert current["pid"] != first["pid"]
                assert current["free"] == first["free"] and current["slots"] == first["slots"]
            print(f"PASS {mode}: PS/2 and serial commands, blocked reader/timer progress, validation, restarts and resource counts ({log})", flush=True)
        finally:
            if "stop_reader" in locals(): stop_reader.set()
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait()


if __name__ == "__main__":
    for mode in sys.argv[1:] or ("bios", "uefi"):
        run(mode)
