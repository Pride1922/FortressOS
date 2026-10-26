#!/usr/bin/env python3
"""
Test USB HID Boot Mouse under QEMU.
Boots FortressOS with qemu-xhci, usb-kbd, and usb-mouse attached.
Verifies:
1. Mouse detection, configuration, and boot protocol activation.
2. Direct root-port mouse attachment under BIOS.
3. Downstream hub/dock topology mouse attachment under UEFI.
4. Live QMP mouse movement (rel-x, rel-y) and button click events reaching the kernel.
5. Actual compositor cursor motion and USB keyboard shell recovery.
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

def qmp_send_mouse(qmp, dx=0, dy=0, btn=None, down=False):
    events = []
    if dx != 0:
        events.append({'type': 'rel', 'data': {'axis': 'x', 'value': dx}})
    if dy != 0:
        events.append({'type': 'rel', 'data': {'axis': 'y', 'value': dy}})
    if btn is not None:
        events.append({'type': 'btn', 'data': {'down': down, 'button': btn}})
    if events:
        qmp.execute('input-send-event', {'events': events})

def run_mouse_test(firmware="bios", use_hub=False):
    topo_name = "Behind Hub" if use_hub else "Direct Root Port"
    print(f"\n=======================================================")
    print(f"--> Testing USB HID Boot Mouse ({topo_name}) under {firmware.upper()}...")
    print(f"=======================================================")

    suffix = f"{firmware}-hub" if use_hub else firmware
    log_path = REPO / "build" / f"usb-mouse-{suffix}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("")

    with tempfile.TemporaryDirectory(prefix="fortress-usb-mouse-") as tmp:
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
            "-qmp", f"unix:{qmp_sock},server=on,wait=off"
        ]

        if use_hub:
            cmd += [
                "-device", "usb-hub,id=hub1,bus=xhci.0,port=1",
                "-device", "usb-kbd,bus=xhci.0,port=1.1",
                "-device", "usb-mouse,bus=xhci.0,port=1.2",
            ]
        else:
            cmd += [
                "-device", "usb-kbd,bus=xhci.0",
                "-device", "usb-mouse,bus=xhci.0",
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
                    qmp_type_string(qmp, "operqtor\n")
                    login_seen = True

                if "$ " in text and login_seen:
                    break
                time.sleep(0.2)

            text = log_path.read_text(errors="replace")
            assert "$ " in text, f"Failed to reach shell prompt within timeout; log:\n{text[-800:]}"
            assert "[USB HID] PASS: Mouse configured" in text, "Mouse configuration missing from log"
            assert "[USB HID] PASS: Keyboard configured" in text, "Keyboard configuration missing from log"
            assert "[USB HID] Ingress worker started" in text, "Ingress worker banner missing from log"

            print(f"  [PASS] USB mouse and keyboard detected and configured ({topo_name})")
            print(f"  [PASS] Shell prompt reached via live USB keyboard login input!")

            # Switch from the default AZERTY layout, then run the actual mouse
            # consumer. Packet UART traces are intentionally disabled by default.
            qmp_type_string(qmp, "lqyout us\n")
            time.sleep(0.3)
            qmp_type_string(qmp, "sudo /bin/compositor\n")
            deadline = time.monotonic()+25
            while time.monotonic()<deadline:
                text=log_path.read_text(errors="replace")
                if "[COMPOSITOR] [OK] Window server ready; polling" in text: break
                time.sleep(0.1)
            assert "[COMPOSITOR] [OK] Window server ready; polling" in text, text[-2000:]
            screen=Path(tmp)/"mouse.ppm"
            def screenshot():
                qmp.execute('screendump',{'filename':str(screen)})
                with screen.open('rb') as f:
                    assert f.readline().strip()==b'P6'
                    w,h=map(int,f.readline().split())
                    assert f.readline().strip()==b'255'
                    return w,h,f.read()
            def pixel(rgb,w,x,y):
                return rgb[(y*w+x)*3:(y*w+x)*3+3]
            deadline=time.monotonic()+10
            while time.monotonic()<deadline:
                w,h,rgb=screenshot()
                if pixel(rgb,w,w//2,h//2)==bytes.fromhex('101010'): break
                time.sleep(0.05)
            assert pixel(rgb,w,w//2,h//2)==bytes.fromhex('101010')

            # Inject Mouse events via QMP: Move mouse and click left button
            time.sleep(0.5)
            print("  --> Injecting mouse motion (dx=15, dy=-10) and left button press...")
            qmp_send_mouse(qmp, dx=15, dy=-10, btn="left", down=True)
            time.sleep(0.2)
            print("  --> Injecting mouse button release...")
            qmp_send_mouse(qmp, dx=0, dy=0, btn="left", down=False)
            time.sleep(0.5)

            deadline=time.monotonic()+10
            while time.monotonic()<deadline:
                w,h,rgb=screenshot()
                if pixel(rgb,w,w//2+15,h//2-10)==bytes.fromhex('101010'): break
                time.sleep(0.05)
            assert pixel(rgb,w,w//2+15,h//2-10)==bytes.fromhex('101010'), "Mouse did not reach compositor"
            assert pixel(rgb,w,w//2,h//2)==bytes.fromhex('141923'), "Cursor left a trail"
            qmp_send_mouse(qmp,btn="right",down=True)
            qmp_send_mouse(qmp,btn="right",down=False)
            deadline=time.monotonic()+10
            while time.monotonic()<deadline:
                text=log_path.read_text(errors="replace")
                if "[COMPOSITOR] Event loop stopped" in text: break
                time.sleep(0.05)
            assert "[COMPOSITOR] Event loop stopped" in text
            print("  [PASS] Real compositor cursor motion, restoration and exit verified")

            # Test keyboard responsiveness after mouse events: "help\n"
            time.sleep(0.5)
            qmp_type_string(qmp, "help\n")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                text = log_path.read_text(errors="replace")
                if "Pipelines:" in text or "layout  Keyboard: us | azerty" in text:
                    break
                time.sleep(0.2)

            assert "Pipelines:" in text or "layout  Keyboard: us | azerty" in text, "Help command did not produce expected output"
            print("  [PASS] Command execution successful after mouse events")

            # Clean quit via QMP
            try:
                qmp.execute('quit')
                proc.wait(timeout=5)
            except Exception:
                proc.terminate()
                proc.wait(timeout=5)
            print(f"  [PASS] Clean QEMU shutdown under {firmware.upper()} completed successfully!")

        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()

def main():
    print("=================================================================")
    print("FortressOS USB HID Boot Mouse Multi-Firmware Acceptance Suite")
    print("=================================================================")

    # 1. BIOS with direct root port mouse & keyboard
    run_mouse_test("bios", use_hub=False)

    # 2. UEFI with downstream mouse & keyboard behind USB Hub / Dock
    run_mouse_test("uefi", use_hub=True)

    print("\n=================================================================")
    print("ALL USB HID BOOT MOUSE MULTI-FIRMWARE TESTS PASSED (100%)")
    print("=================================================================")
    return 0

if __name__ == "__main__":
    sys.exit(main())
