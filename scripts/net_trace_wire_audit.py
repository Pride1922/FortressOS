"""Independent capture audit. Imports no peer parser, checksum or encoder."""
import collections
import json
from pathlib import Path


def records(path):
    raw=Path(path).read_bytes()
    assert 24<=len(raw)<=32*1024*1024
    assert raw[:4] in (bytes.fromhex("d4c3b2a1"),bytes.fromhex("a1b2c3d4"))
    endian="little" if raw[0]==0xd4 else "big"
    assert int.from_bytes(raw[4:6],endian)==2 and int.from_bytes(raw[6:8],endian)==4
    assert int.from_bytes(raw[20:24],endian)==1
    offset=24
    while offset<len(raw):
        assert offset+16<=len(raw)
        size=int.from_bytes(raw[offset+8:offset+12],endian)
        original=int.from_bytes(raw[offset+12:offset+16],endian)
        offset+=16
        assert size==original and 0<size<=1514 and offset+size<=len(raw)
        yield raw[offset:offset+size]
        offset+=size


def valid_sum(raw):
    acc=0
    for index in range(0,len(raw),2):
        acc+=int.from_bytes(raw[index:index+2].ljust(2,b"\0"),"big")
    while acc>65535:
        high,low=divmod(acc,65536)
        acc=high+low
    return acc==65535


def audit(pcap, injection_path, scenarios):
    # Incomplete inputs are fatal, never a pcap-only fallback.
    injection_path=Path(injection_path)
    assert injection_path.is_file() and injection_path.stat().st_size<=8*1024*1024
    entries=[json.loads(line) for line in injection_path.read_text().splitlines()]
    assert entries
    pending=collections.Counter(bytes.fromhex(row["hex"]) for row in entries)
    trace=[]
    all_echo=[]
    identities=set()
    for raw in records(pcap):
        outbound=raw[6:12]==bytes.fromhex("525400123456")
        if not outbound:
            assert pending[raw]>0,"unlogged injection"
            pending[raw]-=1
        if raw[12:14]!=b"\x08\0":
            continue
        assert len(raw)>=34 and raw[14]==0x45
        length=int.from_bytes(raw[16:18],"big")
        assert 20<=length<=len(raw)-14 and valid_sum(raw[14:34])
        if raw[23]!=1:
            continue
        body=raw[34:14+length]
        assert len(body)>=8
        if outbound:
            assert valid_sum(body)
            if body[0]!=8:
                continue
            all_echo.append(raw)
            if raw[18:20]!=b"\0\1":
                assert raw[22]==64,"existing ping TTL changed"
                continue
            identity=body[4:8]
            assert identity not in identities,"wire identity reused"
            identities.add(identity)
            assert length==60 and len(body)==40 and raw[26:30]==bytes([10,0,2,15])
            assert raw[30:34]==bytes([192,0,2,9]) and raw[22] in range(1,31)
            trace.append(raw[22])
        elif body[0] in (3,11) and valid_sum(body):
            assert len(body)==36 and body[8:10]==b"\x45\0" and body[17]==1
            assert int.from_bytes(body[10:12],"big")==60,"quote incorrectly requires full original datagram"
            assert valid_sum(body[8:28])
    assert all(count==0 for count in pending.values()),"injected frame missing from capture"
    expected=[ttl for scenario in scenarios for ttl in scenario["ttls"]]
    assert trace==expected,(trace,expected)
    assert any(raw[18:20]==b"\0\0" for raw in all_echo),"no existing ping regression evidence"
    return dict(trace_probes=len(trace),wire_identities=len(identities),injections=len(entries),ttl_progression=trace)


def negative_gates(root):
    """Verify fail-closed behavior against an otherwise passing real capture."""
    root=Path(root)
    scenarios=json.loads((root/"scenarios.json").read_text())
    audit(root/"wire.pcap",root/"injection.jsonl",scenarios)
    names=[]
    for name,content in (("missing",None),("empty",""),("malformed","{\n"),("incomplete","{}\n")):
        path=root/(name+"-audit-input.jsonl")
        if content is None:
            assert not path.exists()
        else:
            path.write_text(content)
        try:
            audit(root/"wire.pcap",path,scenarios)
        except (AssertionError,KeyError,json.JSONDecodeError,FileNotFoundError):
            names.append(name)
        else:
            raise AssertionError("audit accepted "+name+" input")
    wrong=json.loads(json.dumps(scenarios))
    wrong[0]["ttls"][0]=2
    try:
        audit(root/"wire.pcap",root/"injection.jsonl",wrong)
    except AssertionError:
        names.append("capture/app disagreement")
    else:
        raise AssertionError("audit accepted contradictory TTL expectation")
    (root/"audit-negative-tests.json").write_text(json.dumps(dict(rejected=names),indent=2)+"\n")
    return names
