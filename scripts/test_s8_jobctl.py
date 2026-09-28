#!/usr/bin/env python3
"""BIOS/UEFI real-shell job control, terminal restore and exit cleanup; no data disks."""
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
import traceback
from test_nmi_transitions import QMP, connect

REPO = Path(__file__).resolve().parent.parent
SMP = os.environ.get("SMP", "1")
assert SMP in ("1", "4", "8")


def key(qmp, code):
    qmp.execute("input-send-event", {"events": [
        {"type": "key", "data": {"down": down,
         "key": {"type": "qcode", "data": code}}} for down in (True, False)]})
    time.sleep(0.03)


with tempfile.TemporaryDirectory(prefix="fortress-jobctl-") as directory:
    tmp = Path(directory)
    root = tmp / "iso"
    shutil.copytree(REPO / "build/iso_root", root)
    archive = tmp / "initramfs.tar"
    with tarfile.open(REPO / "bin/initramfs.tar") as src, tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as dst:
        for member in src.getmembers():
            # Preserve the real shell and every production initramfs member.
            dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        dst.add(REPO / "build/s8_jobs_delay_user.elf", arcname="bin/idle-delay")
        dst.add(REPO / "build/s8_jobctl_user.elf", arcname="bin/jobctl-probe")
    for path in (root / "boot/initramfs.tar", root / "initramfs.tar"):
        shutil.copyfile(archive, path)
    config = ("timeout: 0\n/FortressOS Job Control Test\n    protocol: limine\n"
              "    kernel_path: boot():/boot/fortress.elf\n    module_path: boot():/boot/initramfs.tar\n")
    for path in (root / "limine.conf", root / "boot/limine.conf", root / "boot/limine/limine.conf"):
        path.write_text(config)
    iso = tmp / "jobctl.iso"
    subprocess.run(["xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
                    "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                    "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
                    "--efi-boot-image", "--protective-msdos-label", str(root), "-o", str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO / "limine/limine"), "bios-install", str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for mode in ("bios", "uefi"):
        log = REPO / "build" / f"s8-jobctl-{mode}-{SMP}.log"
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
        with (REPO / "build" / f"s8-jobctl-{mode}-{SMP}.stderr").open("wb") as stderr:
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
                deadline = time.monotonic() + 420

                def output():
                    raw = log.read_bytes().decode(errors="replace")
                    assert proc.poll() is None, raw[-3000:]
                    assert "[FATAL]" not in raw and "[FAIL]" not in raw and "JOBCTL FAIL" not in raw, raw[-3000:]
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

                wait(r"fortress> ", seconds=90)
                start = len(output())
                type_uart("layout us\n")
                wait(r"Keyboard layout set to US QWERTY", start)
                wait(r"fortress> ", start)

                def command(line):
                    start = len(output())
                    type_uart(line + "\n")
                    wait(r"\nfortress> ", start)
                    return output()[start:]

                def ctrl(code):
                    qmp.execute("input-send-event", {"events": [
                        {"type": "key", "data": {"down": down,
                         "key": {"type": "qcode", "data": name}}}
                        for name, down in (("ctrl", True), (code, True),
                                           (code, False), ("ctrl", False))]})

                def launch(line):
                    start = len(output())
                    type_uart(line + " &\n")
                    match, end = wait(r"\[(\d+)\] (\d+)\n", start)
                    _, prompt_end = wait(r"fortress> ", end)
                    return match.group(1), match.group(2), prompt_end

                def state(job, name):
                    text = command("jobs")
                    assert re.search(rf"\[{job}\][+ -] {name}", text), text

                def stop_foreground(job, start):
                    # Ctrl+Z flushes queued ordinary input even when the shell
                    # ignores TSTP. Never send it until the helper confirms its
                    # group owns the terminal; retries can erase the fg command.
                    wait(r"JOBCTL FOREGROUND\n", start)
                    ctrl("z")
                    wait(rf"\[{job}\]  Stopped", start)

                 # Multi-stage group: every live member must stop/resume/exit.
                job, pgid, _ = launch("/bin/jobctl-probe loop | /bin/jobctl-probe loop")
                state(job, "Running")
                start = len(output()); type_uart(f"fg %{job}\n")
                wait(r"JOBCTL FOREGROUND", start)
                type_uart("\x1a")
                wait(rf"\[{job}\]  Stopped", start); wait(r"fortress> ", start)
                state(job, "Stopped")
                command("bg") # default %+ is the most recently stopped job
                state(job, "Running")
                # Foreground completion must still drain another background job.
                delayed, _, _ = launch("/bin/idle-delay")
                start = len(output()); type_uart(f"fg %{job}\n")
                wait(r"JOBCTL FOREGROUND", start)
                # No input until the other job finishes: this is the required
                # background-drain-during-fg gate, and removes the delayed job
                # before the later empty-current-job error assertion.
                wait(rf"\[{delayed}\]  Done", start)
                type_uart("\x1a")
                wait(rf"\[{job}\]  Stopped", start); wait(r"fortress> ", start)
                state(job, "Stopped")
                command(f"bg %{job}")
                start = len(output()); type_uart(f"fg %{job}\n")
                wait(r"JOBCTL FOREGROUND", start)
                type_uart("\x1a")
                wait(rf"\[{job}\]  Stopped", start); wait(r"fortress> ", start)
                command(f"bg %{job}")
                start = len(output()); type_uart(f"kill %{job}\n")
                wait(rf"\[{job}\]  Terminated\(signal 15\)", start)
                wait(r"fortress> ", start)
                assert "JOBCTL ABSENT PASS" in command(f"/bin/jobctl-probe absent {pgid}")
                remaining = command("jobs")
                assert not re.search(r"\[\d+\][+ -] (?:Running|Stopped)", remaining), remaining
                assert "no such job" in command("fg %99")
                for line in ("fg %", "bg %0", "kill %+"):
                    text = command(line)
                    assert "invalid job specifier" in text or "no such job" in text, text
                assert "cannot run as a pipeline stage" in command("jobs | cat")

                # Single direct external command uses the same foreground path.
                start = len(output()); type_uart("/bin/jobctl-probe attrs\n")
                match, end = wait(r"\[(\d+)\]  Stopped", start)
                attr_job = match.group(1); wait(r"fortress> ", end)
                assert "JOBCTL SHELL ATTRS PASS" in command("/bin/jobctl-probe shell-attrs")
                assert "JOBCTL ATTRS PASS" in command(f"fg %{attr_job}")
                assert "JOBCTL SHELL ATTRS PASS" in command("/bin/jobctl-probe shell-attrs")

                # Background stdin stops with TTIN, fg hands off before CONT.
                start = len(output())
                reader_job, _, _ = launch("/bin/jobctl-probe read")
                wait(rf"\[{reader_job}\]  Stopped", start)
                state(reader_job, "Stopped")
                start = len(output()); type_uart(f"fg %{reader_job}\n")
                time.sleep(0.3); type_uart("Q")
                wait(r"JOBCTL READ PASS", start); wait(r"fortress> ", start)
                # Prompt controls still act on the shell, not an old job group.
                start = len(output()); type_uart("echo discarded"); ctrl("c")
                wait(r"\nfortress> ", start)
                ctrl("z"); assert re.search(r"\nprompt-ok\n", command("echo prompt-ok"))

                # Kernel parent-death path, with no shell jobs_shutdown call.
                text = command("/bin/jobctl-probe orphan")
                for label in ("ORPHAN", "STAGED"):
                    match = re.search(rf"JOBCTL {label} (\d+)", text)
                    assert match, text
                    assert "JOBCTL ABSENT PASS" in command(f"/bin/jobctl-probe absent {match.group(1)}")

                # Full-pipe producer and stopped consumer ignore HUP. Exit must
                # CONT, allow the grace interval, KILL, collect and restart.
                start = len(output())
                job, pgid, _ = launch("/bin/jobctl-probe writer | /bin/jobctl-probe hold")
                wait(r"JOBCTL PIPE FULL", start)
                wait(r"JOBCTL READY", start) # consumer installed ignored HUP
                time.sleep(0.3)
                start = len(output()); type_uart(f"kill %{job} STOP\n")
                wait(rf"\[{job}\]  Stopped", start); wait(r"fortress> ", start)
                start = len(output()); type_uart("exit\n")
                wait(r"FortressOS shell \(Ring 3\)", start); wait(r"fortress> ", start)
                assert "JOBCTL ABSENT PASS" in command(f"/bin/jobctl-probe absent {pgid}")
                # Idle completion still works after the complete fg/bg lifecycle.
                job, _, idle_start = launch("/bin/idle-delay")
                # Keep the matched prompt cursor: a new len(output()) can
                # skip Done if it arrived before Python resumed polling.
                _, done_end = wait(rf"\[{job}\]  Done +/bin/idle-delay &\n", idle_start)
                wait(r"fortress> ", done_end)
                boot_output = log.read_text(errors="replace")
                if SMP == "1":
                    assert "Single enabled CPU" in boot_output
                else:
                    matches = re.findall(r"All (\d+) application processor\(s\) online", boot_output)
                    assert matches and int(matches[-1]) == int(SMP)-1, matches
                print(f"PASS S8 jobctl {mode}, SMP={SMP}, AP count verified; stop/bg/fg/kill, attributes, orphan/exit cleanup; no data disks", flush=True)
            except Exception:
                (REPO / "build" / f"s8-jobctl-{mode}-{SMP}.failure").write_text(traceback.format_exc())
                raise
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
