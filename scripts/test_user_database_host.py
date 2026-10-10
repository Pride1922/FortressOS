from pathlib import Path
import os,subprocess,tarfile,tempfile
root=Path(__file__).resolve().parent.parent
out=root/'build/permissions-phase3';out.mkdir(parents=True,exist_ok=True)
cmd=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie','-Isrc/include',
     'tests/userdb_host.c','user/tools/userdb.c','user/tools/digest.c','-lcrypt','-o',str(out/'userdb-host')]
subprocess.run(cmd,cwd=root,check=True)
subprocess.run([str(out/'userdb-host')],cwd=root,check=True)
cmd=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',
     '-DTOOL_HOST_TEST','-Isrc/include','-Isrc/fs','tests/login_host.c','user/tools/common.c',
     'user/tools/userdb.c','user/tools/digest.c','-o',str(out/'login-host')]
subprocess.run(cmd,cwd=root,check=True)
subprocess.run([str(out/'login-host')],cwd=root,check=True)
with tempfile.TemporaryDirectory(prefix='fortress-db-stage-') as directory:
    stage=Path(directory);(stage/'bin').mkdir();(stage/'docs').mkdir();(stage/'bin/sudo').write_bytes(b'fixture')
    environment={k:v for k,v in os.environ.items() if k!='FORTRESS_OPERATOR_HASH_FILE'}
    subprocess.run(['python3','scripts/stage_user_database.py',str(stage/'etc')],cwd=root,env=environment,check=True)
    shadow=(stage/'etc/shadow').read_text();assert shadow=='root:!:::::::\noperator::::::::\n'
    assert all(len(line.split(':'))==9 for line in shadow.splitlines())
    archive=stage/'root.tar'
    subprocess.run(['python3','scripts/create_initramfs.py',str(stage),str(archive)],cwd=root,check=True)
    with tarfile.open(archive) as tar:
        for path,mode in [('etc/shadow',0o600),('etc/passwd',0o644),('etc/group',0o644),('bin/sudo',0o4755)]:
            member=tar.getmember(path);assert (member.uid,member.gid,member.mode)==(0,0,mode)
    bad=stage/'hash';bad.write_text('$6$unsupported\n')
    result=subprocess.run(['python3','scripts/stage_user_database.py',str(stage/'etc')],cwd=root,
        env={**environment,'FORTRESS_OPERATOR_HASH_FILE':str(bad)},capture_output=True)
    assert result.returncode!=0 and (stage/'etc/shadow').read_text()==shadow
print('PASS build-time live database: locked root, explicit passwordless operator, 9 fields, root-owned archive modes; unsupported hash rejected before staging')
