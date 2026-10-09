#!/usr/bin/env python3
"""UEFI USB boot smoke on a disposable regular-file copy; no physical claim."""
import argparse
import hashlib
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from test_usb_persistence import run_qemu_session, send_command, check_offline_ext2
from test_smpbench_qemu import validate_profile, validate_barrier
import test_usb_persistence as usb_fixture

# Extend typing locally for redirection; preserve the shared fixture behavior.
base_type = usb_fixture.qmp_type_string
def type_capture_command(qmp, text, delay=0.03):
    for char in text:
        if char not in (">", "_"):
            base_type(qmp, char, delay)
            continue
        code = "dot" if char == ">" else "minus"
        qmp.execute("input-send-event", {"events": [
            {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": key}}}
            for key, down in (("shift", True), (code, True), (code, False), ("shift", False))]})
        time.sleep(delay)
usb_fixture.qmp_type_string = type_capture_command

REPO = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--image", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--wait-profile", action="store_true", help="Test scheduler-wait-only capture instead of phases.")
args = parser.parse_args()
capture_flag = '--wait-profile' if args.wait_profile else '-p'
validate_capture = validate_barrier if args.wait_profile else validate_profile
capture_reps = 9 if args.wait_profile else 5  # Wait-only records are shorter.
out = args.output.resolve()
assert out.is_relative_to(REPO / "build") and out != REPO / "build"
assert args.image.is_file()
out.mkdir(parents=True, exist_ok=False)
with args.image.open("rb") as stream:
    original = hashlib.file_digest(stream, "sha256").hexdigest()
image = out / "disposable-usb.img"
shutil.copyfile(args.image, image)


def action(qmp, child, log):
    for workload in ("signals", "pipes"):
        reply = send_command(qmp, child, log,
            f"smpbench -w {workload} -n 1 -r 1 -i 3 -c {capture_flag}\n", "ok=1 short=0", timeout=60)
        reply = re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", reply)
        validate_capture(reply, 1, 1)
        if args.wait_profile: assert reply.count('wait_diag=1') == 2
    # More than 4096 bytes: exercise the exact capture path omitted initially.
    send_command(qmp, child, log, f"smpbench -w pipes -n 1 -r {capture_reps} -i 3 -c {capture_flag} > /mnt/capture.txt\n")
    reply = send_command(qmp, child, log, "cat /mnt/capture.txt\n", "ok=1 short=0", timeout=60)
    reply = re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", reply)
    validate_capture(reply, 1, capture_reps)
    if args.wait_profile: assert reply.count('wait_diag=1') == capture_reps + 1
    assert "write error" not in reply
    send_command(qmp, child, log, "sync\n")
    send_command(qmp, child, log, "shutdown\n")


run_qemu_session("uefi", image, out / "uefi-usb.log", action, "dell-smpbench-smoke")
check_offline_ext2(image)
with tempfile.TemporaryDirectory(prefix="fortress-capture-audit-") as temporary:
    partition = Path(temporary) / "data.ext2"
    with image.open("rb") as stream:
        stream.seek(133120 * 512)
        partition.write_bytes(stream.read(131072 * 512))
    saved = subprocess.run(["debugfs", "-R", "cat /capture.txt", str(partition)],
                           capture_output=True, check=True, timeout=30).stdout
    assert len(saved) > 4096, "Capture did not exercise the RAM file limit"
    validate_capture(saved.decode(), 1, capture_reps)
    if args.wait_profile: assert saved.count(b'wait_diag=1') == capture_reps + 1
    (out / "capture.txt").write_bytes(saved)
with args.image.open("rb") as stream:
    assert hashlib.file_digest(stream, "sha256").hexdigest() == original, "Frozen image changed"
(out / "result.txt").write_text(f"UEFI TCG USB boot, selected RW mount, n=1 {'scheduler-wait-only' if args.wait_profile else 'phase-profiled'} signals/pipes, >4096-byte /mnt capture independently read/validated with debugfs, clean shutdown and offline ext2 audit PASS. Frozen source image unchanged. No physical or SMP=8 USB claim.\n")
print((out / "result.txt").read_text(), end="")
