"""Construct valid full depth-1 trees, then inventory and test mounted growth."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from test_ext4_crash_inventory import ROOT, command, linux_snapshot


def tree(image, bs, ino):
    with image.open('rb') as stream:
        stream.seek(1024);sb=stream.read(1024)
        first=struct.unpack_from('<I',sb,20)[0];ipg=struct.unpack_from('<I',sb,40)[0]
        size=struct.unpack_from('<H',sb,88)[0];group,within=divmod(ino-1,ipg)
        stream.seek((first+1)*bs+group*32);table=struct.unpack('<I',stream.read(12)[8:12])[0]
        stream.seek(table*bs+within*size+40);root=stream.read(60)
        depth=struct.unpack_from('<H',root,6)[0];count=struct.unpack_from('<H',root,2)[0]
        assert depth<=1 and struct.unpack_from('<H',root)[0]==0xf30a
        nodes=[]
        for k in range(count if depth else 1):
            if depth:
                logical,physical=struct.unpack_from('<II',root,12+k*12)
                assert not struct.unpack_from('<H',root,20+k*12)[0]
                stream.seek(physical*bs);raw=stream.read(bs)
            else:logical,raw=0,root
            n,capacity=struct.unpack_from('<HH',raw,2)
            entries=[]
            for at in range(n):
                start,length=struct.unpack_from('<IH',raw,12+at*12)
                entries.append((start,length-32768 if length>32768 else length))
            nodes.append({'logical':logical,'count':n,'capacity':capacity,'entries':entries})
        return depth,nodes


def build(source,image,bs,folder):
    shutil.copyfile(source,image);log=folder/f'{image.stem}.log'
    stat=command(['debugfs','-R','stat /target.bin',str(image)],log)
    ino=int(re.search(r'Inode:\s+(\d+)',stat)[1]);serial=0
    def allocate(points):
        nonlocal serial
        assert points
        commands=folder/f'{image.stem}-{serial}.commands';serial+=1
        commands.write_text(''.join(f'fallocate /target.bin {p} {p}\n' for p in points))
        command(['debugfs','-w','-f',str(commands),str(image)],log)
    for _ in range(8):
        depth,nodes=tree(image,bs,ino)
        if depth==1 and len(nodes)==4:break
        highest=max(start+length for node in nodes for start,length in node['entries'])
        count=(bs-16)//24 if not depth else nodes[-1]['capacity']-nodes[-1]['count']+1
        allocate([max(100,highest+8)+8*k for k in range(count)])
    else:raise AssertionError('bounded root construction')
    for index in range(4):
        depth,nodes=tree(image,bs,ino);assert depth==1 and len(nodes)==4
        node=nodes[index];need=node['capacity']-node['count'];points=[]
        upper=nodes[index+1]['logical'] if index<3 else node['entries'][-1][0]+8*(need+2)
        previous_end=node['entries'][0][0]+node['entries'][0][1]
        for start,length in node['entries'][1:]+[(upper,0)]:
            point=max(10,previous_end+1)
            while point<=start-2 and len(points)<need:
                points.append(point);point+=2
            previous_end=start+length
        assert len(points)==need, ('insufficient separated slots',index,need,len(points))
        if points:allocate(points)
    depth,nodes=tree(image,bs,ino)
    assert depth==1 and len(nodes)==4 and all(n['count']==n['capacity'] for n in nodes)
    size=max(start+length for node in nodes for start,length in node['entries'])*bs
    command(['debugfs','-w','-R',f'set_inode_field /target.bin size {size}',str(image)],log)
    linux_snapshot(image,bs,'sync',log,deep_size=size)
    print(f'PASS fixture {image.name}: full depth-1 tree and Linux audit',flush=True)
    return {'deep_size':size,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
            'sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'leaves':nodes}


def main():
    parser=argparse.ArgumentParser();parser.add_argument('fixtures',type=Path);args=parser.parse_args()
    allowed=(ROOT/'.codex-remote-attachments').resolve();source=args.fixtures.resolve()
    assert source.is_relative_to(allowed) and (source/'manifest.json').is_file()
    evidence=allowed/'ext4-phase9';out=Path(tempfile.mkdtemp(prefix='depth-',dir=evidence));records=[]
    for bs in (1024,2048,4096):
        for placement in ('normal','wrap'):
            name=f'{bs}-{placement}.img'
            record=build(source/name,out/name,bs,out);record.update(block=bs,placement=placement)
            records.append(record)
    (out/'manifest.json').write_text(json.dumps({'cases':records,'errors':[]},indent=2)+'\n')
    inventories=set(evidence.glob('foundation-*'))
    subprocess.run([sys.executable,str(ROOT/'scripts/test_ext4_crash_inventory.py'),str(out),'--write-only'],check=True)
    created=set(evidence.glob('foundation-*'))-inventories;assert len(created)==1
    inventory=created.pop();manifest=json.loads((inventory/'manifest.json').read_text());assert len(manifest['cases'])==12
    work=out/'promotion.img';proof=[]
    for case in manifest['cases']:
        label=f'{case["block"]}-{case["placement"]}-{case["sector"]}-write'
        work.write_bytes(gzip.decompress((inventory/f'{label}-clean.img.gz').read_bytes()))
        stat=command(['debugfs','-R','stat /target.bin',str(work)],out/f'{label}-promotion.log')
        assert '(ETB0)' in stat and '(ETB1)' in stat,stat
        proof.append(label)
    work.unlink();(out/'promotion.json').write_text(json.dumps({'inventory':str(inventory),'cases':proof},indent=2)+'\n')
    print(f'Depth-1 to depth-2 promotion PASS 12/12; inventory: {inventory}',flush=True)
    subprocess.run([sys.executable,str(ROOT/'scripts/test_ext4_crash_recovery.py'),str(inventory)],check=True)
    print(f'Depth campaign PASS; fixtures: {out}',flush=True)


if __name__=='__main__':main()
