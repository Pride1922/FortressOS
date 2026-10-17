#!/usr/bin/env python3
"""Actual-code sanitizer, upstream identity, differential and freestanding audit."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parent.parent
EVIDENCE=ROOT/'.codex-remote-attachments/crypto-phase2'
BUILD=ROOT/'build/crypto-host'
def run(cmd,log):
    print('+ '+' '.join(map(str,cmd)),file=log,flush=True)
    subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
def main():
    EVIDENCE.mkdir(parents=True,exist_ok=True);BUILD.mkdir(parents=True,exist_ok=True)
    manifest=json.loads((ROOT/'src/lib/crypto/upstream.json').read_text())
    for p,h in manifest['files'].items():
        assert hashlib.sha256((ROOT/p).read_bytes()).hexdigest()==h,p
    common=['gcc','-std=c11','-Wall','-Wextra','-Werror','-Isrc/include','-Itests','-Isrc/lib/crypto/upstream/src']
    wrappers=[f'src/lib/crypto/{s}.c' for s in ('hash','curve','aead','entropy')]
    vendors=[('src/lib/crypto/upstream/src/monocypher.c','mono'),('src/lib/crypto/upstream/src/optional/monocypher-ed25519.c','ed')]
    with (EVIDENCE/'host.log').open('w') as log:
        flags=common+['-O1','-g','-no-pie','-DCRYPTO_HOST_TEST','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        objects=[]
        for p,n in vendors:
            o=str(BUILD/(n+'.o'));objects.append(o)
            run(flags+['-include','src/lib/crypto/namespace.h','-c',p,'-o',o],log)
        run(flags+['tests/crypto_host.c',*wrappers,'src/lib/root_digest.c',*objects,'-o',str(BUILD/'crypto_host')],log)
        run([str(BUILD/'crypto_host')],log)
        flags=common+['-O2','-fPIC','-DCRYPTO_HOST_TEST']
        objects=[]
        for p,n in vendors:
            o=str(BUILD/(n+'-pic.o'));objects.append(o)
            run(flags+['-include','src/lib/crypto/namespace.h','-c',p,'-o',o],log)
        run(flags+['-shared',*wrappers,'tests/crypto_diff_shim.c','src/lib/root_digest.c',*objects,'-o',str(BUILD/'differential.so')],log)
        run([sys.executable,'scripts/test_crypto_differential.py',str(BUILD/'differential.so')],log)
        run(['make','-j8','build/crypto/libfortresscrypto.a'],log)
    audit_stack()
    print(f'PASS crypto sanitizer/differential/upstream identity; evidence: {EVIDENCE / "host.log"}')

def audit_stack():
    # Only reachable functions retained by the partial linker belong to library.
    # Excluded upstream Argon2/HKDF/Elligator have no wrapper/API exposure.
    lib=ROOT/'build/crypto/library.o'
    nm=subprocess.check_output(['nm',str(lib)],text=True)
    assert not re.search(r' [BDGSC] ',nm),'global writable library state'
    assert 'mono_crypto_argon2' not in nm and 'mono_crypto_elligator' not in nm
    names={line.split()[-1] for line in nm.splitlines() if re.search(r' [tT] ',line)}
    names.update(line.split()[-1] for line in nm.splitlines() if re.search(r' U ',line))
    frames={}
    for p in (ROOT/'build/crypto').glob('*.su'):
        if p.name=='probe.su':continue
        for line in p.read_text().splitlines():
            loc,n,kind=line.split('\t');name=loc.split(':')[-1]
            # GCC suffixes are normalized for matching; choose largest if duplicated.
            base=name.split('.')[0]
            if any(x.split('.')[0]==base for x in names):
                # GCC labels fixed outgoing stack-argument pushes (AEAD's
                # eight arguments) dynamic,bounded; the reported bound is finite.
                assert kind in ('static','dynamic,bounded'),line
                frames[base]=max(frames.get(base,0),int(n))
    large={p:n for p,n in frames.items() if n>=1024}
    disassembly=subprocess.check_output(['objdump','-dr',str(lib)],text=True)
    graph={};current=None;call=False
    for line in disassembly.splitlines():
        m=re.match(r'^[0-9a-f]+ <([^>]+)>:',line)
        if m:
            current=m[1].split('.')[0];graph.setdefault(current,set());call=False
        elif current:
            if re.search(r'\b(call|jmp)\s',line):
                call=True
                m=re.search(r'<([^>+]+)',line)
                if m and m[1].split('.')[0]!=current:graph[current].add(m[1].split('.')[0])
            elif 'R_X86_64_' in line and call:
                target=line.split()[-1].split('-0x')[0]
                if target.startswith('.text.'):target=target[len('.text.'):]
                target=target.split('.')[0]
                if target!=current:graph[current].add(target)
                call=False
            elif re.match(r'^\s*[0-9a-f]+:',line):call=False
    def depth(name,path=()):
        assert name not in path,('recursive crypto call graph',path,name)
        return frames.get(name,0)+max([depth(n,path+(name,)) for n in graph.get(name,()) if n in frames] or [0])
    depths={n:depth(n) for n in frames if n.startswith('crypto_')}
    report={'frames':frames,'over_1k':large,'conservative_call_chain_bytes':depths,
            'status':'pending requested stack exception' if large else 'PASS'}
    (EVIDENCE/'stack.json').write_text(json.dumps(report,indent=2))
    # Record machine code for source/optimized-path review. This is supporting
    # inspection, not a proof of constant-time execution or compiler validation.
    (EVIDENCE/'library.disassembly.txt').write_text(disassembly)
    assert not large,f'User stack-limit decision required: {large}; see stack.json'

if __name__=='__main__':main()
