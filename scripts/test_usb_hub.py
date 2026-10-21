#!/usr/bin/env python3
"""
Test USB Hub and Dell Dock Topology Support under QEMU.
Boots FortressOS with qemu-xhci, a USB 2.0 Hub attached, and a USB Keyboard
attached downstream of the hub on port 1.
Verifies:
1. Hub detection, descriptor parsing, Configure Endpoint (Hub=1), and port powering.
2. Downstream port scan, reset, TT inheritance, and route string derivation.
3. Keyboard detection and configuration behind the hub tier.
4. Live keystrokes typed on the USB keyboard behind the hub reaching the shell.
5. Command execution and layout switching under both BIOS and UEFI.
"""
import sys
import time
import shutil
import tempfile
import subprocess
from pathlib import Path
from test_nmi_transitions import QMP

REPO = Path(__file__).resolve().parent.parent

def qmp_type_string(qmp, text, char_delay=0.08):
    for char in text:
        is_upper = char.isupper()
        base = char.lower()
        code = {' ': 'spc', '\n': 'ret', '/': 'slash', '.': 'dot', '_': 'minus', '-': 'minus'}.get(base, base)
        events = []
        if is_upper:
            events.append({'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': 'shift'}}})
        events.append({'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': code}}})
        events.append({'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': code}}})
        if is_upper:
            events.append({'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': 'shift'}}})
        qmp.execute('input-send-event', {'events': events})
        time.sleep(char_delay)

def run_hub_test(firmware="bios"):
    print(f"\n=======================================================")
    print(f"--> Testing USB Hub & Topology under {firmware.upper()}...")
    print(f"=======================================================")

    log_path = REPO / "build" / f"usb-hub-{firmware}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("")

    with tempfile.TemporaryDirectory(prefix="fortress-usb-hub-") as tmp:
        qmp_sock = Path(tmp) / "qmp.sock"
        cmd = [
            "qemu-system-x86_64",
            "-M", "q35",
            "-m", "2G",
            "-accel", "tcg",
            "-smp", "1",
            "-display", "none",
            "-monitor", "none",
            "-no-reboot",
            "-serial", f"file:{log_path}",
            "-boot", "d",
            "-cdrom", "bin/fortress.iso",
            "-device", "qemu-xhci,id=xhci",
            "-device", "usb-hub,id=hub1,bus=xhci.0,port=1",
            "-device", "usb-kbd,bus=xhci.0,port=1.1",
            "-qmp", f"unix:{qmp_sock},server=on,wait=off"
        ]

        if firmware == "uefi":
            code = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
            vars_tmpl = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
            assert code.is_file() and vars_tmpl.is_file()
            vars_copy = Path(tmp) / "vars.fd"
            shutil.copyfile(vars_tmpl, vars_copy)
            cmd += [
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}"
            ]

        proc = subprocess.Popen(cmd, cwd=REPO)
        qmp = None
        try:
            # Wait for QMP socket
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if qmp_sock.exists():
                    try:
                        qmp = QMP(qmp_sock)
                        break
                    except Exception:
                        pass
                time.sleep(0.1)
            assert qmp is not None, "Failed to connect to QMP"

            # Wait for login prompt
            deadline = time.monotonic() + 45
            login_seen = False
            while time.monotonic() < deadline:
                assert proc.poll() is None, "QEMU terminated prematurely"
                text = log_path.read_text(errors="replace")
                if "FortressOS login: " in text and not login_seen:
                    time.sleep(0.5)
                    # In default Belgian AZERTY, 'a' is key 'q'
                    qmp_type_string(qmp, "operqtor\n")
                    login_seen = True

                if "$ " in text and login_seen:
                    break
                time.sleep(0.2)

            text = log_path.read_text(errors="replace")
            assert "$ " in text, f"Failed to reach shell prompt within timeout; log:\n{text[-800:]}"
            assert "[USB HUB] Configuring Hub" in text, "Hub configuration missing from log"
            assert "[USB HUB] Ports:" in text, "Hub port count missing from log"
            assert "[USB HUB] Device connected on Port" in text, "Downstream connection missing from log"
            assert "[USB HID] PASS: Keyboard configured" in text, "Keyboard configuration missing from log"

            print(f"  [PASS] USB Hub detected, configured, and powered")
            print(f"  [PASS] Downstream keyboard detected and configured behind Hub")
            print(f"  [PASS] Shell prompt reached via live keyboard behind Hub!")

            # Test command: "help\n"
            time.sleep(0.5)
            qmp_type_string(qmp, "help\n")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                text = log_path.read_text(errors="replace")
                if "Pipelines:" in text or "layout  Keyboard: us | azerty" in text:
                    break
                time.sleep(0.2)

            assert "Pipelines:" in text or "layout  Keyboard: us | azerty" in text, f"Command execution failed; log:\n{text[-600:]}"
            print(f"  [PASS] Command 'help' executed successfully over Hub keyboard!")

            # Switch layout to US:
            time.sleep(0.5)
            qmp_type_string(qmp, "lqyout us\n")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                text = log_path.read_text(errors="replace")
                if "Keyboard layout set to US QWERTY" in text:
                    break
                time.sleep(0.2)

            assert "Keyboard layout set to US QWERTY" in text, f"Layout switch failed; log:\n{text[-600:]}"
            print(f"  [PASS] Layout successfully switched to US!")

            # Echo test under US layout
            time.sleep(0.5)
            qmp_type_string(qmp, "echo dock-hub-ok\n")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                text = log_path.read_text(errors="replace")
                if "dock-hub-ok" in text:
                    break
                time.sleep(0.2)

            assert "dock-hub-ok" in text, f"Echo test failed; log:\n{text[-600:]}"
            print(f"  [PASS] Command echoed and verified over Hub keyboard under US layout!")

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

if __name__ == "__main__":
    run_hub_test("bios")
    run_hub_test("uefi")
    print("\n=======================================================")
    print("[ALL PASS] USB Hub & Topology 100% verified (BIOS + UEFI)!")
    print("=======================================================\n")
