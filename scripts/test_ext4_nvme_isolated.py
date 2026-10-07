"""Existing NVMe integration regression using an explicit isolated boot snapshot."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import test_ext4_journal_mount as guest
import test_ext4_integration as integration
from test_nmi_transitions import REPO


def main():
    parser=argparse.ArgumentParser();parser.add_argument('workspace',type=Path);args=parser.parse_args()
    root=REPO/'.codex-remote-attachments/ext4-phase9';workspace=args.workspace.resolve();assert workspace.is_relative_to(root.resolve())
    out=Path(tempfile.mkdtemp(prefix='guest-nvme-regression-',dir=root));iso=out/'fixture.iso';shutil.copyfile(workspace/'bin/fortress.iso',iso)
    records=[];errors=[];original=(guest.command,guest.boot,guest.audit)
    integration.SMP=4;guest.command=integration.command;guest.boot=integration.boot;guest.audit=integration.audit
    try:
        for mode in ('bios','uefi'):
            for bs in (1024,2048,4096):
                source=REPO/f'.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu/{bs}-{"wrap" if bs==2048 else "normal"}-512-pending-seed.img'
                records.append(guest.case(mode,bs,source,out,iso))
    except Exception as error:errors.append(repr(error));raise
    finally:
        guest.command,guest.boot,guest.audit=original
        (out/'manifest.json').write_text(json.dumps({'cases':records,'errors':errors,'smp':4,'workspace':str(workspace),
            'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest()},indent=2)+'\n');print(f'Evidence: {out}',flush=True)


if __name__=='__main__':main()
