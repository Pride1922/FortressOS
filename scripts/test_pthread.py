#!/usr/bin/env python3
"""Multi-Firmware QEMU Acceptance Suite for Userland Threads & pthreads (Issue #12).

Verifies acceptance criteria:
1. Kernel manages shared address space (CR3/PML4) across multiple concurrent threads.
2. Credentials and file descriptors are properly shared in the process_t object.
3. Thread creation (SYS_THREAD_CREATE) and thread joining (SYS_THREAD_JOIN) execute cleanly.
4. Futex-based synchronization (SYS_FUTEX) coordinates pthread_mutex_t, pthread_cond_t, and pthread_barrier_t.
5. All tests in /bin/pthread-test pass with zero deadlocks, race conditions, or memory corruption.
6. Acceptance verified across BIOS and UEFI firmware (with SMP=1 and SMP=4).
"""

import os
import re
import selectors
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class Guest:
    def __init__(self, firmware, smp, tmp):
        self.firmware = firmware
        self.smp = smp
        self.data = bytearray()
        argv = [
            'qemu-system-x86_64', '-M', 'q35', '-m', '512M', '-smp', str(smp),
            '-accel', 'tcg', '-cpu', 'max,+rdrand,+rdseed', '-display', 'none',
            '-monitor', 'none', '-serial', 'stdio', '-no-reboot', '-boot', 'd',
            '-cdrom', str(ROOT / 'bin/fortress.iso'),
            '-netdev', 'user,id=n0', '-device', 'e1000e,netdev=n0'
        ]
        if firmware == 'uefi':
            variables = tmp / f'vars-{smp}.fd'
            shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
            argv += [
                '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                '-drive', f'if=pflash,format=raw,unit=1,file={variables}'
            ]
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.proc.stdout, selectors.EVENT_READ)

    def pump(self):
        for key, _ in self.sel.select(0.05):
            self.data.extend(key.fileobj.read1(65536))

    def text(self, start=0):
        cleaned = re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', self.data[start:].decode(errors='replace'))
        return cleaned.replace('\r', '')

    def wait(self, predicate, start=0, timeout=60):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump()
            t = self.text(start)
            if predicate(t):
                return t
            assert self.proc.poll() is None, 'Guest terminated unexpectedly: ' + t[-1500:]
        raise AssertionError(f'Timeout waiting for condition after {timeout}s: ' + t[-2000:])

    def send(self, line):
        self.proc.stdin.write((line + '\n').encode())
        self.proc.stdin.flush()

    def command(self, cmd, timeout=30):
        start = len(self.data)
        self.send(cmd)
        result = self.wait(lambda s: '\n' in s and s.endswith('$ '), start, timeout)
        return result.split('\n', 1)[1]

    def close(self):
        try:
            self.send('sudo /bin/poweroff')
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
            self.proc.wait()


def test_firmware(firmware, smp):
    print(f"\n========================================================")
    print(f"Testing Userland Threads (pthreads) under {firmware.upper()} (SMP={smp})")
    print(f"========================================================")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        guest = Guest(firmware, smp, tmp)
        try:
            print("[1/3] Waiting for boot and login prompt...")
            guest.wait(lambda t: t.endswith('FortressOS login: '), timeout=45)
            print("  [PASS] Boot successful; login prompt reached")

            print("[2/3] Logging in as operator...")
            guest.send('operator')
            guest.wait(lambda t: t.endswith('$ '), timeout=15)
            print("  [PASS] Logged in successfully as operator")

            print("[3/3] Running /bin/pthread-test concurrency suite...")
            test_out = guest.command('/bin/pthread-test')
            print(test_out)
            assert "All Userland Thread Tests PASSED" in test_out, f"Test suite did not report PASS: {test_out}"
            print("  [PASS] /bin/pthread-test executed and 100% of subtests passed")

            print(f"\n[OK] {firmware.upper()} (SMP={smp}) PTHREAD ACCEPTANCE PASSED!")

        finally:
            guest.close()


def main():
    test_firmware('bios', 1)
    test_firmware('bios', 4)
    test_firmware('uefi', 1)
    test_firmware('uefi', 4)
    print("\n========================================================")
    print("ALL USERLAND THREAD (PTHREADS) ACCEPTANCE TESTS PASSED (4/4)!")
    print("========================================================\n")


if __name__ == '__main__':
    main()
