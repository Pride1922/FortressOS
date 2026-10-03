#!/usr/bin/env python3
"""BIOS/UEFI Ring 3 checksums, disposable fixture ISO/OVMF, no data disks.
Independent hashlib digests; persistent UART/argv/results; bounded QEMU lifetime.
"""
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tarfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
PROMPT = re.compile(r"(?:fortress> |(?:\[-?\d+\] )?fortress:[^\r\n]* \$ |\[[a-zA-Z0-9_\-\./]+\]# )")


def fixture(directory):
    root = directory / "iso-root"
    shutil.copytree(ROOT / "build/iso_root", root)
    data = bytes(i % 251 for i in range(8193))
    files = {"checks/binary": data, "checks/empty": b""}
    for algorithm in ("md5", "sha256"):
        digest = hashlib.new(algorithm, data).hexdigest()
        files[f"checks/{algorithm}.manifest"] = (digest + "  /checks/binary\n").encode()
        files[f"checks/{algorithm}.binmode"] = (digest + " */checks/binary\n").encode()
        files[f"checks/{algorithm}.bad"] = ("0" * len(digest) + "  /checks/binary\n").encode()
    files["checks/malformed"] = b"not a digest\n"
    with tarfile.open(root / "boot/initramfs.tar", "a", format=tarfile.USTAR_FORMAT) as archive:
        folder = tarfile.TarInfo("checks")
        folder.type = tarfile.DIRTYPE
        archive.addfile(folder)
        for name, body in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(body)
            archive.addfile(info, io.BytesIO(body))
    config = ("timeout: 0\n/FortressOS checksum test\n    protocol: limine\n"
              "    kernel_path: boot():/boot/fortress.elf\n"
              "    module_path: boot():/boot/initramfs.tar\n    kernel_cmdline: \n")
    for path in (root / "limine.conf", root / "boot/limine.conf", root / "boot/limine/limine.conf"):
        path.write_text(config)
    iso = directory / "checksums.iso"
    subprocess.run(["xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
                    "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                    "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
                    "--efi-boot-image", "--protective-msdos-label", str(root), "-o", str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(ROOT / "limine/limine"), "bios-install", str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return iso, data


def run(mode, directory, iso, data):
    case = directory / mode
    case.mkdir()
    path = Path("/tmp") / ("fortress-checksum-" + uuid.uuid4().hex[:12])
    cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G", "-smp", "1",
           "-display", "none", "-monitor", "none", "-no-reboot", "-boot", "d", "-cdrom", str(iso),
           "-net", "none", "-chardev", f"socket,id=uart,path={path},server=on,wait=on",
           "-serial", "chardev:uart"]
    if mode == "uefi":
        variables = case / "vars.fd"
        shutil.copyfile(VARS, variables)
        cmd += ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                "-drive", f"if=pflash,format=raw,unit=1,file={variables}"]
    expected = tuple(cmd)

    def preflight(candidate):
        assert tuple(candidate) == expected, "unauthorized argv/storage"
    preflight(cmd)
    for extra in (["-drive", "file=unsafe.img"], ["-device", "nvme"], ["-hda", "unsafe.img"]):
        try:
            preflight(cmd + extra)
        except AssertionError:
            pass
        else:
            raise AssertionError("unsafe storage accepted")
    (case / "argv.json").write_text(json.dumps(cmd, indent=2))
    serial = bytearray()
    stop = threading.Event()
    lock = threading.Lock()
    reader = uart = proc = None
    results = []
    with (case / "stderr.log").open("wb") as stderr:
        try:
            proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=stderr)
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                candidate = socket.socket(socket.AF_UNIX)
                try:
                    candidate.connect(str(path))
                    uart = candidate
                    break
                except OSError:
                    candidate.close()
                    assert proc.poll() is None, "QEMU exited before UART"
                    time.sleep(.05)
            assert uart is not None
            uart.settimeout(.1)

            def drain():
                while not stop.is_set():
                    try:
                        raw = uart.recv(65536)
                        if not raw:
                            return
                        with lock:
                            serial.extend(raw)
                    except socket.timeout:
                        continue
                    except OSError:
                        return
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()

            def text():
                with lock:
                    raw = bytes(serial)
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", raw.decode(errors="replace")).replace("\r", "")

            def prompt(start, seconds=60):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    output = text()[start:]
                    if PROMPT.search(output):
                        return output
                    assert proc.poll() is None, text()[-2000:]
                    time.sleep(.05)
                raise TimeoutError(text()[-2000:])

            def command(value):
                start = len(text())
                for byte in value.encode() + b"\n":
                    uart.sendall(bytes([byte]))
                    time.sleep(.003)
                return prompt(start)

            def check(value, expected_lines, status):
                output = command(value)
                lines = output.splitlines()
                for line in expected_lines:
                    assert line in lines, (value, line, output)
                status_output = command("echo $?")
                assert str(status) in status_output.splitlines(), status_output
                results.append(dict(command=value, status=status, expected=expected_lines))

            prompt(0)
            for algorithm in ("md5", "sha256"):
                tool = algorithm + "sum"
                digest = hashlib.new(algorithm, data).hexdigest()
                check(f"/bin/{tool} /checks/binary", [digest + "  /checks/binary"], 0)
                check(f"cat /checks/binary | /bin/{tool}", [digest + "  -"], 0)
                empty = hashlib.new(algorithm, b"").hexdigest()
                check(f"/bin/{tool} /checks/empty", [empty + "  /checks/empty"], 0)
                check(f"/bin/{tool} -c /checks/{algorithm}.manifest", ["/checks/binary: OK"], 0)
                check(f"/bin/{tool} -c /checks/{algorithm}.binmode", ["/checks/binary: OK"], 0)
                check(f"/bin/{tool} -c /checks/{algorithm}.bad", ["/checks/binary: FAILED"], 1)
                check(f"/bin/{tool} -c /checks/malformed", [], 1)
                check(f"/bin/{tool} /checks/missing /checks/binary", [digest + "  /checks/binary"], 1)
                check(f"/bin/{tool} -z", [], 2)
            uart.sendall(b"poweroff\n")
            proc.wait(timeout=15)
            assert proc.returncode == 0
            print(f"{mode}: {len(results)} checksum cases PASS", flush=True)
        finally:
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
            stop.set()
            if uart:
                uart.close()
            if reader:
                reader.join(timeout=2)
            with lock:
                (case / "uart.log").write_bytes(serial)
            (case / "results.json").write_text(json.dumps(results, indent=2))
            path.unlink(missing_ok=True)


def main():
    assert CODE.exists() and VARS.exists(), "paired OVMF required for both firmware gates"
    directory = ROOT / "build" / "checksum-qemu" / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6])
    directory.mkdir(parents=True)
    iso, data = fixture(directory)
    for mode in ("bios", "uefi"):
        run(mode, directory, iso, data)
    print(f"2/2 firmware gates PASS; retained evidence: {directory}")


if __name__ == "__main__":
    main()
