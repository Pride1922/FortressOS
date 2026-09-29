#!/usr/bin/env python3
"""BIOS/UEFI SIGPIPE and real-shell status gate; disposable ISO, no data disks."""
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
from test_nmi_transitions import connect

REPO = Path(__file__).resolve().parent.parent
SMP = os.environ.get("SMP", "1")
assert SMP in ("1", "4", "8")

with tempfile.TemporaryDirectory(prefix="fortress-sigpipe-") as directory:
    tmp = Path(directory)
    root = tmp / "iso"
    shutil.copytree(REPO / "build/iso_root", root)
    archive = tmp / "initramfs.tar"
    with tarfile.open(REPO / "bin/initramfs.tar") as src, tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as dst:
        for member in src.getmembers():
            dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        dst.add(REPO / "build/s8_sigpipe_user.elf", arcname="bin/sigpipe-probe")
    for path in (root / "boot/initramfs.tar", root / "initramfs.tar"):
        shutil.copyfile(archive, path)
    config = ("timeout: 0\n/FortressOS SIGPIPE Test\n    protocol: limine\n"
              "    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n")
    for path in (root / "limine.conf", root / "boot/limine.conf", root / "boot/limine/limine.conf"):
        path.write_text(config)
    iso = tmp / "sigpipe.iso"
    subprocess.run(["xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
                    "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                    "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
                    "--efi-boot-image", "--protective-msdos-label", str(root), "-o", str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO / "limine/limine"), "bios-install", str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for mode in ("bios", "uefi"):
        log = REPO / "build" / f"s8-sigpipe-{mode}-{SMP}.log"
        log.write_text("")
        uart_path = tmp / f"uart-{mode}"
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
               "-smp", SMP, "-display", "none", "-monitor", "none", "-no-reboot",
               "-boot", "d", "-cdrom", str(iso),
               "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
               "-serial", "chardev:uart"]
        firmware = []
        if mode == "uefi":
            variables = tmp / "vars.fd"
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)
            firmware = ["if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                        f"if=pflash,format=raw,unit=1,file={variables}"]
            for drive in firmware:
                cmd += ["-drive", drive]
        assert [cmd[i+1] for i, arg in enumerate(cmd) if arg == "-drive"] == firmware
        assert not any(arg in cmd for arg in ("-blockdev", "-device", "-hda", "-hdb", "-snapshot"))
        assert cmd[cmd.index("-cdrom")+1] == str(iso)
        with (REPO / "build" / f"s8-sigpipe-{mode}-{SMP}.stderr").open("wb") as stderr:
            proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
            uart = reader = None
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
                deadline = time.monotonic() + 300

                def output():
                    raw = log.read_text(errors="replace")
                    assert proc.poll() is None, raw[-3000:]
                    assert not any(s in raw for s in ("[FATAL]", "[FAIL]", "S8 SIGPIPE FAIL")), raw[-3000:]
                    return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", raw).replace("\r", "")

                def wait(pattern, after=0):
                    end = min(deadline, time.monotonic() + 90)
                    while time.monotonic() < end:
                        text = output()
                        match = re.search(pattern, text[after:])
                        if match:
                            return after + match.end()
                        time.sleep(0.02)
                    raise AssertionError(f"Timeout {pattern!r}: {log}\n{output()[-3000:]}")

                def command(line):
                    start = len(output())
                    for byte in (line + "\n").encode():
                        uart.sendall(bytes([byte]))
                        time.sleep(0.005)
                    # Require a new-line prompt, never the echoed command's prompt.
                    wait(r"\n(?:\[\d+\] )?fortress> ", start)
                    return output()[start:]

                wait(r"fortress> ")
                result = command("/bin/sigpipe-probe")
                for marker in ("dispositions and partial", "early close and stopped producer",
                               "head upstream signal metadata"):
                    assert f"S8 SIGPIPE {marker} PASS" in result, result
                assert "S8 SIGPIPE PASS" in result, result
                result = command("/bin/sigpipe-probe d")
                assert "Terminated(signal 13)" in result, result
                assert re.search(r"\n141\n", command("echo $?"))
                result = command("/bin/sigpipe-probe p | head -n 1")
                assert "Terminated(signal 13)" in result, result
                # Pipeline status stays the last stage's status, not the producer's.
                assert re.search(r"\n0\n", command("echo $?"))
                command("echo x | /bin/sigpipe-probe d")
                assert re.search(r"\n141\n", command("echo $?"))
                assert re.search(r"\n1\n", command("echo hello | cat | wc -l"))
                assert "Running" not in command("jobs")
                boot = log.read_text(errors="replace")
                assert boot.count("FortressOS shell (Ring 3)") == 1, "Shell restarted"
                if SMP == "1":
                    assert "Single enabled CPU" in boot
                else:
                    counts = re.findall(r"All (\d+) application processor\(s\) online", boot)
                    assert counts and int(counts[-1]) == int(SMP)-1, counts
                print(f"PASS S8 SIGPIPE {mode}, SMP={SMP}; dispositions, partial, stop/close, head, shell status; no data disks", flush=True)
            finally:
                stop.set()
                if uart:
                    uart.close()
                if reader:
                    reader.join(timeout=1)
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
