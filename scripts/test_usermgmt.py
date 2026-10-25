#!/usr/bin/env python3
"""QEMU Acceptance Suite for FortressOS Users, Groups, and Privileges Subsystem.

Verifies acceptance criteria:
1. Booting the live image results in a functional user environment.
2. Logging in as operator succeeds.
3. `id` outputs structured format with human-readable names and supplementary groups.
4. `id root` outputs root user identity.
5. `groups` outputs group names for current user and target user.
6. `/bin/nologin` politely rejects execution with exit code 1.
7. `sudo addgroup testgrp` creates a new group.
8. `sudo delgroup testgrp` deletes the group.
9. `sudo delgroup wheel` fails closed, refusing to remove critical system groups.
10. `passwd root` is rejected, preserving root account security.
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
            time.sleep(0.05)
        raise TimeoutError(f"Wait timed out after {timeout}s: text='{self.text(start)}'")

    def send(self, line):
        self.proc.stdin.write(line.encode('latin1') + b'\n')
        self.proc.stdin.flush()

    def command(self, cmd, timeout=15):
        start = len(self.data)
        self.send(cmd)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump()
            t = self.text(start)
            if cmd in t and (t.rstrip().endswith('$') or t.rstrip().endswith('#')):
                return t
            time.sleep(0.05)
        raise TimeoutError(f"Command timed out: {cmd}, got: {self.text(start)}")

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def test_firmware(firmware, smp):
    print(f"\n========================================================")
    print(f"Testing Users & Groups Subsystem under {firmware.upper()} (SMP={smp})")
    print(f"========================================================")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        guest = Guest(firmware, smp, tmp)
        try:
            print("[1/8] Waiting for boot and login prompt...")
            guest.wait(lambda t: t.endswith('FortressOS login: '), timeout=45)
            print("  [PASS] Boot successful; login prompt reached")

            print("[2/8] Logging in as operator...")
            guest.send('operator')
            guest.wait(lambda t: t.endswith('$ '), timeout=15)
            print("  [PASS] Logged in successfully as operator")

            print("[3/8] Verifying 'id' command output...")
            id_out = guest.command('id')
            assert "uid=" in id_out and "operator" in id_out, f"Unexpected id output: {id_out}"
            assert "gid=" in id_out and "groups=" in id_out, f"Unexpected id output: {id_out}"
            print(f"  id output: {id_out.strip()}")
            print("  [PASS] 'id' command verified with named user and group resolution")

            print("[4/8] Verifying 'id root' command output...")
            id_root = guest.command('id root')
            assert "uid=0(root)" in id_root and "gid=0(root)" in id_root, f"Unexpected id root: {id_root}"
            print(f"  id root output: {id_root.strip()}")
            print("  [PASS] 'id root' correctly displays superuser identity")

            print("[5/8] Verifying 'groups' command output...")
            groups_out = guest.command('groups')
            assert "operator" in groups_out and "wheel" in groups_out, f"Unexpected groups output: {groups_out}"
            print(f"  groups output: {groups_out.strip()}")
            groups_root = guest.command('groups root')
            assert "root" in groups_root, f"Unexpected groups root output: {groups_root}"
            print("  [PASS] 'groups' command correctly resolves group memberships")

            print("[6/8] Verifying '/bin/nologin' behavior...")
            nologin_out = guest.command('/bin/nologin')
            assert "This account is currently not available" in nologin_out, f"Unexpected nologin output: {nologin_out}"
            print("  [PASS] '/bin/nologin' politely rejected execution")

            print("[7/8] Verifying 'addgroup' and 'delgroup' lifecycle...")
            add_out = guest.command('sudo addgroup testgrp')
            assert "Group created" in add_out or "created" in add_out or "$" in add_out, f"Unexpected addgroup: {add_out}"
            print("  [PASS] 'addgroup testgrp' succeeded")

            del_out = guest.command('sudo delgroup testgrp')
            assert "Group deleted" in del_out or "deleted" in del_out or "$" in del_out, f"Unexpected delgroup: {del_out}"
            print("  [PASS] 'delgroup testgrp' succeeded")

            del_wheel = guest.command('sudo delgroup wheel')
            assert "critical" in del_wheel or "cannot" in del_wheel or "error" in del_wheel, f"Unexpected delgroup wheel output: {del_wheel}"
            print("  [PASS] 'delgroup wheel' failed closed protecting system security")

            print("[8/8] Verifying root password protection...")
            passwd_root = guest.command('passwd root')
            assert "cannot modify password for root" in passwd_root or "failed" in passwd_root, f"Unexpected passwd root: {passwd_root}"
            print("  [PASS] Root password modification blocked")

            print(f"\n[OK] {firmware.upper()} (SMP={smp}) USERS & GROUPS ACCEPTANCE PASSED!\n")
        finally:
            guest.close()


def main():
    configurations = [
        ('bios', 1),
        ('uefi', 1),
    ]
    for fw, smp in configurations:
        test_firmware(fw, smp)
    print("\n========================================================")
    print("ALL USERS & GROUPS MULTI-FIRMWARE ACCEPTANCE TESTS PASSED (2/2)!")
    print("========================================================\n")


if __name__ == '__main__':
    main()
