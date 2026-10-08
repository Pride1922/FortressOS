"""Read-only Case 2 capture audit; Linux replay operates only on a copy."""
import hashlib,json,sys,subprocess
from pathlib import Path
from test_jbd2_replay_host import oracle,blocks,be
from test_nmi_transitions import REPO

def main():
 source=Path(sys.argv[1]).resolve(); manifest=Path(sys.argv[2]).resolve()
 root=(REPO/'.codex-remote-attachments/ext4-phase9').resolve()
 assert source.is_relative_to(root) and manifest.is_relative_to(root)
 info=json.loads(manifest.read_text()); seedpath=Path(info['data_source']); seed=seedpath.read_bytes()
 assert hashlib.sha256(seed).hexdigest()==info['data_source_sha256']
 data=source.read_bytes(); assert len(data)==33554432
 digest=hashlib.sha256(data).hexdigest(); capture=json.loads((source.parent/'manifest.json').read_text(encoding='utf-8-sig'))
 assert capture['sha256']==digest and capture['physical_writes'] is False and capture['data_partuuid']==info['data_partuuid']
 out=source.parent; copy=out/'case2-linux-recovered.ext4'; assert not copy.exists()
 oracle(source,copy,out/'case2-linux-recovery.log'); final=copy.read_bytes()
 changed=[k for k in range(0,len(seed),4096) if seed[k:k+4096]!=final[k:k+4096]]
 applied=[k for k in changed if data[k:k+4096]==final[k:k+4096]]
 remaining=[k for k in changed if data[k:k+4096]==seed[k:k+4096]]
 assert applied and remaining
 # The replay journal must remain active at the captured pause.
 import struct
 ino=struct.unpack_from('<I',seed,1248)[0]; mapping=blocks(seedpath,f'<{ino}>')
 assert be(data[mapping[0]*4096:mapping[0]*4096+1024],28)!=0
 stat=subprocess.run(['debugfs','-R','stat /cut-commit.txt',str(copy)],capture_output=True,text=True,check=True)
 assert 'Type: regular' in stat.stdout and 'Size: 0' in stat.stdout
 (out/'case2-linux-cut-stat.log').write_text(stat.stdout+stat.stderr)
 assert hashlib.sha256(source.read_bytes()).hexdigest()==digest
 (out/'case2-audit.json').write_text(json.dumps({'status':'PASS','source_sha256':digest,'applied_offsets':applied,'remaining_offsets':remaining,'journal_retained':True,'linux_recovery':True,'physical_writes':False},indent=2))
 print('PASS: durable partial replay, retained journal, Linux restart; original untouched')
if __name__=='__main__':main()
