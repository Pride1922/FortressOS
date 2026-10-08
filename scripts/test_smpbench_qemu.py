#!/usr/bin/env python3
import os
import re
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROMPT_PATTERN = re.compile(r'(?:fortress> |(?:\[\d+\] )?fortress:[^\r\n]* \$ )')

def run_smpbench_test(smp_count):
    print(f"\n========================================================")
    print(f"Testing SMP={smp_count} QEMU session")
    print(f"========================================================")
    with tempfile.TemporaryDirectory(prefix=f"fortress-smpbench-{smp_count}-") as tmp:
        uart_path = Path(tmp) / "uart.sock"
        log = REPO / "build" / f"smpbench-smp{smp_count}.log"
        if log.exists():
            log.unlink()

        cmd = [
            "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
            "-smp", str(smp_count), "-display", "none", "-no-reboot", "-monitor", "none",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
            "-serial", "chardev:uart",
            "-boot", "d", "-cdrom", "bin/fortress.iso"
        ]

        proc = subprocess.Popen(cmd, cwd=REPO)
        stop_drain = threading.Event()
        try:
            # Wait for socket
            sock = None
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if uart_path.exists():
                    try:
                        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        sock.connect(str(uart_path))
                        break
                    except (ConnectionRefusedError, FileNotFoundError):
                        time.sleep(0.1)
                time.sleep(0.1)
            assert sock is not None, "Failed to connect to UART socket"

            # Drain UART continuously in background
            sock.settimeout(0.2)
            def drain():
                while not stop_drain.is_set():
                    try:
                        if not sock.recv(65536):
                            return
                    except socket.timeout:
                        continue
                    except Exception:
                        return
            drain_thread = threading.Thread(target=drain, daemon=True)
            drain_thread.start()

            def output():
                if not log.exists():
                    return ""
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", log.read_bytes().decode(errors="replace")).replace("\r", "")

            # Wait for shell prompt
            deadline = time.monotonic() + 45
            prompt_seen = False
            while time.monotonic() < deadline:
                text = output()
                if PROMPT_PATTERN.search(text):
                    prompt_seen = True
                    break
                assert proc.poll() is None, "QEMU terminated unexpectedly"
                time.sleep(0.1)
            assert prompt_seen, f"Shell prompt not reached at SMP={smp_count}:\n{output()[-1000:]}"

            def send_cmd(command):
                start = len(output())
                for byte in (command + "\n").encode():
                    sock.send(bytes([byte]))
                    time.sleep(0.005)
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    cur = output()[start:]
                    if PROMPT_PATTERN.search(cur):
                        time.sleep(0.1)
                        # Extract between command and next prompt
                        lines = cur.splitlines()
                        return "\n".join(lines[1:-1]) if len(lines) > 2 else cur
                    assert proc.poll() is None, "QEMU died during command"
                    time.sleep(0.1)
                raise TimeoutError(f"Command {command!r} timed out:\n{output()[-1000:]}")

            # 1. lockstat before benchmark
            print(f"--- lockstat (before smpbench at SMP={smp_count}) ---")
            out_lockstat_before = send_cmd("lockstat")
            print(out_lockstat_before.strip())

            # 2. smpbench default
            print(f"\n--- smpbench default (SMP={smp_count}) ---")
            out_smpbench_def = send_cmd("smpbench")
            print(out_smpbench_def.strip())

            # 3. smpbench -c
            print(f"\n--- smpbench -c (SMP={smp_count}) ---")
            out_smpbench_c = send_cmd("smpbench -c")
            print(out_smpbench_c.strip())

            # 4. smpbench -w spawn_wait
            print(f"\n--- smpbench -w spawn_wait (SMP={smp_count}) ---")
            out_spawn_wait = send_cmd("smpbench -w spawn_wait")
            print(out_spawn_wait.strip())

            # 5. smpbench -w spawn_wait -c
            print(f"\n--- smpbench -w spawn_wait -c (SMP={smp_count}) ---")
            out_spawn_wait_c = send_cmd("smpbench -w spawn_wait -c")
            print(out_spawn_wait_c.strip())

            # 6. smpbench -w signals
            print(f"\n--- smpbench -w signals (SMP={smp_count}) ---")
            out_signals = send_cmd("smpbench -w signals")
            print(out_signals.strip())

            # 7. smpbench -w signals -c
            print(f"\n--- smpbench -w signals -c (SMP={smp_count}) ---")
            out_signals_c = send_cmd("smpbench -w signals -c")
            print(out_signals_c.strip())

            # 8. smpbench -w pipes
            print(f"\n--- smpbench -w pipes (SMP={smp_count}) ---")
            out_pipes = send_cmd("smpbench -w pipes")
            print(out_pipes.strip())

            # 9. smpbench -w pipes -c
            print(f"\n--- smpbench -w pipes -c (SMP={smp_count}) ---")
            out_pipes_c = send_cmd("smpbench -w pipes -c")
            print(out_pipes_c.strip())

            # 10. lockstat after benchmark
            print(f"\n--- lockstat (after smpbench at SMP={smp_count}) ---")
            out_lockstat_after = send_cmd("lockstat")
            print(out_lockstat_after.strip())

        finally:
            stop_drain.set()
            proc.terminate()
            proc.wait()

def main():
    run_smpbench_test(1)
    run_smpbench_test(4)
    run_smpbench_test(8)

if __name__ == "__main__":
    main()
