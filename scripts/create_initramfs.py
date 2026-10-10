"""Build the Phase0 root-owned USTAR layout independent of host/DrvFS modes."""
import sys,tarfile
from pathlib import Path
source=Path(sys.argv[1]);target=Path(sys.argv[2])
def metadata(member):
 if member.name=='bin/benchrun':return None
 if not (member.isdir() or member.isfile()):raise ValueError(f'Unsupported initramfs entry: {member.name}')
 member.uid=member.gid=0;member.uname=member.gname=''
 member.mode=0o755 if member.isdir() or member.name.startswith('bin/') else 0o644
 if member.name=='etc/shadow':member.mode=0o600
 if member.name=='bin/sudo':member.mode=0o4755
 return member
with tarfile.open(target,'w',format=tarfile.USTAR_FORMAT) as archive:
 for name in ('bin','etc','docs'):archive.add(source/name,arcname=name,filter=metadata)
