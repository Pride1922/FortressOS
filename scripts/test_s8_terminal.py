#!/usr/bin/env python3
"""BIOS/UEFI real UART and PS/2 Phase 3 probes, disposable ISO, no data disk."""
from pathlib import Path
import os
import re
import shutil
import socket
import subprocess
import tarfile
import tempfile
import threading
import time
from test_nmi_transitions import QMP, connect

REPO = Path(__file__).resolve().parent.parent
SMP = os.environ.get("SMP", "1")
assert SMP in ("1", "4", "8")


def key(qmp, code, modifier):
    events = [{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": name}}}
              for name, down in ((modifier, True), (code, True), (code, False), (modifier, False))]
    qmp.execute("input-send-event", {"events": events})


with tempfile.TemporaryDirectory(prefix="fortress-terminal-") as directory:
    tmp = Path(directory)
    root = tmp / "iso"
    shutil.copytree(REPO / "build/iso_root", root)
    archive = tmp / "initramfs.tar"
    with tarfile.open(REPO / "bin/initramfs.tar") as src, tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as dst:
        for member in src.getmembers():
            if member.name.lstrip("./") != "bin/shell":
                dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        dst.add(REPO / "build/s8_terminal_user.elf", arcname="bin/shell")
    for path in (root / "boot/initramfs.tar", root / "initramfs.tar"):
        shutil.copyfile(archive, path)
    config = ("timeout: 0\n/FortressOS Terminal Test\n    protocol: limine\n"
              "    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n")
    for path in (root / "limine.conf", root / "boot/limine.conf", root / "boot/limine/limine.conf"):
        path.write_text(config)
    iso = tmp / "terminal.iso"
    subprocess.run(["xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
                    "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                    "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
                    "--efi-boot-image", "--protective-msdos-label", str(root), "-o", str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO / "limine/limine"), "bios-install", str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for mode in ("bios", "uefi"):
        log = REPO / "build" / f"s8-terminal-{mode}-{SMP}.log"
        log.write_text("")
        uart_path, qmp_path = tmp / f"uart-{mode}", tmp / f"qmp-{mode}"
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
               "-smp", SMP, "-display", "none", "-monitor", "none", "-no-reboot",
               "-boot", "d", "-cdrom", str(iso),
               "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
               "-serial", "chardev:uart", "-qmp", f"unix:{qmp_path},server=on,wait=off"]
        firmware = []
        if mode == "uefi":
            variables = tmp / "vars.fd"
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)
            firmware = ["if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                        f"if=pflash,format=raw,unit=1,file={variables}"]
            for drive in firmware:
                cmd += ["-drive", drive]
        # Final argv preflight: only test ISO plus paired firmware, no extra
        # device/backend arguments and no environment-provided QEMU arguments.
        assert [cmd[i+1] for i, arg in enumerate(cmd) if arg == "-drive"] == firmware
        assert not any(arg in cmd for arg in ("-blockdev", "-device", "-hda", "-hdb", "-snapshot"))
        assert cmd[cmd.index("-cdrom")+1] == str(iso)
        with (REPO / "build" / f"s8-terminal-{mode}-{SMP}.stderr").open("wb") as stderr:
            proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
            uart = qmp = reader = None
            stop = threading.Event()
            try:
                uart = connect(uart_path)
                uart.settimeout(0.2)
                def drain():
                    while not stop.is_set():
                        try:
                            if not uart.recv(65536):
                                return
                        except socket.timeout:
                            pass
                        except OSError:
                            return
                reader = threading.Thread(target=drain, daemon=True)
                reader.start()
                qmp = QMP(qmp_path)
                deadline = time.monotonic() + 180
                stages = [("PRESERVE", lambda: uart.sendall(b"P")),
                          ("READER", lambda: key(qmp, "r", "shift")),
                          ("UART INT", lambda: uart.sendall(b"\x03")),
                          ("KEY INT", lambda: key(qmp, "c", "ctrl")),
                          ("KEY STOP", lambda: key(qmp, "z", "ctrl")),
                          ("UART LITERAL", lambda: uart.sendall(b"\x03")),
                          ("KEY LITERAL", lambda: key(qmp, "z", "ctrl")),
                          ("PASS", lambda: None)]
                for name, action in stages:
                    stage_deadline = min(deadline, time.monotonic() + (90 if name == "PRESERVE" else 20))
                    while time.monotonic() < stage_deadline:
                        output = log.read_text(errors="replace")
                        assert "S8 TERMINAL FAIL" not in output and "[FATAL]" not in output, output[-3000:]
                        assert proc.poll() is None, output[-3000:]
                        if f"S8 TERMINAL {name}\n" in output:
                            action()
                            break
                        time.sleep(0.02)
                    else:
                        raise AssertionError(f"Timeout at {name}: {log}\n{output[-3000:]}")
                output = log.read_text(errors="replace")
                if SMP == "1":
                    assert "Single enabled CPU" in output
                else:
                    matches = re.findall(r"All (\d+) application processor\(s\) online", output)
                    assert matches and int(matches[-1]) == int(SMP)-1, matches
                print(f"PASS S8 terminal {mode}, SMP={SMP}, AP count verified; UART + PS/2; no data disks", flush=True)
            finally:
                stop.set()
                if uart:
                    uart.close()
                if reader:
                    reader.join(timeout=1)
                if qmp:
                    qmp.stream.close()
                    qmp.sock.close()
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
