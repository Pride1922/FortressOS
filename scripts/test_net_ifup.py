#!/usr/bin/env python3
"""NET-3 Step 5: /bin/ifup acceptance test under QEMU.

Runs BIOS and UEFI boot with e1000 under SMP=1, waits for Ring 3 shell,
executes /bin/ifup tests (CLI CIDR, dotted netmask, dry-run, config file, error handling,
network reconfiguration verification via ifconfig and ping, and DNS fallback diagnostics).
Powers off cleanly. Enforces argv preflight (disposable ISO/OVMF, no data disks).
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
PROMPT_PATTERN = r"(?:fortress> |\[[a-zA-Z0-9_\-\./]+\]# )"


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
    log = REPO / "build" / f"net-ifup-{mode}.log"
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

            def run_command(command, timeout=15):
                before = len(get_text())
                uart.sendall(command.encode("ascii") + b"\n")
                wait_for_prompt(after_idx=before, timeout=timeout)
                return get_text()[before:]

            # 1. Wait for initial Ring 3 shell prompt
            initial = wait_for_prompt()
            print(f"[{mode}] Shell ready")

            # 2. Check initial ifconfig
            out = run_command("/bin/ifconfig")
            assert "inet 10.0.2.15/24" in out, f"Unexpected initial IP: {out}"
            print(f"[{mode}] Case 1 (initial ifconfig 10.0.2.15/24): PASS")

            # 3. Test ifup --help
            out = run_command("/bin/ifup --help")
            assert "usage: ifup" in out, f"ifup --help failed: {out}"
            print(f"[{mode}] Case 2 (ifup --help): PASS")

            # 4. Test ifup --dry-run
            out = run_command("/bin/ifup --dry-run 10.0.2.50/24 10.0.2.2")
            assert "eth0: address 10.0.2.50/24 gateway 10.0.2.2 valid (dry-run)" in out, f"dry-run failed: {out}"
            out_check = run_command("/bin/ifconfig")
            assert "inet 10.0.2.15/24" in out_check, f"dry-run modified interface: {out_check}"
            print(f"[{mode}] Case 3 (ifup --dry-run no-op): PASS")

            # 5. Apply CIDR form: 10.0.2.50/24 10.0.2.2
            out = run_command("/bin/ifup 10.0.2.50/24 10.0.2.2")
            assert "eth0: address 10.0.2.50/24 gateway 10.0.2.2 applied" in out, f"ifup CIDR apply failed: {out}"
            out_check = run_command("/bin/ifconfig")
            assert "inet 10.0.2.50/24" in out_check, f"ifconfig did not reflect new IP: {out_check}"
            print(f"[{mode}] Case 4 (ifup CIDR apply 10.0.2.50/24): PASS")

            # 6. Apply dotted-decimal form: 10.0.2.60 255.255.255.0 10.0.2.2
            out = run_command("/bin/ifup 10.0.2.60 255.255.255.0 10.0.2.2")
            assert "eth0: address 10.0.2.60/24 gateway 10.0.2.2 applied" in out, f"ifup dotted apply failed: {out}"
            out_check = run_command("/bin/ifconfig")
            assert "inet 10.0.2.60/24" in out_check, f"ifconfig did not reflect new IP: {out_check}"
            print(f"[{mode}] Case 5 (ifup dotted apply 10.0.2.60/24): PASS")

            # 7. Malformed address rejection
            out = run_command("/bin/ifup 10.0.2.300/24 10.0.2.2")
            assert "invalid" in out.lower(), f"ifup accepted malformed address: {out}"
            out_check = run_command("/bin/ifconfig")
            assert "inet 10.0.2.60/24" in out_check, f"ifconfig changed after malformed address: {out_check}"
            print(f"[{mode}] Case 6 (malformed address rejection): PASS")

            # 8. Apply config file from /etc/network.conf
            out = run_command("/bin/ifup /etc/network.conf")
            assert "eth0: address 10.0.2.15/24 gateway 10.0.2.2 applied" in out, f"ifup config file failed: {out}"
            out_check = run_command("/bin/ifconfig")
            assert "inet 10.0.2.15/24" in out_check, f"ifconfig did not reflect file config: {out_check}"
            print(f"[{mode}] Case 7 (ifup config file /etc/network.conf): PASS")

            # 9. Ping SLIRP gateway with restored IP
            out = run_command("/bin/ping -c 2 10.0.2.2", timeout=20)
            assert "bytes from 10.0.2.2" in out or "seq=" in out, f"Ping gateway failed after reconfiguration: {out}"
            print(f"[{mode}] Case 8 (ping gateway after reconfiguration): PASS")

            # 10. ifup missing default config file diagnostic
            out = run_command("/bin/ifup")
            assert "cannot open" in out.lower() or "error" in out.lower(), f"Unexpected missing config output: {out}"
            print(f"[{mode}] Case 9 (ifup missing /mnt/.fortress/network.conf diagnostic): PASS")

            # 11. nslookup missing DNS server diagnostic when -s omitted
            out = run_command("/bin/nslookup service.test")
            assert "no DNS server specified" in out, f"nslookup did not print missing DNS server diagnostic: {out}"
            print(f"[{mode}] Case 10 (nslookup missing DNS server diagnostic): PASS")

            # 12. Clean shutdown
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
    print("All /bin/ifup QEMU integration tests passed.")


if __name__ == "__main__":
    main()
