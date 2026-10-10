"""Finite source call-site inventory; not a proof of runtime authorization."""
from pathlib import Path
import hashlib,json,re
ROOT=Path(__file__).resolve().parent.parent
out=ROOT/'build/permissions-phase1';out.mkdir(parents=True,exist_ok=True)
trusted={'src/fs/ext2.c','src/fs/ext4.c','src/fs/ext4_mount_journal.inc',
 'src/fs/tarfs.c','src/fs/usb_mount.c','src/kernel/main.c','src/mm/memory_boot_test.c'}
operations='lookup|lookup_ref|open|open_ext|open_mode|open_exec|mkdir|unlink|rename|readdir|readdir_file|create|create_ext|create_ref|create_attrs_ref|create_node|truncate|open_terminal|chmod|fchmod|chown|write'
pattern=re.compile(r'\bvfs_('+operations+r')(_kernel|_creds)?\(')
records=[];sources={}
for p in sorted((ROOT/'src').rglob('*')):
 if p.suffix not in ('.c','.inc'):continue
 name=p.relative_to(ROOT).as_posix();s=p.read_text();sources[name]=hashlib.sha256(p.read_bytes()).hexdigest()
 for line,text in enumerate(s.splitlines(),1):
  for match in pattern.finditer(text):
   suffix=match[2]
   if name=='src/fs/vfs.c':kind='VFS implementation/trusted compatibility'
   elif suffix=='_creds':kind='actor entry'
   elif match[1]=='readdir_file' and name=='src/kernel/syscall.c':kind='previously admitted descriptor READ right'
   elif match[1]=='write' and name=='src/kernel/syscall.c':kind='admitted descriptor; current actor for filesystem set-ID clearing, streams use existing rights'
   elif match[1]=='write' and name in trusted:kind='trusted kernel descriptor write'
   elif suffix=='_kernel':
    if name in trusted:kind='boot/mount/explicit test kernel work'
    elif name=='src/kernel/thread.c':
     assert match[1] in ('open_ext','open_terminal'),(name,line,text)
     kind='non-user construction FD action/initial standard descriptors'
    elif name=='src/kernel/syscall.c':
     assert 'vfs_lookup_kernel("/mnt")' in text,(name,line,text)
     kind='fixed mount metrics query, not user-selected pathname'
    else:raise AssertionError(('unclassified trusted path',name,line,text))
   else:raise AssertionError(('unsuffixed production path',name,line,text))
   records.append({'file':name,'line':line,'api':match[0][:-1],'classification':kind})
 # Numeric signal API is trusted compatibility only inside the registry.
 if name!='src/kernel/process_table.c':assert not re.search(r'\bprocess_signal_send\(',s),name
syscall=(ROOT/'src/kernel/syscall.c').read_text()
for token in ('vfs_open_creds','vfs_lookup_creds','vfs_mkdir_creds','vfs_unlink_creds','vfs_rename_creds','vfs_readdir_file','process_signal_send_creds','CAP_SYS_BOOT'):
 assert token in syscall,token
thread=(ROOT/'src/kernel/thread.c').read_text()
assert 'vfs_open_exec_creds(path, &actor, &err)' in thread
assert 'vfs_open_mode_creds(act->path, act->flags, act->mode, &inherited' in thread
assert 'pid, spawn_flags, &result, &actor)' in thread
net=(ROOT/'src/net/net_socket_syscall.c').read_text()
assert 'ntohs(a.port)<1024' in net and 'process_record_creds(caller->tid,&actor)' in net
value={'scope':'source/API inventory including Phase2 metadata and descriptor content paths; dynamic admission evidence separate',
 'sources':sources,'call_sites':records,'unclassified':[]}
(out/'audit.json').write_text(json.dumps(value,indent=2)+'\n')
print(f'PASS permissions source inventory: {len(records)} classified sites, no unclassified path/trusted API calls; {out}/audit.json')
