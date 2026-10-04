#!/usr/bin/env python3
"""NET-3 Step 3: /bin/ifconfig acceptance test under QEMU.

Runs BIOS and UEFI boot with e1000 under SMP=1, waits for Ring 3 shell,
executes /bin/ifconfig, verifies exact boot configuration output, and powers off cleanly.
Enforces argv preflight (disposable ISO/OVMF, no data disks).
"""

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
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
ISO = REPO / "bin" / "fortress.iso"
PROMPT_PATTERN = r"(?:fortress> |(?:\[-?\d+\] )?fortress:[^\r\n]* \$ |\[[a-zA-Z0-9_\-\./]+\]# )"


def qemu_command(mode, iso, variables, log, uart_path):
    cmd = [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
        "-smp", "1", "-display", "none", "-monitor", "none", "-no-reboot",
        "-boot", "d", "-cdrom", str(iso),
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0",
        "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
        "-serial", "chardev:uart"
    ]
    if mode == "uefi":
        cmd += [
            "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
            "-drive", f"if=pflash,format=raw,unit=1,file={variables}"
        ]
    return cmd


def preflight(cmd, mode, iso, variables, log, uart_path):
    assert mode in ("bios", "uefi")
    expected = qemu_command(mode, iso, variables, log, uart_path)
    assert cmd == expected, f"unexpected QEMU argv: {cmd} != {expected}"
    for token in ("-hda", "-hdb", "nvme", "-blockdev", "-snapshot", "usb-storage"):
        assert token not in cmd, f"preflight found forbidden token {token}"


def run_session(mode, iso, tmp):
    log = REPO / "build" / f"net-ifconfig-{mode}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text("")
    uart_path = tmp / f"uart-{mode}"
    variables = tmp / f"vars-{mode}.fd"
    if mode == "uefi":
        shutil.copyfile(VARS, variables)

    cmd = qemu_command(mode, iso, variables, log, uart_path)
    preflight(cmd, mode, iso, variables, log, uart_path)

    with log.with_suffix(".stderr").open("wb") as stderr:
        proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
        uart = None
        stop_reader = threading.Event()
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if uart_path.exists():
                    try:
                        uart = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        uart.connect(str(uart_path))
                        break
                    except (ConnectionRefusedError, OSError):
                        pass
                time.sleep(0.05)
            assert uart is not None, f"Could not connect to UART at {uart_path}"
            uart.settimeout(0.2)

            def drain():
                while not stop_reader.is_set():
                    try:
                        if not uart.recv(65536):
                            return
                    except (socket.timeout, OSError):
                        pass
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()

            def get_text():
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", log.read_bytes().decode(errors="replace")).replace("\r", "")

            def wait_for_prompt(after_idx=0, timeout=60):
                d = time.monotonic() + timeout
                while time.monotonic() < d:
                    t = get_text()
                    if re.search(PROMPT_PATTERN, t[after_idx:]):
                        return t[after_idx:]
                    time.sleep(0.1)
                raise TimeoutError(f"Prompt not seen within {timeout}s: {t[-500:]}")

            # 1. Wait for initial Ring 3 shell prompt
            initial = wait_for_prompt()
            print(f"[{mode}] Shell ready")

            # 2. Run /bin/ifconfig
            before_len = len(get_text())
            uart.sendall(b"/bin/ifconfig\n")
            wait_for_prompt(after_idx=before_len, timeout=10)
            output = get_text()[before_len:]

            print(f"[{mode}] Output of /bin/ifconfig:\n{output.strip()}")

            # 3. Assertions on output
            assert "eth0  HWaddr 52:54:00:12:34:56" in output, f"MAC missing in output: {output}"
            assert "inet 10.0.2.15/24  netmask 255.255.255.0  broadcast 10.0.2.255" in output, f"IP/netmask missing in output: {output}"
            assert "gateway 10.0.2.2" in output, f"Gateway missing in output: {output}"
            assert "dns " not in output, f"DNS unexpectedly present in boot output: {output}"
            assert "mtu 1500" in output, f"MTU missing in output: {output}"
            assert "link UP" in output, f"Link status missing in output: {output}"
            assert re.search(r"RX \d+\s+TX \d+", output), f"Packet counters missing in output: {output}"

            # 4. Clean shutdown
            uart.sendall(b"poweroff\n")
            proc.wait(timeout=10)
            print(f"[{mode}] PASS")

        finally:
            stop_reader.set()
            if uart:
                try:
                    uart.close()
                except OSError:
                    pass
            if proc.poll() is None:
                proc.kill()
                proc.wait()


def main():
    assert ISO.exists(), f"{ISO} does not exist. Run make first."
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        run_session("bios", ISO, tmp)
        if CODE.exists() and VARS.exists():
            run_session("uefi", ISO, tmp)
        else:
            print("Skipping UEFI (OVMF not installed)")
    print("All /bin/ifconfig QEMU integration tests passed.")


if __name__ == "__main__":
    main()
