#!/usr/bin/env python3
"""
Test USB HID Boot Keyboard under QEMU.
Boots FortressOS with qemu-xhci and usb-kbd attached.
Verifies:
1. Keyboard detection, configuration, and boot protocol activation.
2. Ingress worker initialization on BSP.
3. Live keystrokes typed on the USB keyboard reaching the shell.
4. Command execution and layout switching under both BIOS and UEFI.
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

def run_hid_test(firmware="bios"):
    print(f"\n=======================================================")
    print(f"--> Testing USB HID Boot Keyboard under {firmware.upper()}...")
    print(f"=======================================================")

    log_path = REPO / "build" / f"usb-hid-{firmware}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("")

    with tempfile.TemporaryDirectory(prefix="fortress-usb-hid-") as tmp:
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
            "-device", "usb-kbd,bus=xhci.0",
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
            assert "[USB HID] PASS: Keyboard configured" in text, "Keyboard configuration missing from log"
            assert "[USB HID] Ingress worker started" in text, "Ingress worker banner missing from log"

            print(f"  [PASS] USB keyboard detected and configured")
            print(f"  [PASS] Shell prompt reached via live USB keyboard login input!")

            # Now test typing a command: "help\n"
            time.sleep(0.5)
            qmp_type_string(qmp, "help\n")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                text = log_path.read_text(errors="replace")
                if "Pipelines:" in text or "layout  Keyboard: us | azerty" in text:
                    break
                time.sleep(0.2)

            assert "Pipelines:" in text or "layout  Keyboard: us | azerty" in text, f"Command execution failed over USB keyboard; log:\n{text[-600:]}"
            print(f"  [PASS] Command 'help' executed successfully over USB keyboard!")

            # Now test layout switching to US:
            # In AZERTY, to type "layout us\n":
            # l -> l, a -> q, y -> y, o -> o, u -> u, t -> t, spc, u -> u, s -> s
            time.sleep(0.5)
            qmp_type_string(qmp, "lqyout us\n")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                text = log_path.read_text(errors="replace")
                if "Keyboard layout set to US QWERTY" in text:
                    break
                time.sleep(0.2)

            assert "Keyboard layout set to US QWERTY" in text, f"Layout switch output missing; log:\n{text[-600:]}"
            print(f"  [PASS] Layout successfully switched to US!")

            # Under US layout, type "echo test123\n"
            time.sleep(0.5)
            qmp_type_string(qmp, "echo test123\n")
            time.sleep(1.0)
            text = log_path.read_text(errors="replace")
            assert "test123" in text, f"Echo test under US layout failed; log:\n{text[-600:]}"
            print(f"  [PASS] Echo command typed and verified under US layout!")

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

if __name__ == "__main__":
    run_hid_test("bios")
    run_hid_test("uefi")
    print("\n=======================================================")
    print("[ALL PASS] USB HID Keyboard 100% verified (BIOS + UEFI)!")
    print("=======================================================\n")
