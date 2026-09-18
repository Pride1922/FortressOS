#!/usr/bin/env python3
"""Real IRQ1/IRQ4 input, sleeping syscall continuation and shell lifecycle tests.

QMP input-send-event drives QEMU's PS/2 device; no guest input-buffer writes.
GDB reads only inspect scheduler state and resource counters while paused.
"""
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import threading
from test_nmi_transitions import REPO, Remote, QMP, symbols, connect


def offsets(tmp):
    source = Path(tmp) / "offsets.c"
    source.write_text('''#include <stdio.h>
#include "thread.h"
int main(void) {
 printf("%zu %zu %zu %zu", offsetof(tcb_t, state), offsetof(tcb_t, total_ticks),
        offsetof(tcb_t, fd_table), offsetof(tcb_t, wait_channel));
}''')
    exe = str(Path(tmp) / "offsets")
    subprocess.run(["gcc", "-Isrc/kernel", "-Isrc/include", str(source), "-o", exe], cwd=REPO, check=True)
    return list(map(int, subprocess.check_output([exe]).split()))


def run(mode):
    sym = symbols()
    log = REPO / "build" / f"shell-{mode}.log"
    log.write_text("")
    with tempfile.TemporaryDirectory(prefix="fortress-input-") as tmp:
        state_offset, ticks_offset, fds_offset, channel_offset = offsets(tmp)
        uart_path, qmp_path, gdb_path = [Path(tmp) / n for n in ("uart", "qmp", "gdb")]
        cmd = ["qemu-system-x86_64", "-M", "q35", "-m", "2G", "-display", "none",
               "-no-reboot", "-S", "-monitor", "none", "-boot", "d", "-cdrom", "bin/fortress.iso",
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
            qmp.execute("cont")

            def output():
                return log.read_text(errors="replace").replace("\r", "")

            def wait_prompt(after=0):
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    text = output()
                    if "fortress> " in text[after:]:
                        time.sleep(0.1)  # Let read enter the blocked list.
                        return text[after:]
                    assert child.poll() is None, child.stderr.read().decode()
                    time.sleep(0.05)
                qmp.execute("stop")
                print("debug serial available:", remote.memory(sym["serial_available"], 1).hex(), flush=True)
                qmp.execute("screendump", {"filename": str(REPO / "build" / "shell-timeout.png"), "format": "png"})
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
                    blocked = u64(sym["g_blocked_threads"])
                    assert blocked, "stdin reader must sleep, not yield/poll"
                    assert remote.memory(blocked + 16, 6) == b"shell\0"
                    assert int.from_bytes(remote.memory(blocked + state_offset, 4), "little") == 2
                    assert u64(blocked + channel_offset) == sym["g_input"]
                    assert u64(sym["g_current_thread"]) != blocked
                    assert remote.memory(blocked + fds_offset, 32 * 8) == bytes(32 * 8), "Leaked descriptor"
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
            assert "Show commands" in keyboard_command("helx\bp\n")
            assert "Hello\nfortress> " in keyboard_command("echo Hello\n")
            assert "shell\n" in keyboard_command("ls /bin\n")
            assert "Welcome to FortressOS" in keyboard_command("cat /etc/motd\n")
            assert "No such file" in uart_command("cat /missing\r\n")
            assert "Not a regular file" in uart_command("cat /bin\n")
            assert "Unknown command" in uart_command("invalid\n")
            assert "command discarded" in uart_command("x" * 200 + "\n")
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
