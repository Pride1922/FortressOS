"""Independent DNS audit: imports capture container/TCP audit, no peer helpers."""
import json
import re
from collections import Counter
from pathlib import Path
from net_tcp_wire_audit import records, parse, folded

def qname(body):
    assert len(body)>=12
    at=12; labels=[]
    while body[at]:
        size=body[at]; assert 1<=size<=63 and at+1+size<len(body)
        labels.append(body[at+1:at+1+size].decode('ascii').lower()); at+=size+1
    assert body[at+1:at+5]==b'\0\x01\0\x01'
    return '.'.join(labels)

def audit(root):
    root=Path(root); rows=json.loads((root/'dns-cases.json').read_text())
    assert rows and len(rows)<=32
    # Use actual retained UART output, not a synthetic PASS field.
    serial=re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]','',(root/'serial.log').read_bytes().decode(errors='replace')).replace('\r','')
    for row in rows:
        assert row['output'] in serial and row['expected'] in row['output'],(row['command'],row['expected'])
    assert (root/'injection.jsonl').exists()
    inbound=Counter()
    for line in (root/'injection.jsonl').read_text().splitlines():
        record=json.loads(line); inbound[bytes.fromhex(record['hex'])]+=1
    socket_backend=json.loads((root/'scenario.json').read_text())['backend']=='socket'
    udp_queries={}; seen=[]; flows={}; count=0
    for _,raw in records(root/'wire.pcap'):
        count+=1; guest=raw[6:12]==bytes.fromhex('525400123456')
        if socket_backend and not guest:
            assert inbound[raw]>0,'capture has unlogged injection'; inbound[raw]-=1
        if len(raw)<42 or raw[12:14]!=b'\x08\x00': continue
        length=int.from_bytes(raw[16:18],'big'); ihl=(raw[14]&15)*4
        assert folded(raw[14:14+ihl])==65535 and 14+length<=len(raw)
        if raw[23]==1:
            body=raw[14+ihl:14+length]
            assert len(body)>=8 and folded(body)==65535
        if raw[23]==17:
            udp=raw[14+ihl:14+length]; source=int.from_bytes(udp[:2],'big'); dest=int.from_bytes(udp[2:4],'big')
            if 53 not in (source,dest): continue
            assert len(udp)==int.from_bytes(udp[4:6],'big')
            assert folded(raw[26:34]+b'\0\x11'+len(udp).to_bytes(2,'big')+udp)==65535
            body=udp[8:]; ident=int.from_bytes(body[:2],'big'); full_name=qname(body); name=full_name.split('.')[0]+'.test'
            if guest:
                assert dest==53 and body[2:12]==bytes.fromhex('01000001000000000000')
                udp_queries[(source,ident)]=(full_name,body); seen.append(full_name)
            else:
                assert source==53 and (dest,ident) in udp_queries
                expected,q=udp_queries[(dest,ident)]; assert full_name==expected and body[12:len(q)]==q[12:]
                if name in ('tcp.test','stall.test','trickle.test','eof.test'): assert body[2]&2
                elif name=='missing.test': assert body[3]&15==3
                elif name=='bad.test': assert body[-5:]==bytes.fromhex('00030a0002')
                elif name=='alias.test': assert b'\x07service' in body
                else: assert body[-4:]==bytes([10,0,2,2]) and body[3]&15==0
        elif raw[23]==6:
            packet=parse(raw)
            if packet is None or 53 not in (packet['source'],packet['dest']): continue
            port=packet['source'] if guest else packet['dest']; flow=flows.setdefault(port,{'base':[None,None],'data':[{},{}]})
            direction=0 if guest else 1
            if packet['flags']&2: flow['base'][direction]=(packet['seq']+1)&0xffffffff
            if packet['data']:
                assert flow['base'][direction] is not None
                at=(packet['seq']-flow['base'][direction])&0xffffffff
                assert at+len(packet['data'])<=4098
                for i,b in enumerate(packet['data'],at): assert flow['data'][direction].setdefault(i,b)==b
    tcp_names=[]
    for flow in flows.values():
        streams=[]
        for data in flow['data']:
            assert sorted(data)==list(range(len(data))), 'TCP framing gap'
            streams.append(bytes(data[i] for i in range(len(data))))
        query,reply=streams; assert len(query)>=2 and int.from_bytes(query[:2],'big')==len(query)-2
        full_name=qname(query[2:]); name=full_name.split('.')[0]+'.test'; tcp_names.append(name)
        if name=='tcp.test':
            assert len(reply)>=2 and int.from_bytes(reply[:2],'big')==len(reply)-2
            assert reply[2:4]==query[2:4] and qname(reply[2:])==full_name and reply[-4:]==bytes([10,0,2,2])
        elif name=='stall.test': assert not reply
        elif name=='trickle.test': assert 2<len(reply)<int.from_bytes(reply[:2],'big')+2
        elif name=='eof.test': assert len(reply)==1
        else: raise AssertionError('unexpected DNS TCP question')
    for row in rows:
        if row.get('dns_name'): assert row['dns_name'] in seen
    for name in ('tcp.test','stall.test','trickle.test','eof.test'): assert name in tcp_names
    assert sum(n.split('.')[0]=='timeout' for n in seen)==3,seen
    return dict(frames=count,udp_questions=seen,tcp_questions=tcp_names,independent=True)
