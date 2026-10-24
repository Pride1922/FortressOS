"""Linux-produced, checksummed recovery fixture for private boot tests."""
import struct
from pkg_enroll_fixture import command,BS
from test_jbd2_replay_host import blocks,be,put,seal,sbseal,crc

def pending(source,directory,grant_block,bad_projected=False):
    directory.mkdir();disk=source.read_bytes();root=directory/'root.img'
    root.write_bytes(disk[2048*512:(2048+65536)*512])
    payload=directory/'payload';good=root.read_bytes()[grant_block*BS:(grant_block+1)*BS]
    content=bytearray(good)
    if bad_projected:content[0]^=1
    payload.write_bytes(content)
    commands=directory/'journal.commands'
    commands.write_text(f'journal_open -c -v 3\njournal_write -b {grant_block} {payload}\njournal_close\n')
    log=command(['debugfs','-w','-f',str(commands),str(root)])
    data=bytearray(root.read_bytes());ino=struct.unpack_from('<I',data,1248)[0];mapping=blocks(root,f'<{ino}>')
    js=data[mapping[0]*BS:(mapping[0]+1)*BS];assert be(js,40)==0x11 and be(js,28)
    seed=crc(0xffffffff,data[1128:1144]);cursor=be(js,28);normalized=0
    # e2fsprogs experimental writer emits zero descriptor UUID slots. Match
    # the independently documented normalization in test_jbd2_replay_host.py.
    while cursor<len(mapping):
        where=mapping[cursor]*BS;b=bytearray(data[where:where+BS])
        if be(b,0)!=0xc03b3998:break
        kind=be(b,4);cursor+=1
        if kind!=1:continue
        at=12
        while at+16<=BS-4:
            flags=be(b,at+4);at+=16;cursor+=1
            if not flags&2:
                assert not any(b[at:at+16]);b[at:at+16]=data[1128:1144];at+=16;normalized+=1
            if flags&8:break
        seal(b,BS-4,seed);data[where:where+BS]=b
    assert normalized==1
    data[grant_block*BS]^=0x80
    struct.pack_into('<I',data,1120,0x46);struct.pack_into('<H',data,1082,0);sbseal(data)
    initial=bytearray(disk);initial[2048*512:(2048+65536)*512]=data
    expected=bytearray(initial);expected[(2048*512+grant_block*BS):(2048*512+(grant_block+1)*BS)]=good
    journal=bytearray(js);put(journal,24,be(js,24)+1);put(journal,28,0);put(journal,88,be(js,20))
    head=bytearray(journal[:1024]);seal(head,252,0xffffffff);journal[:1024]=head
    start=2048*512+mapping[0]*BS;expected[start:start+BS]=journal
    source_out=directory/'pending.img';source_out.write_bytes(initial)
    expected_out=directory/'expected.img';expected_out.write_bytes(expected)
    (directory/'generation.log').write_text(log+f'Normalized UUID slots: {normalized}\n')
    return source_out,expected_out
