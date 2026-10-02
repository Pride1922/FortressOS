"""Independent capture/audit evidence must fail when required inputs disappear."""
import json
import shutil
import sys
import tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from net_dns_wire_audit import audit

vector=json.loads((ROOT/'tests/fixtures/net_dns_wire_vectors.json').read_text())
query=bytes.fromhex(vector['query']); answer=bytes.fromhex(vector['a_response'])
assert len(query)==30 and len(answer)==46
assert bytes.fromhex(vector['tcp_a_response'])==b'\x00\x2e'+answer
assert answer[:2]==query[:2] and answer[12:30]==query[12:] and answer[30:32]==b'\xc0\x0c'

if len(sys.argv)>1:
    source=Path(sys.argv[1]); assert audit(source)
    with tempfile.TemporaryDirectory(prefix='dns-audit-reject-') as name:
        target=Path(name)
        for filename in ('dns-cases.json','serial.log','scenario.json','injection.jsonl','wire.pcap'):
            shutil.copyfile(source/filename,target/filename)
        for corruption in ('missing','malformed','counterfeit-output'):
            if corruption=='missing': (target/'injection.jsonl').unlink()
            elif corruption=='malformed': (target/'injection.jsonl').write_text('invalid JSON\n')
            else:
                shutil.copyfile(source/'injection.jsonl',target/'injection.jsonl')
                rows=json.loads((target/'dns-cases.json').read_text()); rows[0]['expected']='counterfeit DNS result'
                (target/'dns-cases.json').write_text(json.dumps(rows))
            try: audit(target)
            except (AssertionError, OSError, ValueError, KeyError): pass
            else: raise AssertionError('incomplete/counterfeit audit passed: '+corruption)
print('DNS golden framing and optional missing/malformed/counterfeit audit rejection PASS')
