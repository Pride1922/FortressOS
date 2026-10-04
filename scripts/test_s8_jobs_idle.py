#!/usr/bin/env python3
"""BIOS/UEFI real-shell idle reaping and edit preservation; no data disks."""
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


def key(qmp, code):
    qmp.execute("input-send-event", {"events": [
        {"type": "key", "data": {"down": down,
         "key": {"type": "qcode", "data": code}}} for down in (True, False)]})
    time.sleep(0.03)


with tempfile.TemporaryDirectory(prefix="fortress-jobs-idle-") as directory:
    tmp = Path(directory)
    root = tmp / "iso"
    shutil.copytree(REPO / "build/iso_root", root)
    archive = tmp / "initramfs.tar"
    with tarfile.open(REPO / "bin/initramfs.tar") as src, tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as dst:
        for member in src.getmembers():
            # Preserve the real shell and every production initramfs member.
            dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        dst.add(REPO / "build/s8_jobs_delay_user.elf", arcname="bin/idle-delay")
    for path in (root / "boot/initramfs.tar", root / "initramfs.tar"):
        shutil.copyfile(archive, path)
    config = ("timeout: 0\n/FortressOS Jobs Idle Test\n    protocol: limine\n"
              "    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n")
    for path in (root / "limine.conf", root / "boot/limine.conf", root / "boot/limine/limine.conf"):
        path.write_text(config)
    iso = tmp / "jobs-idle.iso"
    subprocess.run(["xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
                    "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                    "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
                    "--efi-boot-image", "--protective-msdos-label", str(root), "-o", str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO / "limine/limine"), "bios-install", str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for mode in ("bios", "uefi"):
        log = REPO / "build" / f"s8-jobs-idle-{mode}-{SMP}.log"
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
        with (REPO / "build" / f"s8-jobs-idle-{mode}-{SMP}.stderr").open("wb") as stderr:
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
                deadline = time.monotonic() + 240

                def output():
                    raw = log.read_bytes().decode(errors="replace")
                    assert proc.poll() is None, raw[-3000:]
                    assert "[FATAL]" not in raw and "[FAIL]" not in raw, raw[-3000:]
                    return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", raw).replace("\r", "")

                def wait(pattern, after=0, seconds=60):
                    end = min(deadline, time.monotonic() + seconds)
                    while time.monotonic() < end:
                        text = output()
                        match = re.search(pattern, text[after:])
                        if match:
                            return match, after + match.end()
                        time.sleep(0.02)
                    raise AssertionError(f"Timeout waiting for {pattern!r}: {log}\n{output()[-3000:]}")

                def type_uart(text):
                    # Pace below the hardware FIFO, as in test_shell.py.
                    for byte in text.encode():
                        uart.sendall(bytes([byte]))
                        time.sleep(0.005)

                wait(r"(?:fortress> |fortress:[^\r\n]* \$ )", seconds=90)
                start = len(output())
                type_uart("prompt 'fortress> '\n")
                wait(r"fortress> ", start)
                start = len(output())
                type_uart("layout us\n")
                wait(r"Keyboard layout set to US QWERTY", start)
                wait(r"fortress> ", start)

                def launch():
                    start = len(output())
                    type_uart("/bin/idle-delay &\n")
                    match, end = wait(r"\[(\d+)\] (\d+)\n", start)
                    job = match.group(1)
                    _, prompt_end = wait(r"fortress> ", end)
                    # Completion must happen after returning to an idle prompt.
                    assert not re.search(rf"\[{job}\]  Done", output()[start:]), "Delay helper exited too early"
                    return job, prompt_end

                # No input whatsoever from launch to notification.
                job, idle_start = launch()
                _, done_end = wait(rf"\[{job}\]  Done +/bin/idle-delay &\n", idle_start)
                wait(r"fortress> ", done_end)

                # Launch first, then leave a partially edited command at the
                # next prompt. A second launch cannot be typed into that draft.
                job, idle_start = launch()
                type_uart("echo ac")
                key(qmp, "left")
                _, edit_end = wait(r"fortress> echo ac", idle_start)
                assert not re.search(rf"\[{job}\]  Done", output()[idle_start:]), "Delay helper exited before editing"
                # No UART writes or QMP key events until the notification and
                # redraw both arrive. Require the draft AFTER this exact Done.
                _, done_end = wait(rf"\[{job}\]  Done +/bin/idle-delay &\n", edit_end)
                _, redraw_end = wait(r"fortress> echo ac", done_end)
                # Empty timeout scans must neither print nor repaint repeatedly.
                time.sleep(0.35)
                assert output()[redraw_end:] == "", "Idle scans produced extra output"
                # The cursor was between a and c: inserting b proves cursor as
                # well as text survived. Only now permit Enter to execute it.
                start = len(output())
                key(qmp, "b")
                key(qmp, "ret")
                wait(r"\nabc\n", start)
                wait(r"fortress> ", start)
                assert output().count("FortressOS shell (Ring 3)") == 1, "Shell restarted"
                boot_output = log.read_text(errors="replace")
                if SMP == "1":
                    assert "Single enabled CPU" in boot_output
                else:
                    matches = re.findall(r"All (\d+) application processor\(s\) online", boot_output)
                    assert matches and int(matches[-1]) == int(SMP)-1, matches
                print(f"PASS S8 jobs idle {mode}, SMP={SMP}, AP count verified; idle Done + draft/cursor preserved; no data disks", flush=True)
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
