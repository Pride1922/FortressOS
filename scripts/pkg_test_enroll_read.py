"""Read-only private artifact admission; policy must come from outside the bundle.

Exit 0=protected candidate, 2=verified legacy absence, 1=refusal. This does not
mount a root or turn either result into writable production admission.
"""
import argparse,json,os,pathlib,stat,subprocess,tempfile

def snapshot(path,limit):
 fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
 with os.fdopen(fd,'rb') as f:
  st=os.fstat(f.fileno())
  if not stat.S_ISREG(st.st_mode) or st.st_nlink!=1 or st.st_uid!=os.getuid() or stat.S_IMODE(st.st_mode)!=0o600 or st.st_size>limit:raise ValueError('private regular snapshot required')
  data=f.read(limit+1)
  if len(data)!=st.st_size:raise ValueError('short/changing snapshot')
  return data

def inspect(directory,required,verifier,policy):
 d=pathlib.Path(directory)
 try:
  try:top=d.lstat()
  except FileNotFoundError:return 'REFUSE' if required else 'LEGACY'
  if not stat.S_ISDIR(top.st_mode):return 'REFUSE'
  # lstat distinguishes verified absence from inaccessible/malformed namespace.
  dirs=[]
  for name in ('root','boot'):
   p=d/name
   try:st=p.lstat()
   except FileNotFoundError:continue
   if not stat.S_ISDIR(st.st_mode) or st.st_uid!=os.getuid() or stat.S_IMODE(st.st_mode)!=0o700:return 'REFUSE'
   dirs.append(name)
  if not dirs:return 'REFUSE' if required else 'LEGACY'
  if set(dirs)!={'root','boot'}:return 'REFUSE'
  if {p.name for p in (d/'root').iterdir()}!={'records.bin'} or {p.name for p in (d/'boot').iterdir()}!={'selector.bin'}:return 'REFUSE'
  bundle=snapshot(d/'root/records.bin',32768);selector=snapshot(d/'boot/selector.bin',1536)
  if not required:return 'REFUSE'
  with tempfile.TemporaryDirectory(prefix='fortress-enroll-read-') as tmp:
   tmp=pathlib.Path(tmp);(tmp/'bundle').write_bytes(bundle);(tmp/'selector').write_bytes(selector)
   r=subprocess.run([str(pathlib.Path(verifier).resolve()),str(pathlib.Path(policy).resolve()),str(tmp/'selector'),str(tmp/'bundle'),'1'],capture_output=True,text=True,timeout=30)
   return 'PROTECTED' if not r.returncode else 'REFUSE'
 except (OSError,ValueError,subprocess.TimeoutExpired):return 'REFUSE'

def main():
 p=argparse.ArgumentParser();p.add_argument('--directory',required=True);p.add_argument('--mode',choices=('required','legacy'),required=True);p.add_argument('--verifier',required=True);p.add_argument('--policy',required=True);a=p.parse_args()
 result=inspect(a.directory,a.mode=='required',a.verifier,a.policy);print(json.dumps({'private_artifact_admission':result,'production_activation':False}));return {'PROTECTED':0,'LEGACY':2,'REFUSE':1}[result]
if __name__=='__main__':raise SystemExit(main())
