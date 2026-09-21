#!/usr/bin/env python3
"""SMP Piece 1 (AP discovery) acceptance.

Boots QEMU twice: once with multiple vCPUs (every AP must report in and
match ACPI MADT) and once with a single vCPU (the pre-existing single-CPU
path must still work unchanged). Both runs must still reach the shell
prompt -- this piece must not regress ordinary boot.
"""
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
from test_nmi_transitions import REPO, connect


def boot(smp_count):
    print(f"Booting QEMU with -smp {smp_count}...", flush=True)
    with tempfile.TemporaryDirectory(prefix="fortress-smp-") as tmp:
        uart_path = Path(tmp) / "uart"
        log = Path(tmp) / "serial.log"
        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", str(smp_count),
            "-display", "none", "-no-reboot", "-monitor", "none",
            "-boot", "d", "-cdrom", "bin/fortress.iso",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
            "-serial", "chardev:uart",
            "-drive", "file=build/nvme_gpt.img,if=none,id=nvm0,format=raw,snapshot=on",
            "-device", "nvme,serial=fortress0,drive=nvm0",
        ]
        child = subprocess.Popen(qemu_cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            uart = connect(uart_path)
            uart.settimeout(0.2)
            stop_drain = threading.Event()

            def drain():
                while not stop_drain.is_set():
                    try:
                        if not uart.recv(65536):
                            return
                    except socket.timeout:
                        continue

            drain_thread = threading.Thread(target=drain, daemon=True)
            drain_thread.start()

            deadline = time.monotonic() + 45
            prompt_found = False
            while time.monotonic() < deadline:
                if log.exists() and "fortress> " in log.read_text(errors="replace"):
                    prompt_found = True
                    break
                assert child.poll() is None, child.stderr.read().decode()
                time.sleep(0.1)
            assert prompt_found, f"Shell prompt not reached with -smp {smp_count}:\n{log.read_text(errors='replace')[-2000:]}"

            stop_drain.set()
            return log.read_text(errors="replace")
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()


def test_multi_cpu(smp_count):
    output = boot(smp_count)
    ap_count = smp_count - 1
    assert "SMP Piece 1: AP Discovery" in output, "SMP Piece 1 checkpoint did not run"
    assert "[FAIL]" not in output.split("SMP Piece 1: AP Discovery", 1)[1].split("SMP Piece 1 (AP discovery) complete.", 1)[0], \
        f"SMP Piece 1 reported a failure with -smp {smp_count}:\n{output}"
    assert "Limine SMP response disagrees with ACPI MADT" not in output
    expected = f"All {ap_count} application processor(s) online" if ap_count else "Single-CPU system confirmed"
    assert expected in output, f"Expected '{expected}' in output with -smp {smp_count}, not found"
    assert "SMP Piece 1 (AP discovery) complete." in output
    print(f"PASS -smp {smp_count}: {ap_count} AP(s) matched MADT and reported in; boot reached shell", flush=True)


def test_single_cpu_unchanged():
    output = boot(1)
    assert "SMP Piece 1: AP Discovery" in output
    assert "Single-CPU system confirmed by both MADT and Limine" in output
    assert "[FAIL]" not in output.split("SMP Piece 1: AP Discovery", 1)[1].split("SMP Piece 1 (AP discovery) complete.", 1)[0]
    print("PASS -smp 1: single-CPU path unchanged, boot reached shell", flush=True)


if __name__ == "__main__":
    test_single_cpu_unchanged()
    test_multi_cpu(4)
