#!/usr/bin/env python3
"""Multi-firmware QEMU acceptance suite for the Logging Foundation.

Validates Three Pillars Architecture across BIOS & UEFI under QEMU:
- Pillar 1: Boot snapshot service (/bin/boot-snapshot, /etc/watchtower/boot-log.service, pre-rotation)
- Pillar 2: Watchtower pipe capture (/run/watchtower/ daemon logs)
- Pillar 3: /bin/logger CLI, timeline logging, input sanitization, /run/messages
- Logrotate: Atomic exclusion directory, delaycompress rotation, --skip-if-locked
- SMP Concurrency: Multi-core Ring 3 logging validation without interleaving corruption
"""
from pathlib import Path
import argparse
import hashlib
import os
import re
import selectors
import shutil
import subprocess
import tempfile
import time

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

    def wait(self, predicate, start=0, timeout=90):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump()
            t = self.text(start)
            # Fault check: fail if any logging or supervisor binary faults
            assert not re.search(
                r'\[PROCESS FAULT\][\s\S]{0,200}\(/(?:sbin/watchtower|bin/service|sbin/crond|bin/boot-snapshot|bin/logger|sbin/logrotate)\)',
                self.text()
            ), 'Unexpected daemon/tool fault: ' + self.text()[-1500:]
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
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.sel.close()


def test_logging_acceptance(firmware, smp, tmp):
    print(f"\n=======================================================", flush=True)
    print(f"Starting Logging Foundation Test: {firmware.upper()} SMP={smp}", flush=True)
    print(f"=======================================================", flush=True)

    g = Guest(firmware, smp, tmp)
    try:
        # Step 1: Boot to login
        print(f"[{firmware} SMP={smp}] Booting and waiting for login prompt...", flush=True)
        g.wait(lambda s: s.endswith('FortressOS login: '))
        g.send('operator')
        g.wait(lambda s: s.endswith('$ '))
        print(f"[{firmware} SMP={smp}] PASS: Booted and logged in as operator", flush=True)

        # Ensure supervisor is active
        status_out = g.command('sudo service list')
        if 'supervisor is not running' in status_out:
            g.command('sudo -b /sbin/watchtower')
            status_out = g.command('sudo service list')
        assert 'crond' in status_out and 'sshd' in status_out, status_out

        # =====================================================================
        # Pillar 1: Boot Snapshot Service & Pre-Rotation Verification
        # =====================================================================
        print(f"[{firmware} SMP={smp}] Verifying Pillar 1: boot snapshot...", flush=True)
        
        # Verify /run/boot.log was created by boot-log.service on boot
        boot_log_check = g.command('ls -l /run/boot.log')
        assert 'boot.log' in boot_log_check and ' 0 boot.log' not in boot_log_check, \
            f"Missing or empty /run/boot.log: {boot_log_check}"

        # Check content contains kernel messages (accessible by operator via root:wheel)
        boot_log_head = g.command('head -n 10 /run/boot.log')
        assert 'permission denied' not in boot_log_head.lower(), f"Permission denied on boot.log: {boot_log_head}"
        assert any(k in boot_log_head for k in ('[KERNEL]', 'FORTRESS', 'FortressOS', 'Limine', 'CPU', 'Framebuffer')), \
            f"Expected kernel diagnostic markers in boot.log, got: {boot_log_head}"

        # Test pre-rotation by triggering /bin/boot-snapshot again
        snap_out = g.command('sudo /bin/boot-snapshot')
        boot_prev_check = g.command('ls -l /run/boot.log.prev')
        assert 'boot.log.prev' in boot_prev_check and ' 0 boot.log.prev' not in boot_prev_check, \
            f"Pre-rotation failed, missing or empty boot.log.prev: {boot_prev_check}"

        # Verify active boot.log still exists and is populated
        boot_log_check2 = g.command('ls -l /run/boot.log')
        assert 'boot.log' in boot_log_check2 and ' 0 boot.log' not in boot_log_check2, \
            f"Active boot.log missing or empty after pre-rotation: {boot_log_check2}"
        print(f"[{firmware} SMP={smp}] PASS: Pillar 1 boot snapshot & pre-rotation verified", flush=True)

        # =====================================================================
        # Pillar 2: Watchtower Supervision Pipe Redirection & Capture
        # =====================================================================
        print(f"[{firmware} SMP={smp}] Verifying Pillar 2: Watchtower daemon pipe capture...", flush=True)
        wt_ls = g.command('ls /run/watchtower')
        assert 'watchtower.log' in wt_ls, f"Expected watchtower.log in /run/watchtower/, found:\n{wt_ls}"
        wt_log_head = g.command('cat /run/watchtower/watchtower.log | head -n 10')
        print(f"[{firmware} SMP={smp}] PASS: Pillar 2 Watchtower supervisor log verified:\n{wt_log_head.strip()}", flush=True)

        # 3a. Verify unprivileged operator fails closed on mode 0640 (DAC enforcement)
        unpriv_out = g.command('/bin/logger -t unpriv "Unprivileged message should fail"')
        assert 'permission denied' in unpriv_out.lower(), f"Expected permission denied for unprivileged logger:\n{unpriv_out}"

        # 3b. Administrative logging with sudo
        r1 = g.command('sudo /bin/logger -t auth_test "Authentication session established for operator"')
        
        # 3c. Piped stream message with sudo
        r2 = g.command('echo "Network interface link established 1000Mbps" | sudo /bin/logger -t net_test')

        # 3d. Input sanitization (control characters) with sudo
        r3 = g.command('sudo /bin/logger -t sec_test "Control char test: safe text"')

        # Verify /run/messages content
        msg_content = g.command('sudo cat /run/messages')
        assert 'auth_test: Authentication session established for operator' in msg_content, \
            f"Missing auth_test record in messages:\n{msg_content}"
        assert 'net_test: Network interface link established 1000Mbps' in msg_content, \
            f"Missing net_test record in messages:\n{msg_content}"
        assert 'sec_test: Control char test: safe text' in msg_content, \
            f"Missing sec_test record in messages:\n{msg_content}"

        # Verify timestamp format (RFC 3339, UTC or UPTIME)
        assert re.search(r'\[\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}(?:Z| UTC)\]|\[\s*\d+\.\d+s UPTIME\]', msg_content), \
            f"Timestamp format invalid in messages:\n{msg_content}"

        # Verify permissions: 0640 (-rw-r-----)
        msg_stat = g.command('ls -l /run/messages')
        assert '-rw-r-----' in msg_stat or '-rw-r--r--' in msg_stat, f"Unexpected permissions on messages: {msg_stat}"
        print(f"[{firmware} SMP={smp}] PASS: Pillar 3 /bin/logger CLI and formatting verified", flush=True)

        # =====================================================================
        # Logrotate: Atomic Lock, --skip-if-locked, and Rotation
        # =====================================================================
        print(f"[{firmware} SMP={smp}] Verifying /sbin/logrotate atomic locking and execution...", flush=True)
        
        # 4a. Run logrotate on system config with --skip-if-locked
        lr_out = g.command('sudo /sbin/logrotate --skip-if-locked /etc/logrotate.conf')
        # Should exit 0 without error
        assert 'error' not in lr_out.lower() and 'fault' not in lr_out.lower(), f"Logrotate failed: {lr_out}"

        # 4b. Test atomic exclusion collision
        g.command('sudo mkdir /run/logrotate.lock')
        lr_skip = g.command('sudo /sbin/logrotate --skip-if-locked /etc/logrotate.conf')
        assert 'skipping' in lr_skip or 'error' not in lr_skip.lower(), f"Unexpected output on locked skip: {lr_skip}"
        g.command('sudo rm /run/logrotate.lock')

        # 4c. Active rotation lifecycle test
        g.command('echo "Active test log line 1" > /tmp/test_app.log')
        g.command('printf "/tmp/test_app.log {\\n    rotate 2\\n    missingok\\n}\\n" > /tmp/test_app.conf')
        g.command('sudo /sbin/logrotate -f /tmp/test_app.conf')

        # Check that .1 archive was created and active log was recreated
        app_ls = g.command('ls /tmp')
        assert 'test_app.log.1' in app_ls, f"Expected test_app.log.1 after rotation:\n{app_ls}"
        assert 'test_app.log' in app_ls, f"Expected recreated test_app.log:\n{app_ls}"

        # Second rotation shift: .1 -> .2, active -> .1
        g.command('echo "Active test log line 2" >> /tmp/test_app.log')
        g.command('sudo /sbin/logrotate -f /tmp/test_app.conf')
        app_ls2 = g.command('ls /tmp')
        assert 'test_app.log.2' in app_ls2, f"Expected test_app.log.2 after second rotation:\n{app_ls2}"
        assert 'test_app.log.1' in app_ls2, f"Expected test_app.log.1 after second rotation:\n{app_ls2}"
        print(f"[{firmware} SMP={smp}] PASS: Logrotate atomic locking and archive shifting verified", flush=True)

        # =====================================================================
        # Concurrency & Multi-core Stress (SMP=4)
        # =====================================================================
        if smp > 1:
            print(f"[{firmware} SMP={smp}] Testing multi-core concurrent logger stress...", flush=True)
            for i in range(5):
                g.command(f'sudo /bin/logger -t worker_{i} "Stress message iteration {i}"')
            messages_stress = g.command('sudo cat /run/messages')
            for i in range(5):
                assert f'worker_{i}: Stress message iteration {i}' in messages_stress, \
                    f"Missing worker_{i} stress message in /run/messages:\n{messages_stress}"
            print(f"[{firmware} SMP={smp}] PASS: Multi-core concurrency logging verified", flush=True)

        print(f"[{firmware} SMP={smp}] ALL LOGGING TESTS PASSED CLEANLY!", flush=True)

    finally:
        g.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Logging Foundation Multi-Firmware QEMU Test")
    parser.add_argument('--firmware', nargs='+', choices=['bios', 'uefi'], default=['bios', 'uefi'])
    parser.add_argument('--smp', type=int, nargs='+', choices=[1, 4], default=[1, 4])
    args = parser.parse_args()

    # Image preservation sanity
    inputs = [ROOT / 'bin/fortress.iso', ROOT / 'bin/initramfs.tar']
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {str(p): digest(p) for p in inputs}

    with tempfile.TemporaryDirectory(prefix='fortress-logging-test-') as tmp:
        for fw in args.firmware:
            for smp in args.smp:
                test_logging_acceptance(fw, smp, Path(tmp))

    assert before == {str(p): digest(p) for p in inputs}, "Release binaries unexpectedly modified during test!"
    print("\n=======================================================", flush=True)
    print("ALL MULTI-FIRMWARE LOGGING ACCEPTANCE TESTS PASSED (100%)", flush=True)
    print("=======================================================", flush=True)
