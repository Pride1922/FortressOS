#!/usr/bin/env python3
"""Check the actual freestanding gterm entry frame, before its first call.
Host terminal tests have much larger stacks and cannot detect this boot crash.
"""
import argparse
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf')
    args = parser.parse_args()
    text = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn',
                                    '--disassemble=rust_main', args.elf], text=True)
    assert '<rust_main>:' in text, 'missing rust_main entry'
    frame = 0
    called = False
    for line in text.split('<rust_main>:', 1)[1].splitlines():
        match = re.match(r'\s*[0-9a-f]+:\s+(\S+)\s*(.*)', line)
        if not match:
            continue
        op, operands = match.groups()
        operands = operands.replace(' ', '')
        if op in ('call', 'callq'):
            called = True
            break
        if op in ('push', 'pushq'):
            frame += 8
        elif op == 'sub' and operands.endswith(',%rsp'):
            amount = re.fullmatch(r'\$(0x[0-9a-f]+|[0-9]+),%rsp', operands)
            assert amount, 'unbounded entry stack adjustment'
            frame += int(amount[1], 0)
        elif operands.endswith(',%rsp'):
            raise AssertionError('unsupported entry stack adjustment: ' + line)
    assert called, 'entry has no bounded initial call'
    assert frame <= 1024, f'gterm entry frame {frame} bytes exceeds 1024-byte budget'
    print(f'PASS gterm compiled entry frame: {frame} bytes (budget 1024)')


if __name__ == '__main__':
    main()
