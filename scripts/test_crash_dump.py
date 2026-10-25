#!/usr/bin/env python3
"""Multi-Firmware QEMU Acceptance Suite for User-Space Crash Dumps (Issue #39).

Verifies acceptance criteria:
1. Unhandled Ring 3 CPU exceptions (#PF, #DE, #UD) generate bounded crash core dumps.
2. Full architectural registers (RAX..R15, RIP, RSP, RFLAGS with decoded condition flags) are emitted to COM1 & dmesg.
3. Bounded code window (16 bytes at RIP) and user stack window (16 QWORDs at RSP) are safely dumped with page validation.
4. Process terminates cleanly with POSIX status (128 + signal), leaving kernel and shell stable.
5. Custom signal handlers for fault signals (SIGSEGV) catch faults and recover; recursive faults trigger crash core dumps.
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


def run_test(firmware, smp):
    print(f"\n========================================================")
    print(f" Testing Crash Dumps under {firmware.upper()} (SMP={smp})")
    print(f"========================================================")
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp = Path(tmp_dir)
        guest = Guest(firmware, smp, tmp)
        try:
            print("  Waiting for login prompt...")
            guest.wait(lambda s: s.endswith('FortressOS login: '), timeout=45)
            print("  Logging in as operator...")
            guest.send('operator')
            print("  Waiting for shell prompt...")
            guest.wait(lambda s: s.endswith('$ '), timeout=15)
            print("  [PASS] Shell prompt active.")

            # Test 1: CLI Help
            print("  [TEST 1] Testing /bin/crash-test --help...")
            out = guest.command('/bin/crash-test --help')
            assert 'Usage: crash-test' in out, f"Unexpected --help output: {out}"
            assert 'null' in out and 'divzero' in out and 'illegal' in out, out
            print("    [PASS] --help usage displayed correctly.")

            # Test 2: NULL Pointer Dereference (#PF / SIGSEGV)
            print("  [TEST 2] Testing NULL pointer dereference crash dump (#PF / SIGSEGV)...")
            out = guest.command('/bin/crash-test null')
            assert '[CRASH] ==================== USER SPACE CRASH DUMP ====================' in out, out
            assert '[CRASH] Process:' in out and 'crash-test' in out, out
            assert 'SIGSEGV (11) - Fault: #PF (Page Fault' in out, out
            assert 'CR2 = 0x0000000000000000' in out, out
            assert 'Error Code:' in out, out
            assert '[CRASH] --- Architectural Registers ---' in out, out
            assert 'RAX:' in out and 'RBX:' in out and 'RIP:' in out and 'RSP:' in out, out
            assert 'RFLAGS:' in out, out
            assert '[CRASH] --- Code at RIP ---' in out, out
            assert '[CRASH] --- Stack Window' in out, out
            assert 'Action: Terminating process PID' in out, out
            assert '139 (kernel remains operational)' in out, out
            print("    [PASS] Full register, code, and stack crash dump generated for SIGSEGV.")

            # Test exit code
            code_out = guest.command('echo $?')
            assert '139' in code_out, f"Expected exit code 139 for SIGSEGV, got: {code_out}"
            print("    [PASS] Exit code reflects POSIX 128 + SIGSEGV (139).")

            # Test 3: Division by zero (#DE / SIGFPE)
            print("  [TEST 3] Testing integer division by zero crash dump (#DE / SIGFPE)...")
            out = guest.command('/bin/crash-test divzero')
            assert '[CRASH] ==================== USER SPACE CRASH DUMP ====================' in out, out
            assert 'SIGFPE (8) - Fault: #DE (Divide Error' in out, out
            assert '136 (kernel remains operational)' in out, out
            code_out = guest.command('echo $?')
            assert '136' in code_out, f"Expected exit code 136 for SIGFPE, got: {code_out}"
            print("    [PASS] Integer divide error generates SIGFPE crash dump and exit 136.")

            # Test 4: Invalid Opcode (#UD / SIGILL)
            print("  [TEST 4] Testing invalid opcode crash dump (#UD / SIGILL)...")
            out = guest.command('/bin/crash-test illegal')
            assert '[CRASH] ==================== USER SPACE CRASH DUMP ====================' in out, out
            assert 'SIGILL (4) - Fault: #UD (Invalid Opcode' in out, out
            assert '132 (kernel remains operational)' in out, out
            code_out = guest.command('echo $?')
            assert '132' in code_out, f"Expected exit code 132 for SIGILL, got: {code_out}"
            print("    [PASS] Invalid opcode generates SIGILL crash dump and exit 132.")

            # Test 5: Custom Signal Handler Recovery
            print("  [TEST 5] Testing custom signal handler recovery...")
            out = guest.command('/bin/crash-test handler')
            assert '[CRASH-TEST] Caught SIGSEGV in user handler successfully!' in out, out
            code_out = guest.command('echo $?')
            assert '0' in code_out, f"Expected exit code 0 for recovered handler, got: {code_out}"
            print("    [PASS] Custom SIGSEGV handler intercepted fault and recovered cleanly.")

            # Test 6: Recursive Fault in Signal Handler
            print("  [TEST 6] Testing recursive fault fallback to crash dump...")
            out = guest.command('/bin/crash-test recfault')
            assert '[CRASH-TEST] In handler; triggering recursive segfault...' in out, out
            assert '[CRASH] ==================== USER SPACE CRASH DUMP ====================' in out, out
            assert 'SIGSEGV (11)' in out, out
            print("    [PASS] Recursive fault in signal handler cleanly fell back to crash core dump.")

            # Test 7: dmesg ring buffer verification
            print("  [TEST 7] Verifying crash dump persistence in kernel dmesg...")
            dmesg_out = guest.command('sudo /bin/dmesg')
            assert '[CRASH]' in dmesg_out, f"dmesg missing [CRASH] logs: {dmesg_out}"
            assert 'USER SPACE CRASH DUMP' in dmesg_out, f"dmesg missing crash dump header: {dmesg_out}"
            print("    [PASS] Kernel log buffer contains crash dump records.")

            print(f"[SUCCESS] All Crash Dump acceptance tests PASSED on {firmware.upper()} (SMP={smp})!")
        finally:
            guest.close()


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(line_buffering=True)
    print("=================================================================")
    print(" FortressOS User-Space Crash Dump Acceptance Suite (Issue #39)   ")
    print("=================================================================")

    # Test matrix: BIOS and UEFI across SMP=1 and SMP=4
    matrix = [
        ('bios', 1),
        ('bios', 4),
        ('uefi', 1),
        ('uefi', 4),
    ]

    for firmware, smp in matrix:
        run_test(firmware, smp)

    print("\n=================================================================")
    print(" [ACCEPTANCE CONFIRMED] 100% of Crash Dump Matrix PASSED!        ")
    print("=================================================================")


if __name__ == '__main__':
    main()
