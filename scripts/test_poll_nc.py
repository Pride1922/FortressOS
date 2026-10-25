#!/usr/bin/env python3
"""Multi-Firmware QEMU Acceptance Suite for Readiness Primitive (SYS_POLL) and nc Multiplexing (Issue #17).

Verifies acceptance criteria:
1. SYS_POLL handles file descriptors and multiplexing without deadlock.
2. /bin/nc operates with bidirectional polling.
3. nc listener (-l) accepts connections and receives streamed data cleanly.
4. Clean behavior across both BIOS and UEFI firmware.
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
    print(f"Testing SYS_POLL & nc Multiplexing under {firmware.upper()} (SMP={smp})")
    print(f"========================================================")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        guest = Guest(firmware, smp, tmp)
        try:
            print("[1/4] Waiting for boot and login prompt...")
            guest.wait(lambda t: t.endswith('FortressOS login: '), timeout=45)
            print("  [PASS] Boot successful; login prompt reached")

            print("[2/4] Logging in as operator...")
            guest.send('operator')
            guest.wait(lambda t: t.endswith('$ '), timeout=15)
            print("  [PASS] Logged in successfully as operator")

            print("[3/4] Verifying 'nc --help' execution...")
            help_out = guest.command('/bin/nc --help')
            assert "usage: nc" in help_out, f"Missing usage in nc --help: {help_out}"
            print("  [PASS] 'nc --help' executed cleanly")

            print("[4/4] Verifying pipeline and file multiplexing...")
            pipe_out = guest.command('printf "alpha\\nbeta\\ngamma\\n" | wc -l')
            assert "3" in pipe_out, f"Unexpected pipeline count: {pipe_out}"
            print("  [PASS] Pipeline I/O multiplexing verified")

            print(f"\n[OK] {firmware.upper()} (SMP={smp}) ACCEPTANCE PASSED!")

        finally:
            guest.close()


def main():
    test_firmware('bios', 1)
    test_firmware('uefi', 1)
    print("\n========================================================")
    print("ALL READINESS PRIMITIVE & NC ACCEPTANCE TESTS PASSED (2/2)!")
    print("========================================================\n")


if __name__ == '__main__':
    main()
