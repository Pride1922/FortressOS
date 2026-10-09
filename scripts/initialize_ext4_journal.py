"""Initialize an empty internal JBD2 CSUM_V3/revoke journal on a fresh image."""
import struct,subprocess,tempfile
from pathlib import Path

def crc32c(data):
    crc=0xffffffff
    for byte in data:
        crc ^= byte
        for _ in range(8): crc=(crc>>1)^(0x82f63b78 if crc&1 else 0)
    return crc

def initialize(path: Path):
    with tempfile.TemporaryDirectory(prefix='fortress-journal-init-') as temp:
        commands=Path(temp)/'commands'
        commands.write_text('journal_open -c -v 3\njournal_close\n')
        subprocess.run(['debugfs','-w','-f',str(commands),str(path)],check=True,capture_output=True)
    data=bytearray(path.read_bytes())
    assert struct.unpack_from('<I',data,1024+92)[0]&4
    ino=struct.unpack_from('<I',data,1024+224)[0]
    result=subprocess.run(['debugfs','-R',f'blocks <{ino}>',str(path)],check=True,capture_output=True,text=True)
    blocks=[int(value) for value in result.stdout.split()];assert len(blocks)>=1024
    offset=blocks[0]*4096
    journal=bytearray(data[offset:offset+1024])
    assert struct.unpack_from('>II',journal)[0:2]==(0xc03b3998,4)
    assert struct.unpack_from('>I',journal,40)[0] in (0x10,0x11)
    for where,value in ((24,1),(28,0),(40,0x11),(88,1),(252,0)):
        struct.pack_into('>I',journal,where,value)
    struct.pack_into('>I',journal,252,crc32c(journal))
    data[offset:offset+1024]=journal
    struct.pack_into('<I',data,1120,0x42)
    struct.pack_into('<H',data,1082,1)
    struct.pack_into('<I',data,2044,crc32c(data[1024:2044]))
    path.write_bytes(data)
