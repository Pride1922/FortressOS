import tempfile,subprocess,re,struct,json
from pathlib import Path
from create_ext4_fixtures import ROOT,run
from test_jbd2_replay_host import FEATURES,blocks,be,put,seal,sbseal
(ROOT/'build/permissions-phase0').mkdir(parents=True,exist_ok=True)
out=Path(tempfile.mkdtemp(prefix='inodes-',dir=ROOT/'build/permissions-phase0'))
common=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread','-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm']
# EXT2 adapter intentionally uses its single-thread shims, not hardware locks.
subprocess.run([x for x in common if x!='-Itests/ext4_host']+['tests/perm_ext2_host.c','-o',str(out/'ext2')],cwd=ROOT,check=True)
subprocess.run(common+['tests/perm_ext4_host.c','tests/ext4_fault_disk.c','-o',str(out/'ext4')],cwd=ROOT,check=True)
records=[]
for fs,isz in [('ext2',128),('ext2',256),('ext4',256)]:
 source=out/f'{fs}-{isz}.img';result=out/f'{fs}-{isz}-result.img'
 with source.open('xb') as f:f.truncate(16*1024*1024 if fs=='ext2' else 32*1024*1024)
 features='none,filetype,sparse_super,large_file' if fs=='ext2' else FEATURES
 args=['mke2fs','-q','-t',fs,'-b','4096','-I',str(isz),'-O',features,'-E','lazy_itable_init=0','-m','0','-g','4096','-N','512','-F',str(source)]
 log=run(args)
 if fs=='ext4':
  commands=out/'journal.commands';commands.write_text('journal_open -c -v 3\njournal_close\n');log+=run(['debugfs','-w','-f',str(commands),str(source)])
  d=bytearray(source.read_bytes());jmap=blocks(source,f'<{struct.unpack_from("<I",d,1248)[0]}>');js=bytearray(d[jmap[0]*4096:(jmap[0]+1)*4096])
  put(js,28,0);put(js,88,1);put(js,40,0x11);head=bytearray(js[:1024]);seal(head,252,0xffffffff);js[:1024]=head
  d[jmap[0]*4096:(jmap[0]+1)*4096]=js;struct.pack_into('<I',d,1120,0x42);struct.pack_into('<H',d,1082,1);sbseal(d);source.write_bytes(d)
 text=run([str(out/fs),str(source),str(result)],timeout=900);print(text,flush=True);log+=text
 log+=run(['e2fsck','-fn',str(result)]);stat=run(['debugfs','-R','stat /mode',str(result)]);log+=stat
 assert re.search(r'Mode:\s+07777',stat) or re.search(r'Mode:\s+7777',stat),stat
 ids=re.search(r'User:\s+(-?\d+)\s+Group:\s+(-?\d+)',stat);assert ids and tuple(int(v)&0xffffffff for v in ids.groups())==(0x12345678,0x87654321),stat
 (out/f'{fs}-{isz}.log').write_text(log);records.append({'fs':fs,'inode_size':isz,'mkfs':args})
text=run([str(out/'ext4'),str(out/'ext4-256.img'),str(out/'attrs'), '--cuts'],timeout=900)
print(text,flush=True)
for state in ('old','new'):
 image=out/f'attrs-{state}.img'
 log=run(['e2fsck','-fn',str(image)])+run(['debugfs','-R','stat /attributes',str(image)])
 if state=='old':assert 'File not found' in log
 else:
  assert re.search(r'Mode:\s+06751',log),log
  ids=re.search(r'User:\s+(-?\d+)\s+Group:\s+(-?\d+)',log)
  assert ids and tuple(int(v)&0xffffffff for v in ids.groups())==(0x12345678,0x87654321),log
 (out/f'attrs-{state}.linux.log').write_text(log)
(out/'attrs-cuts.log').write_text(text)
(out/'manifest.json').write_text(json.dumps(records,indent=2));print(f'PASS inode full mode/owner matrix + Linux fsck/stat: {out}',flush=True)
