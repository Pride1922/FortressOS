#!/usr/bin/env python3
"""Phase 9G.4 — USB Persistent Storage 3-Boot Acceptance Suite.

Covers:
1. Multi-firmware testing: legacy BIOS and UEFI with paired OVMF 4M firmware.
2. Emulated USB flash drive boot (qemu-xhci + usb-storage, bootindex=1).
3. Preflight QEMU argv verification: strict exclusion of NVMe and unauthorized drives.
4. Verification of read-write production mount of sdap2 at /mnt.
5. 3-Boot Lifecycle:
   - Boot 1: Create file /mnt/persist.txt, verify cat, clean shutdown, offline e2fsck -fn (0 errors).
   - Boot 2: Reboot, verify contents, overwrite with shorter content, create /mnt/second.txt, clean shutdown, offline e2fsck -fn (0 errors).
   - Boot 3: Reboot third time, verify exact persistence of modified and second file, remove second file, clean shutdown, offline e2fsck -fn (0 errors).
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent
CODE = Path('/usr/share/OVMF/OVMF_CODE_4M.fd')
VARS = Path('/usr/share/OVMF/OVMF_VARS_4M.fd')

import sys
sys.path.insert(0, str(REPO / "scripts"))
from test_nmi_transitions import QMP


def qmp_type_string(qmp, text, delay=0.03):
    for char in text:
        is_upper = char.isupper()
        base = char.lower()
        code = {' ': 'spc', '\n': 'ret', '/': 'slash', '.': 'dot', '_': 'minus', '-': 'minus'}.get(base, base)
        events = []
        if is_upper:
            events.append({'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': 'shift'}}})
        events.append({'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': code}}})
        events.append({'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': code}}})
        if is_upper:
            events.append({'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': 'shift'}}})
        qmp.execute('input-send-event', {'events': events})
        time.sleep(delay)


def send_command(qmp, child, log_path, cmd, expect=None, timeout=15):
    start = len(log_path.read_text(errors='replace')) if log_path.exists() else 0
    qmp_type_string(qmp, cmd)
    if expect:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            reply = log_path.read_text(errors='replace')[start:].replace('\r', '')
            if expect in reply:
                return reply
            assert child.poll() is None, f"Child exited prematurely with code {child.poll()}"
            time.sleep(0.05)
        raise AssertionError(f"Timeout waiting for {expect!r} after sending {cmd!r}. Output:\n{log_path.read_text(errors='replace')[-1000:]}")
    return ""


def check_offline_ext2(img_path: Path) -> None:
    """Extracts partition 2 slice (64 MiB starting at sector 133120) and runs e2fsck -fn."""
    ext2_offset = 133120 * 512
    ext2_size = 131072 * 512
    with tempfile.NamedTemporaryFile(prefix="fortress-e2fsck-", suffix=".ext2") as tmp:
        with open(img_path, "rb") as f:
            f.seek(ext2_offset)
            data = f.read(ext2_size)
            assert len(data) == ext2_size, "Failed to read full ext2 partition"
            tmp.write(data)
            tmp.flush()

        res = subprocess.run(["e2fsck", "-fn", tmp.name], capture_output=True, text=True)
        assert res.returncode == 0, f"Offline e2fsck integrity check failed:\n{res.stdout}\n{res.stderr}"


def configure_disposable_img_rw(img_path: Path) -> None:
    """Configures limine.conf on the disposable image to boot in persistent writable mode."""
    fat_offset = 2048 * 512
    res = subprocess.run(["mtype", "-i", f"{img_path}@@{fat_offset}", "::limine.conf"],
                         capture_output=True, text=True, check=True)
    conf = res.stdout.replace("usb_data_mode=ro", "usb_data_mode=rw")
    with tempfile.NamedTemporaryFile("w", suffix=".conf") as tmp_conf:
        tmp_conf.write(conf)
        tmp_conf.flush()
        for dst in ["::limine.conf", "::boot/limine.conf", "::boot/limine/limine.conf", "::EFI/BOOT/limine.conf"]:
            subprocess.run(["mcopy", "-o", "-i", f"{img_path}@@{fat_offset}", tmp_conf.name, dst],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def run_qemu_session(firmware: str, img_path: Path, log_path: Path,
                     action_cb, round_name: str, writable: bool = True) -> None:
    """Runs a single QEMU session with strict drive assertions and executes action_cb(qmp, child, log_path)."""
    with tempfile.TemporaryDirectory(prefix=f"fortress-persist-{round_name}-") as tmp:
        cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '2G', '-accel', 'tcg',
               '-smp', '1', '-display', 'none', '-monitor', 'none', '-no-reboot',
               '-serial', f'file:{log_path}']
        firmware_drives = []
        if firmware == 'uefi':
            assert CODE.is_file() and VARS.is_file(), 'Paired OVMF 4M firmware required'
            variables = Path(tmp) / 'vars.fd'
            shutil.copyfile(VARS, variables)
            firmware_drives = [
                f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
                f'if=pflash,format=raw,unit=1,file={variables}',
            ]
            for drive in firmware_drives:
                cmd += ['-drive', drive]

        cmd += ['-device', 'qemu-xhci,id=xhci,p2=4,p3=0',
                '-device', 'usb-storage,drive=usbdrive,bootindex=1',
                '-drive', f'if=none,id=usbdrive,format=raw,file={img_path}',
                '-qmp', f'unix:{tmp}/qmp,server=on,wait=off']

        # Preflight assertions: only firmware drives and the disposable USB image are permitted
        permitted_drives = list(firmware_drives) + [f'if=none,id=usbdrive,format=raw,file={img_path}']
        assert [cmd[i + 1] for i, arg in enumerate(cmd) if arg == '-drive'] == permitted_drives
        assert '-blockdev' not in cmd and '-hda' not in cmd and '-hdb' not in cmd
        assert 'nvme' not in ' '.join(cmd)

        expected_devices = ['qemu-xhci,id=xhci,p2=4,p3=0', 'usb-storage,drive=usbdrive,bootindex=1']
        assert [cmd[i + 1] for i, arg in enumerate(cmd) if arg == '-device'] == expected_devices

        log_path.write_text('')
        stderr_path = log_path.with_suffix('.stderr')

        with stderr_path.open('w') as err:
            child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=err)
            qmp = None
            try:
                qmp = QMP(Path(tmp) / 'qmp')
                # Wait for interactive shell prompt
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    assert child.poll() is None, f"QEMU crashed during startup: {stderr_path.read_text()}"
                    time.sleep(0.5)
                    if log_path.exists():
                        output = log_path.read_text(errors='replace')
                        if 'FortressOS shell (Ring 3)' in output and 'fortress> ' in output:
                            break
                else:
                    raise AssertionError(f'{round_name}: shell prompt not reached within timeout')

                # Verify the actual mount mode, including the shipped RO default.
                output = log_path.read_text(errors='replace')
                mode = 'read-write' if writable else 'read-only'
                stage = '9G.4' if writable else '9G.3'
                assert f'[USB {stage}] PASS: Mounted sdap2 {mode} at /mnt' in output, f"{mode} mount failed in {round_name}"
                if writable:
                    assert '[USB 9G.4] Flush preflight passed' in output, f"Flush preflight missing in {round_name}"

                # Execute round actions
                action_cb(qmp, child, log_path)

                # Wait for clean shutdown
                exit_code = child.wait(timeout=20)
                assert exit_code == 0, f"QEMU exited with non-zero code {exit_code}"
            finally:
                if qmp and getattr(qmp, 'sock', None):
                    try:
                        qmp.stream.close()
                        qmp.sock.close()
                    except Exception:
                        pass
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()


def test_read_only_default(firmware: str):
    """Reproduce the reported editor failure without changing the boot default."""
    with tempfile.TemporaryDirectory(prefix='fortress-usb-ro-') as tmp:
        img = Path(tmp) / 'usb.img'
        shutil.copyfile(REPO / 'bin' / 'fortress.img', img)
        log = REPO / 'build' / f'usb-editor-ro-{firmware}.log'

        def action(qmp, child, log_path):
            send_command(qmp, child, log_path, 'edit /mnt/dell.txt\n', 'edit> ')
            send_command(qmp, child, log_path, 'a\n', '> ')
            send_command(qmp, child, log_path, 'test\n', '> ')
            send_command(qmp, child, log_path, '.\n', 'edit> ')
            reply = send_command(qmp, child, log_path, 'w\n', 'edit> ')
            assert 'Read-only filesystem.' in reply
            assert 'boot the Writable USB entry' in reply
            assert 'Buffer preserved in memory only' in reply
            reply = send_command(qmp, child, log_path, 'p\n', 'edit> ')
            assert 'test' in reply
            send_command(qmp, child, log_path, 'q\n', 'fortress> ')
            send_command(qmp, child, log_path, 'cat /mnt/dell.txt\n', 'No such file')
            send_command(qmp, child, log_path, 'shutdown\n')

        run_qemu_session(firmware, img, log, action, f'{firmware}-ro', writable=False)
        check_offline_ext2(img)
        print(f'[PASS] {firmware}: RO default rejects save with guidance and preserves editor buffer.', flush=True)


def test_firmware_persistence(firmware: str):
    print(f"\n==================================================================")
    print(f"--> Testing 3-Boot USB Writable Persistence under {firmware.upper()}...")
    print(f"==================================================================")

    src_img = REPO / 'bin' / 'fortress.img'
    assert src_img.is_file(), "bin/fortress.img must exist before running persistence test"

    with tempfile.TemporaryDirectory(prefix=f"fortress-persist-{firmware}-") as test_dir:
        disposable_img = Path(test_dir) / f"fortress_usb_{firmware}.img"
        shutil.copyfile(src_img, disposable_img)

        # Enable writable persistence in limine.conf on disposable image
        configure_disposable_img_rw(disposable_img)

        # Pre-boot offline audit
        check_offline_ext2(disposable_img)

        # =====================================================================
        # BOOT 1: Create file /mnt/persist.txt
        # =====================================================================
        print(f"  [BOOT 1] Booting and creating /mnt/persist.txt via editor...")
        log_b1 = REPO / 'build' / f'usb-persist-{firmware}-boot1.log'

        def action_boot1(qmp, child, log_path):
            send_command(qmp, child, log_path, 'ls /mnt\n', 'README.txt')
            send_command(qmp, child, log_path, 'edit /mnt/persist.txt\n', 'edit> ')
            send_command(qmp, child, log_path, 'a\n', '> ')
            send_command(qmp, child, log_path, 'Phase 9G.4 persistence round 1 line 1\n', '> ')
            send_command(qmp, child, log_path, 'Phase 9G.4 persistence round 1 line 2\n', '> ')
            send_command(qmp, child, log_path, '.\n', 'edit> ')
            send_command(qmp, child, log_path, 'w\n', 'Saved')
            send_command(qmp, child, log_path, 'q\n', 'fortress> ')
            send_command(qmp, child, log_path, 'cat /mnt/persist.txt\n', 'Phase 9G.4 persistence round 1 line 2')
            send_command(qmp, child, log_path, 'ls /mnt\n', 'persist.txt')
            send_command(qmp, child, log_path, 'shutdown\n', None)

        run_qemu_session(firmware, disposable_img, log_b1, action_boot1, f"{firmware}-boot1")
        print(f"  [PASS] Boot 1 clean shutdown complete.")

        # Offline audit 1
        print(f"  [AUDIT 1] Verifying offline ext2 filesystem integrity (e2fsck -fn)...")
        check_offline_ext2(disposable_img)
        print(f"  [PASS] Audit 1: 0 errors reported by e2fsck.")

        # =====================================================================
        # BOOT 2: Verify persistence, overwrite file, create second file
        # =====================================================================
        print(f"  [BOOT 2] Rebooting, verifying /mnt/persist.txt, overwriting, creating /mnt/second.txt...")
        log_b2 = REPO / 'build' / f'usb-persist-{firmware}-boot2.log'

        def action_boot2(qmp, child, log_path):
            send_command(qmp, child, log_path, 'ls /mnt\n', 'persist.txt')
            send_command(qmp, child, log_path, 'cat /mnt/persist.txt\n', 'Phase 9G.4 persistence round 1 line 2')
            send_command(qmp, child, log_path, 'edit /mnt/persist.txt\n', 'edit> ')
            send_command(qmp, child, log_path, 'd 1\n', 'Deleted line 1')
            send_command(qmp, child, log_path, 'd 1\n', 'Deleted line 1')
            send_command(qmp, child, log_path, 'a\n', '> ')
            send_command(qmp, child, log_path, 'Overwritten truncated content in round 2\n', '> ')
            send_command(qmp, child, log_path, '.\n', 'edit> ')
            send_command(qmp, child, log_path, 'w\n', 'Saved')
            send_command(qmp, child, log_path, 'q\n', 'fortress> ')
            send_command(qmp, child, log_path, 'cat /mnt/persist.txt\n', 'Overwritten truncated content in round 2')

            send_command(qmp, child, log_path, 'edit /mnt/second.txt\n', 'edit> ')
            send_command(qmp, child, log_path, 'a\n', '> ')
            send_command(qmp, child, log_path, 'Second persistent file\n', '> ')
            send_command(qmp, child, log_path, '.\n', 'edit> ')
            send_command(qmp, child, log_path, 'w\n', 'Saved')
            send_command(qmp, child, log_path, 'q\n', 'fortress> ')
            send_command(qmp, child, log_path, 'ls /mnt\n', 'second.txt')
            send_command(qmp, child, log_path, 'shutdown\n', None)

        run_qemu_session(firmware, disposable_img, log_b2, action_boot2, f"{firmware}-boot2")
        print(f"  [PASS] Boot 2 clean shutdown complete.")

        # Offline audit 2
        print(f"  [AUDIT 2] Verifying offline ext2 filesystem integrity (e2fsck -fn)...")
        check_offline_ext2(disposable_img)
        print(f"  [PASS] Audit 2: 0 errors reported by e2fsck.")

        # =====================================================================
        # BOOT 3: Verify modifications, unlink second file
        # =====================================================================
        print(f"  [BOOT 3] Rebooting, verifying modified content and /mnt/second.txt, removing second file...")
        log_b3 = REPO / 'build' / f'usb-persist-{firmware}-boot3.log'

        def action_boot3(qmp, child, log_path):
            send_command(qmp, child, log_path, 'cat /mnt/persist.txt\n', 'Overwritten truncated content in round 2')
            send_command(qmp, child, log_path, 'cat /mnt/second.txt\n', 'Second persistent file')
            send_command(qmp, child, log_path, 'rm /mnt/second.txt\n', 'fortress> ')
            send_command(qmp, child, log_path, 'cat /mnt/second.txt\n', 'No such file')
            send_command(qmp, child, log_path, 'shutdown\n', None)

        run_qemu_session(firmware, disposable_img, log_b3, action_boot3, f"{firmware}-boot3")
        print(f"  [PASS] Boot 3 clean shutdown complete.")

        # Offline audit 3
        print(f"  [AUDIT 3] Verifying offline ext2 filesystem integrity (e2fsck -fn)...")
        check_offline_ext2(disposable_img)
        print(f"  [PASS] Audit 3: 0 errors reported by e2fsck.")

        print(f"[ALL PASS] 3-boot persistence suite passed successfully under {firmware.upper()}!\n")


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--firmware', choices=['bios', 'uefi', 'both'], default='both')
    args = parser.parse_args()

    firmwares = ['bios', 'uefi'] if args.firmware == 'both' else [args.firmware]
    for fw in firmwares:
        test_read_only_default(fw)
        test_firmware_persistence(fw)
