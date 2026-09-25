#!/usr/bin/env python3
"""UEFI 8 GiB boot, absent COM1, non-fixture NVMe ID: keyboard-only shell.

Inspects the console cache read-only and captures actual framebuffer output.
The NVMe device uses Intel IDs to exercise the hardware boot path which skips
storage fixture tests. This does not validate the physical Dell's controller.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from test_nmi_transitions import REPO, Remote, QMP, symbols

sym = symbols()
with tempfile.TemporaryDirectory(prefix="fortress-keyboard-") as tmp:
    tmp = Path(tmp)
    shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", tmp / "vars.fd")
    cmd = ["qemu-system-x86_64", "-M", "q35", "-m", "8G", "-S", "-display", "none",
           "-serial", "none", "-monitor", "none", "-no-reboot", "-boot", "d",
           "-cdrom", "bin/fortress.iso", "-gdb", f"unix:{tmp}/gdb,server=on,wait=off",
           "-qmp", f"unix:{tmp}/qmp,server=on,wait=off",
           "-drive", "file=build/nvme_gpt.img,if=none,id=nvm0,format=raw,snapshot=on",
           "-device", "nvme,serial=fortress0,drive=nvm0,use-intel-id=on",
           "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
           "-drive", f"if=pflash,format=raw,unit=1,file={tmp}/vars.fd"]
    child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        qmp, remote = QMP(tmp / "qmp"), Remote(tmp / "gdb")
        remote.request("qSupported")
        remote.request("qXfer:features:read:target.xml:0,fff")
        remote.resume_to(sym["input_read_timeout"])
        assert remote.memory(sym["serial_available"], 1) == b"\0"
        qmp.execute("cont")
        time.sleep(0.2)
        for c in "echo hello\n":
            code = {" ": "spc", "\n": "ret"}.get(c, c)
            qmp.execute("input-send-event", {"events": [
                {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": code}}}
                for down in (True, False)]})
            time.sleep(0.04)
        time.sleep(0.2)
        qmp.execute("stop")
        # These debug-only layouts match console.c: 8-byte geometry, 12-byte cells.
        console = remote.memory(sym["g_console"], 48)
        cols, rows = [int.from_bytes(console[i:i+8], "little") for i in (32, 40)]
        assert 0 < cols <= 512 and 0 < rows <= 256
        lines = []
        for row in range(rows):
            data = bytearray()
            for offset in range(0, cols * 12, 1024):
                data += remote.memory(sym["cells"] + row * 512 * 12 + offset, min(1024, cols * 12 - offset))
            lines.append(bytes(data[8::12]).decode("ascii").rstrip())
        text = "\n".join(lines)
        assert "QEMU storage fixture tests skipped" in text
        assert "fortress> echo hello\nhello\nfortress>" in text, text
        from test_shell import scheduler_symbols
        scheduler_symbols(remote, sym)
        assert int.from_bytes(remote.memory(sym["g_blocked_threads"], 8), "little") != 0
        screenshot = REPO / "build" / "shell-keyboard-only.png"
        qmp.execute("screendump", {"filename": str(screenshot), "format": "png"})
        print(f"PASS keyboard-only UEFI 8 GiB: hardware boot path, no UART, real PS/2 echo, sleeping reader ({screenshot})")
    finally:
        child.terminate()
        try: child.wait(timeout=5)
        except subprocess.TimeoutExpired: child.kill(); child.wait()
