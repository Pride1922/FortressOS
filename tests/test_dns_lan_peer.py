"""Literal wire checks for the manual LAN fixture; no physical claim."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from dns_lan_server import answer,question

header=bytes.fromhex('123401000001000000000000')
suffix=bytes.fromhex('0000010001')
names={
    'service.test':bytes.fromhex('07736572766963650474657374'),
    'alias.test':bytes.fromhex('05616c6961730474657374'),
    'missing.test':bytes.fromhex('076d697373696e670474657374'),
    'tcp.test':bytes.fromhex('037463700474657374'),
    'stall.test':bytes.fromhex('057374616c6c0474657374'),
}
ip=bytes([192,168,0,153])
for name,wire in names.items():
    raw=header+wire+suffix
    assert question(raw)==(0x1234,name,raw[12:])
    response=answer(raw,ip)
    assert response[:2]==raw[:2] and response[12:len(raw)]==raw[12:]
    if name=='missing.test': assert response[3]&15==3
    elif name in ('tcp.test','stall.test'):
        assert response[2]&2 and len(response)==len(raw)
        assert answer(raw,ip,tcp=True)[-4:]==ip
    else:
        assert response[-4:]==ip
        if name=='alias.test': assert response[6:8]==b'\0\2'
for raw in (header,header+bytes([0xc0,12])+suffix,header+bytes([64])+b'a'*64+suffix):
    try: question(raw)
    except ValueError: pass
    else: raise AssertionError('invalid query admitted')
print('LAN DNS fixture literal names / UDP TC / TCP A / alias / NXDOMAIN / rejected bounds PASS (no hardware claim)')
