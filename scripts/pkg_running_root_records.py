"""Fresh regular-file installed fixture; binaries bind to measured bootstrap."""
import pathlib,tarfile
from pkg_pending_records import prepare
from pkg_enroll_fixture import command,FILES

def build(image,out,roles,kernel,bootstrap,root_version=3,catalog_revision=7):
    out=pathlib.Path(out);directory=out.parent/'running-payload';directory.mkdir()
    # The base runner has completed its measured bootstrap before fixture build.
    # Runner supplies this exact path through the wrapper, never a target path.
    archive=BUILD_BOOTSTRAP
    commands=['mkdir /bin','mkdir /home','mkdir /home/operator','mkdir /var/log','mkdir /mnt','mkdir /dev','mkdir /run','mkdir /tmp','mkdir /sbin',
              'set_inode_field /home/operator mode 040700','set_inode_field /home/operator uid 1000','set_inode_field /home/operator gid 1000','set_inode_field /tmp mode 041777']
    with tarfile.open(archive) as tar:
        for name in ('login','shell','sudo','pkg-boot'):
            member=tar.getmember('bin/'+name);path=directory/name;path.write_bytes(tar.extractfile(member).read());commands += [f'write {path} /bin/{name}',f'set_inode_field /bin/{name} mode {"0104755" if name=="sudo" else "0100755"}']
    proof=directory/'proof';proof.write_bytes(b'enrolled-root-proof\n');commands.append(f'write {proof} /installed-proof')
    script=directory/'commands';script.write_text('\n'.join(commands)+'\n');(directory/'seed.log').write_text(command(['debugfs','-w','-f',str(script),str(image)]))
    return prepare(image,out,roles,kernel,bootstrap,root_version=root_version,catalog_revision=catalog_revision)

BUILD_BOOTSTRAP=None
