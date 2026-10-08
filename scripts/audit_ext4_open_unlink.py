"""Case 3 original orphan audit and independent Linux cleanup on a copy."""
import hashlib,json,struct,subprocess,sys,shutil
from pathlib import Path
from test_nmi_transitions import REPO

def command(args):
 r=subprocess.run(args,capture_output=True,text=True);return r.returncode,r.stdout+r.stderr

def bitmap_sets(d):
 bs=4096; ipg=struct.unpack_from('<I',d,1064)[0]; bpg=struct.unpack_from('<I',d,1056)[0]
 groups=(struct.unpack_from('<I',d,1028)[0]+bpg-1)//bpg
 result=[]
 for g in range(groups):
  bb,ib=struct.unpack_from('<II',d,bs+g*32)
  result.append((d[bb*bs:bb*bs+(bpg+7)//8],d[ib*bs:ib*bs+(ipg+7)//8]))
 return result

def verify_pending(source,baseline,out):
 d=source.read_bytes(); seed=baseline.read_bytes(); ino=struct.unpack_from('<I',d,1256)[0]; assert ino
 ipg=struct.unpack_from('<I',d,1064)[0]; group=(ino-1)//ipg; index=(ino-1)%ipg
 table=struct.unpack_from('<I',d,4096+group*32+8)[0]; inode=d[table*4096+index*256:table*4096+(index+1)*256]
 assert struct.unpack_from('<H',inode,26)[0]==0 and struct.unpack_from('<I',inode,20)[0]==0
 assert struct.unpack_from('<I',inode,4)[0]==8192
 code,stat=command(['debugfs','-R','stat /cut-open-unlink.bin',str(source)]); assert code==0 and 'File not found' in stat
 target=out/'orphan-payload.bin';assert not target.exists()
 code,log=command(['debugfs','-R',f'dump <{ino}> {target}',str(source)]); assert code==0
 expected=bytes((k*17+3)&255 for k in range(8192)); assert target.read_bytes()==expected;target.unlink()
 # Two data blocks and exactly one inode remain allocated while pinned.
 assert struct.unpack_from('<I',seed,1036)[0]-struct.unpack_from('<I',d,1036)[0]==2
 assert struct.unpack_from('<I',seed,1040)[0]-struct.unpack_from('<I',d,1040)[0]==1
 oldmaps=bitmap_sets(seed);newmaps=bitmap_sets(d);delta=[0,0]
 for old,new in zip(oldmaps,newmaps):
  for kind in (0,1):
   for a,b in zip(old[kind],new[kind]):
    assert a & ~b == 0,'unrelated allocation disappeared at pause'
    delta[kind]+=(a^b).bit_count()
 assert delta==[2,1],'unexpected block/inode ownership delta'
 (out/'orphan-pending-stat.log').write_text(stat+log)
 return ino

def verify_reclaimed(source,baseline,out):
 d=source.read_bytes(); seed=baseline.read_bytes()
 assert struct.unpack_from('<I',d,1256)[0]==0
 assert bitmap_sets(d)==bitmap_sets(seed),'allocation ownership did not return to baseline'
 assert d[1036:1044]==seed[1036:1044],'free counts did not return to baseline'
 code,stat=command(['debugfs','-R','stat /cut-open-unlink.bin',str(source)]);assert code==0 and 'File not found' in stat
 code,log=command(['e2fsck','-fn',str(source)]); assert code==0,log
 (out/'orphan-reclaimed-fsck.log').write_text(log+stat)
 for name,size,wanted in [('test-1M.bin',1048576,'470952a05336a638e11755d028432cb890c3240d0b33668038a975e7e3b5b4ef'),
                          ('test-16M.bin',16777216,'71b83c780eeb1a2665d5aabf71229490ae069450ef9b88ca91d1229fe17f45c6')]:
  target=out/('orphan-audit-'+name);assert not target.exists()
  code,log=command(['debugfs','-R',f'dump /{name} {target}',str(source)]);assert code==0
  payload=target.read_bytes();assert len(payload)==size and hashlib.sha256(payload).hexdigest()==wanted
  target.unlink()

def main():
 source=Path(sys.argv[1]).resolve(); info=json.loads(Path(sys.argv[2]).read_text()); baseline=Path(info['data_source'])
 root=(REPO/'.codex-remote-attachments/ext4-phase9').resolve();assert source.is_relative_to(root)
 digest=hashlib.sha256(source.read_bytes()).hexdigest();assert source.stat().st_size==33554432
 cap=json.loads((source.parent/'manifest.json').read_text(encoding='utf-8-sig'))
 assert cap['sha256']==digest and cap['physical_writes'] is False and cap['data_partuuid']==info['data_partuuid']
 assert hashlib.sha256(baseline.read_bytes()).hexdigest()==info['data_source_sha256']
 out=source.parent;ino=verify_pending(source,baseline,out)
 target=out/'case3-linux-recovered.ext4';assert not target.exists();shutil.copyfile(source,target)
 code,log=command(['e2fsck','-fy',str(target)]);assert code in (0,1),log
 (out/'case3-linux-recovery.log').write_text(log);verify_reclaimed(target,baseline,out)
 assert hashlib.sha256(source.read_bytes()).hexdigest()==digest
 (out/'case3-audit.json').write_text(json.dumps({'status':'PASS','source_sha256':digest,'orphan_inode':ino,'retained_bytes':True,'linux_cleanup':True,'allocation_baseline_restored':True,'physical_writes':False},indent=2))
 print('PASS: durable orphan payload/ownership; Linux cleanup exactly restores allocation baseline')
if __name__=='__main__':main()
