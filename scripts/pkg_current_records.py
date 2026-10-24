"""Offline bounded demo endpoint fixture; no installation command or writer."""
import struct
from pkg_enroll_records import H,sealed
from pkg_enroll_fixture import command
from pkg_mount_vectors import vectors

def payloads(image,directory):
    vectors(directory)
    artifact=(directory/'artifact.fpk').read_bytes();exe=(directory/'executable.bin').read_bytes()
    commands=directory/'current.commands'
    commands.write_text(f'write {directory/"artifact.fpk"} /pkg/artifact.fpk\n'
        f'write {directory/"executable.bin"} /usr/bin/demo\n'
        'set_inode_field /pkg/artifact.fpk mode 0100600\nset_inode_field /pkg/artifact.fpk uid 0\n'
        'set_inode_field /pkg/artifact.fpk gid 0\nset_inode_field /pkg/artifact.fpk generation 900\n'
        'set_inode_field /usr/bin/demo mode 0100755\nset_inode_field /usr/bin/demo uid 0\n'
        'set_inode_field /usr/bin/demo gid 0\nset_inode_field /usr/bin/demo generation 901\n')
    (directory/'generation.log').write_text(command(['debugfs','-w','-f',str(commands),str(image)]))
    return artifact,exe

def advance(prepared,artifact,executable):
    parts=list(prepared['parts']);state=bytearray(parts[6]);struct.pack_into('<Q',state,24,2)
    state[80:112]=H(parts[6]);struct.pack_into('<Q',state,112,1);state[120:152]=H(artifact);state[152:184]=H(executable)
    struct.pack_into('<Q',state,184,len(executable));state[192]=4;state[193:197]=b'demo';state=sealed(state)
    owners=bytearray(parts[3]);struct.pack_into('<Q',owners,48,2);owners=sealed(owners)
    grant=bytearray(parts[2]);struct.pack_into('<Q',grant,24,2);grant[168:232]=H(state)*2
    grant[264:296]=H(owners);struct.pack_into('<QQ',grant,360,2,2);grant=sealed(grant)
    account=bytearray(parts[4]);struct.pack_into('<Q',account,440,2);account[448:480]=H(grant);account[512:544]=H(state);account=sealed(account)
    parts[2]=grant;parts[3]=owners;parts[4]=account;parts[6]=state
    return parts,prepared['bundle'][:96]+b''.join(parts)
