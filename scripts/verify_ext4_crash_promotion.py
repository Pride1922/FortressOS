"""Prove every fragmented growth baseline actually promotes the inode root."""
import argparse
import gzip
import json
import re
from pathlib import Path
from test_ext4_crash_inventory import command, ROOT


def main():
    parser=argparse.ArgumentParser();parser.add_argument('inventory',type=Path);args=parser.parse_args()
    root=args.inventory.resolve()
    assert root.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    manifest=json.loads((root/'manifest.json').read_text())
    assert not manifest['errors'];records=[];work=root/'promotion-check.img'
    for case in manifest['cases']:
        if case['operation']!='write':continue
        label=f'{case["block"]}-{case["placement"]}-{case["sector"]}-write'
        work.write_bytes(gzip.decompress((root/f'{label}-clean.img.gz').read_bytes()))
        text=command(['debugfs','-R','stat /target.bin',str(work)],root/f'{label}.promotion.log')
        assert '(ETB0)' in text and re.findall(r'\((\d+)\):\d+',text)==['0','1','2','3','4'],text
        records.append(label)
    work.unlink();assert len(records)==12
    (root/'promotion-verification.json').write_text(json.dumps(records,indent=2)+'\n')
    print('Fragmented root promotion PASS 12/12: five extents and an external leaf')


if __name__=='__main__':main()
