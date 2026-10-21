"""Private C candidate gate; no production build, filesystem or media writes."""
import datetime
import hashlib
import json
import pathlib
import struct
import subprocess


def main():
    out = pathlib.Path('.codex-remote-attachments') / ('pkg-grant-codec-' +
        datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    out.mkdir(parents=True, exist_ok=False)
    sources = ['tests/pkg_grant_candidate.c', 'tests/pkg_grant_candidate.h',
               'tests/pkg_grant_candidate_host.c', 'scripts/test_pkg_grant_candidate.py',
               'tests/pkg_grant_candidate.mk', 'user/tools/digest.c',
               'user/tools/digest.h', 'src/include/types.h', 'AGENTS.md', 'README.md']
    def inventory():
        return {p: hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest() for p in sources}
    before = inventory()
    def seal(b):
        b[992:1024] = hashlib.sha256(b[:992]).digest()
        return b
    def put(b, offset, value, fmt='Q'):
        struct.pack_into('<'+fmt, b, offset, value)
    def fixture(phase):
        b = bytearray(1024)
        b[:16] = b'FORT-PKG-GRT-v1\0'
        put(b,16,1024,'I'); put(b,20,1,'H'); put(b,22,phase,'H')
        put(b,24,phase+1); put(b,32,1)
        for offset in (56,72,88): b[offset:offset+16] = bytes([offset])*16
        for offset in (104,168,232,264): b[offset:offset+32] = bytes([offset%255+1])*32
        put(b,344,1,'I'); put(b,352,44,'I'); put(b,356,1,'I')
        for offset in (360,376,384): put(b,offset,1)
        if phase:
            b[40:56] = b'T'*16; b[392:424] = b'P'*32
            put(b,348,1,'H')
            for offset in (296,304,312,320): put(b,offset,4)
        if phase >= 2:
            b[136:168] = b'I'*32; b[200:232] = b'N'*32; put(b,368,2)
        elif not phase:
            b[200:232] = b[168:200]; put(b,368,1)
        return seal(b)
    vectors=[]
    def save(name,b):
        path=out/(name+'.bin'); path.write_bytes(b); vectors.append(str(path))
    for phase in range(4): save('ok-phase-'+str(phase),fixture(phase))
    b=fixture(2)
    for offset in (312,320,328,336): put(b,offset,2)
    save('ok-charged',seal(b))
    b=fixture(1); put(b,24,2**64-1); save('ok-last-sequence-readonly',seal(b))
    # Exact EOF and checksum defects, then rehashed semantic defects.
    for n in (0,1,15,16,991,992,1023): save('bad-short-'+str(n),fixture(1)[:n])
    save('bad-trailing',fixture(1)+b'x')
    for pos in range(1024):
        b=fixture(1); b[pos]^=1; save('bad-bit-'+str(pos),b)
    mutations=[(16,1023,'I'),(20,2,'H'),(22,4,'H'),(24,0,'Q'),(32,0,'Q'),
               (344,0,'I'),(344,2,'I'),(348,0,'H'),(348,4,'H'),(350,1,'H'),
               (352,0,'I'),(356,0,'I'),(360,0,'Q'),(376,0,'Q'),(384,0,'Q'),
               (296,5,'Q'),(328,2**64-1,'Q'),(368,1,'Q'),(424,1,'Q')]
    for k,(offset,value,fmt) in enumerate(mutations):
        b=fixture(1); put(b,offset,value,fmt); save('bad-semantic-'+str(k),seal(b))
    for offset,length in ((40,16),(56,16),(72,16),(88,16),(104,32),(168,32),
                          (232,32),(264,32),(392,32)):
        b=fixture(1); b[offset:offset+length]=bytes(length)
        save('bad-zero-'+str(offset),seal(b))
    for offset in range(432,992):
        b=fixture(1); b[offset]=1; save('bad-reserved-'+str(offset),seal(b))
    for phase,offset in ((0,40),(0,136),(0,200),(0,296),(0,348),(0,392),
                         (1,136),(1,200),(2,136),(2,200),(3,136),(3,200)):
        b=fixture(phase); b[offset]^=1
        if phase>=2: b[offset:offset+32]=bytes(32)
        save('bad-phase-%d-%d'%(phase,offset),seal(b))
    flags=['gcc','-std=c11','-Wall','-Wextra','-Werror','-g','-O1',
           '-I.','-Isrc/include','-Iuser/tools','-ffunction-sections','-fdata-sections']
    commands=[]
    def run(args,name):
        commands.append(args)
        r=subprocess.run(args,text=True,capture_output=True,timeout=120)
        (out/(name+'.stdout')).write_text(r.stdout)
        (out/(name+'.stderr')).write_text(r.stderr)
        assert r.returncode==0, (name,r.returncode,r.stderr)
        return r.stdout
    try:
        exe=str(out/'host')
        run(flags+['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
            '-Wl,--gc-sections','tests/pkg_grant_candidate.c',
            'tests/pkg_grant_candidate_host.c','user/tools/digest.c','-o',exe], 'compile')
        print(run([exe,*vectors],'host'),end='')
        run(flags+['-ffreestanding','-fno-builtin','-mno-red-zone','-mno-sse',
            '-mno-mmx','-msoft-float','-fstack-usage','-Wframe-larger-than=1024',
            '-c','tests/pkg_grant_candidate.c','-o',str(out/'candidate.o')], 'object')
    finally:
        after=inventory()
        (out/'manifest.json').write_text(json.dumps({'before':before,'after':after,
            'changed':[p for p in before if before[p]!=after[p]],'commands':commands,
            'vectors':len(vectors),'scope':'private C codec and arithmetic; byte-tear detection and mock before/after commit snapshots only; no JBD2, filesystem, boot, Ring3 or media acceptance'},indent=2)+'\n')
    assert before==after
    print('Evidence:',out)


if __name__ == '__main__':
    main()
