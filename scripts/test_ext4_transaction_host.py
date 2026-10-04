"""8.1 uses explicit Phase-7 disposable CSUM_V3 fixtures, never raw media."""
from test_jbd2_write_host import main
if __name__ == '__main__':
    main('ext4_transaction_host',foundation=True)
