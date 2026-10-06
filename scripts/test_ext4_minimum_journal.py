"""Actual minimum-size e2fsprogs journals; separate from synthetic ENOSPC tests."""
from create_ext4_fixtures import ROOT
from test_jbd2_write_host import main


if __name__=='__main__':
    main(binary=str(ROOT/'.codex-remote-attachments/ext4-phase9/bin/jbd2_write_minimum_host'),
         minimum_journal=True,evidence_parent='.codex-remote-attachments/ext4-phase9')
