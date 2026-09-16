#!/usr/bin/env python3
"""Automated tests for FortressOS reboot and shutdown shell commands."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from test_nmi_transitions import REPO, connect

def test_power_command(cmd_name):
    print(f"Testing '{cmd_name}' command in QEMU...", flush=True)
    with tempfile.TemporaryDirectory(prefix="fortress-power-") as tmp:
        uart_path = Path(tmp) / "uart"
        log = Path(tmp) / "serial.log"
        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-display", "none",
            "-no-reboot", "-monitor", "none", "-boot", "d", "-cdrom", "bin/fortress.iso",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
            "-serial", "chardev:uart",
            "-drive", "file=build/nvme_gpt.img,if=none,id=nvm0,format=raw,snapshot=on",
            "-device", "nvme,serial=fortress0,drive=nvm0"
        ]
        child = subprocess.Popen(qemu_cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            uart = connect(uart_path)
            uart.settimeout(0.2)
            import threading
            stop_drain = threading.Event()
            def drain():
                while not stop_drain.is_set():
                    try:
                        if not uart.recv(65536): return
                    except socket.timeout:
                        continue
            drain_thread = threading.Thread(target=drain, daemon=True)
            drain_thread.start()

            # Wait for shell prompt
            deadline = time.monotonic() + 45
            prompt_found = False
            while time.monotonic() < deadline:
                if log.exists():
                    txt = log.read_text(errors="replace")
                    if "fortress> " in txt:
                        prompt_found = True
                        break
                assert child.poll() is None, child.stderr.read().decode()
                time.sleep(0.1)

            assert prompt_found, f"Shell prompt not reached:\n{log.read_text(errors='replace')[-1000:]}"
            time.sleep(0.2)

            # Send command
            for b in (cmd_name + "\n").encode():
                uart.send(bytes([b]))
                time.sleep(0.01)

            # QEMU should exit on reboot (-no-reboot) or shutdown
            exit_code = child.wait(timeout=10)
            assert exit_code == 0, f"QEMU exited with code {exit_code}"

            output = log.read_text(errors="replace")
            if cmd_name == "reboot":
                assert "Restarting system..." in output or "Reboot initiated" in output
            elif cmd_name == "shutdown":
                assert "Shutting down system..." in output or "Shutdown initiated" in output

            print(f"PASS: '{cmd_name}' cleanly triggered platform action and QEMU exited (code {exit_code})", flush=True)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()

if __name__ == "__main__":
    test_power_command("shutdown")
    test_power_command("reboot")
