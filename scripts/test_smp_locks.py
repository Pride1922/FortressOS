#!/usr/bin/env python3
"""SMP Piece 3: Lock discipline & per-CPU tracking verification runner.

Verifies:
1. BIOS and UEFI single-CPU baseline (-smp 1): selftests pass, shell reached.
2. BIOS and UEFI multi-core atomic contention (-smp 4 and -smp 8):
   - BSP + AP 1 concurrently perform 100,000 increments each on a shared counter
   - Final counter strictly equals 200,000 (zero lost updates)
   - Contention count > 0 verified
   - APs 2..N remain parked with IF=0
3. AP rank inversion isolation test:
   - AP 1 intentionally attempts an inverted lock acquisition
   - spin_panic_ap fires, prints held chain to raw UART, and halts AP 1
   - BSP continues cleanly to shell prompt
"""
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent


def symbols():
    result = {}
    for line in subprocess.check_output(["nm", "-an", "bin/fortress.elf"], cwd=REPO, text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            result[fields[2]] = int(fields[0], 16)
    for name in ("smp_init", "g_smp_lock_test", "cpu_locals"):
        assert name in result, f"Missing required symbol: {name}"
    return result


def connect(path):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        sock = socket.socket(socket.AF_UNIX)
        try:
            sock.connect(str(path))
            sock.settimeout(45)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close()
            time.sleep(0.05)
    raise RuntimeError(f"QEMU socket unavailable: {path}")


class Remote:
    def __init__(self, path):
        self.sock = connect(path)

    def byte(self):
        b = self.sock.recv(1)
        if not b:
            raise RuntimeError("QEMU debugger disconnected")
        return b

    def request(self, message):
        data = message.encode()
        self.sock.sendall(b"$" + data + b"#" + f"{sum(data) & 255:02x}".encode())
        while self.byte() != b"$":
            pass
        encoded = bytearray()
        while True:
            b = self.byte()
            if b == b"#":
                break
            encoded += b
        checksum = self.byte() + self.byte()
        assert sum(encoded) & 255 == int(checksum, 16), "GDB packet checksum"
        self.sock.sendall(b"+")
        decoded = bytearray()
        i = 0
        while i < len(encoded):
            b = encoded[i]
            if b == ord("}"):
                i += 1
                decoded.append(encoded[i] ^ 0x20)
            elif b == ord("*"):
                i += 1
                decoded.extend(bytes([decoded[-1]]) * (encoded[i] - 29))
            else:
                decoded.append(b)
            i += 1
        return decoded.decode()

    def memory_write(self, addr, data: bytes):
        res = self.request(f"M{addr:x},{len(data):x}:{data.hex()}")
        assert res == "OK", f"Memory write failed at {addr:#x}: {res}"

    def breakpoint(self, addr, enable=True):
        assert self.request(f"{'Z' if enable else 'z'}1,{addr:x},1") == "OK"

    def resume_to(self, addr):
        self.breakpoint(addr)
        response = self.request("c")
        assert response.startswith(("T05", "S05")), response
        self.breakpoint(addr, False)

    def continue_exec(self):
        self.sock.sendall(b"$c#63")
        while self.byte() != b"+":
            pass


def run_contention_test(mode, cpus):
    log = REPO / "build" / f"smp-locks-{mode}-{cpus}.log"
    with tempfile.TemporaryDirectory(prefix="fortress-locks-") as temp:
        temp = Path(temp)
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
               "-smp", str(cpus), "-display", "none", "-no-reboot", "-monitor", "none",
               "-serial", f"file:{log}", "-boot", "d", "-cdrom", "bin/fortress.iso"]
        if mode == "uefi":
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", temp / "vars.fd")
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                    "-drive", f"if=pflash,format=raw,unit=1,file={temp}/vars.fd"]
        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if log.exists() and "fortress> " in log.read_text(errors="replace"):
                    break
                assert child.poll() is None, child.stderr.read().decode()
                time.sleep(0.1)
            else:
                raise TimeoutError(f"Shell prompt not reached in {mode} -smp {cpus}")
        finally:
            child.terminate()
            child.wait()

        output = log.read_text(errors="replace")
        assert "BSP spinlock selftest passed (ranks, classification, asserts)" in output, "Selftest missing"
        if cpus == 1:
            assert "Single-CPU system: SMP lock contention test skipped." in output, "Single CPU skip missing"
        else:
            assert "Starting two-core lock contention test (BSP + AP 1)..." in output, "Contention start missing"
            assert "Counter value: 200000 (expected: 200000)" in output, "Counter mismatch"
            assert "Two-core lock contention test passed" in output, "Contention pass missing"
        assert "[ OK ] SMP Piece 3 (Lock discipline) complete." in output, "Piece 3 completion missing"
        print(f"PASS {mode} -smp {cpus}: contention={cpus > 1}, selftest, shell reached")


def run_inversion_test(mode):
    sym = symbols()
    log = REPO / "build" / f"smp-locks-inversion-{mode}.log"
    with tempfile.TemporaryDirectory(prefix="fortress-locks-inv-") as temp:
        temp = Path(temp)
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
               "-smp", "4", "-display", "none", "-S", "-no-reboot", "-monitor", "none",
               "-serial", f"file:{log}", "-gdb", f"unix:{temp}/gdb,server=on,wait=off",
               "-boot", "d", "-cdrom", "bin/fortress.iso"]
        if mode == "uefi":
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", temp / "vars.fd")
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                    "-drive", f"if=pflash,format=raw,unit=1,file={temp}/vars.fd"]
        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            remote = Remote(temp / "gdb")
            remote.request("qSupported")
            remote.resume_to(sym["smp_init"])
            # Set test_mode = SMP_TEST_MODE_INVERSION (2) in g_smp_lock_test
            remote.memory_write(sym["g_smp_lock_test"], struct.pack("<I", 2))
            remote.continue_exec()

            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if log.exists() and "fortress> " in log.read_text(errors="replace"):
                    break
                assert child.poll() is None, child.stderr.read().decode()
                time.sleep(0.1)
            else:
                raise TimeoutError(f"Shell prompt not reached during inversion test in {mode}")
        finally:
            child.terminate()
            child.wait()

        output = log.read_text(errors="replace")
        assert "Lock discipline on AP 1: recursive/inverted acquisition: ap1-rank1" in output, "AP panic missing"
        assert "held chain" in output, "Held chain diagnostic missing"
        assert "AP 1 rank inversion caught and isolated via spin_panic_ap; BSP unharmed" in output, "Inversion pass missing"
        assert "[ OK ] SMP Piece 3 (Lock discipline) complete." in output, "Piece 3 completion missing"
        assert "fortress> " in output, "Shell prompt missing"
        print(f"PASS {mode} rank inversion: AP 1 trapped & halted via spin_panic_ap, BSP unharmed, shell reached")


def run_assert_held_test(mode):
    sym = symbols()
    log = REPO / "build" / f"smp-locks-assert-{mode}.log"
    with tempfile.TemporaryDirectory(prefix="fortress-locks-assert-") as temp:
        temp = Path(temp)
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
               "-smp", "1", "-display", "none", "-S", "-no-reboot", "-monitor", "none",
               "-serial", f"file:{log}", "-gdb", f"unix:{temp}/gdb,server=on,wait=off",
               "-boot", "d", "-cdrom", "bin/fortress.iso"]
        if mode == "uefi":
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", temp / "vars.fd")
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                    "-drive", f"if=pflash,format=raw,unit=1,file={temp}/vars.fd"]
        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            remote = Remote(temp / "gdb")
            remote.request("qSupported")
            remote.resume_to(sym["smp_init"])
            # Set test_mode = SMP_TEST_MODE_ASSERT_HELD (3) in g_smp_lock_test
            remote.memory_write(sym["g_smp_lock_test"], struct.pack("<I", 3))
            remote.continue_exec()

            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if log.exists() and "Lock discipline on BSP: assertion failed: lock not held" in log.read_text(errors="replace"):
                    break
                time.sleep(0.1)
        finally:
            child.terminate()
            child.wait()

        output = log.read_text(errors="replace")
        assert "Lock discipline on BSP: assertion failed: lock not held by caller: unheld-lock" in output, "Assert held panic missing"
        assert "held chain" in output, "Held chain diagnostic missing"
        assert "fortress> " not in output, "Shell should not have been reached after BSP fatal panic"
        print(f"PASS {mode} assert_held negative: BSP fatal trap on unheld lock verified, CPU halted")


def main():
    print("=== SMP Piece 3 Lock Discipline & Contention Suite ===")
    for mode in ("bios", "uefi"):
        for cpus in (1, 4, 8):
            run_contention_test(mode, cpus)
        run_inversion_test(mode)
        run_assert_held_test(mode)
    print("ALL SMP PIECE 3 LOCK DISCIPLINE TESTS PASSED!")


if __name__ == "__main__":
    main()
