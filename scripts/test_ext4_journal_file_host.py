"""8.2 explicit disposable journal files, actual engine and Linux oracle."""
from test_jbd2_write_host import main
if __name__ == '__main__':
    main('ext4_journal_file_host',file_writes=True)
