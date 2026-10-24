"""Current IDLE endpoint native delivery; no installation or mutation verbs."""
import pathlib,struct,shutil
import test_pkg_boot_admission as runner
from pkg_current_boot_overlay import overlay
from pkg_current_records import payloads,advance
from pkg_enroll_fixture import BS,gpt
from pkg_enroll_records import H,sealed
from test_jbd2_replay_host import blocks

original_build=runner.build
original_boot=runner.boot

def build_current(image,out,roles,kernel,bootstrap):
    artifact,executable=payloads(image,pathlib.Path(out).parent/'current-payloads')
    prepared=original_build(image,out,roles,kernel,bootstrap,artifact=artifact)
    parts,current=advance(prepared,artifact,executable)
    prepared['parts']=parts
    # Policy module is independent of measured bootstrap. The appended baseline
    # is authenticated by the selector, not adopted from current disk records.
    prepared['policy']+=struct.pack('<QQ',2,2)+prepared['bundle']
    prepared['current_bundle']=current
    return prepared

def boot_current(*args,**kwargs):return original_boot(*args,**kwargs,sequence=2)

def extra_cases(out,work,iso,source,prepared,run):
    root=(out/'oracle-root.img').read_bytes()
    exe_blocks=blocks(out/'oracle-root.img','/usr/bin/demo')
    artifact_blocks=blocks(out/'oracle-root.img','/pkg/artifact.fpk')
    for label,block in (('current-corrupt-executable',exe_blocks[0]),('current-corrupt-artifact',artifact_blocks[0])):
        bad=bytearray(root);bad[block*BS]^=1;target=out/(label+'.img');target.write_bytes(gpt(bad))
        yield boot_current(out,work,iso,target,'bios',1,label,True)
    baseline=[];bundle=prepared['bundle'];at=96
    for i in range(8):
        n=struct.unpack_from('<I',bundle,32+8*i)[0];baseline.append(bundle[at:at+n]);at+=n
    def put_records(data,parts):
        for part,index in zip(parts,(16,0,1,2,5,4,6,7)):
            at=0
            for lo,n,physical in prepared['identities'][index]['extents']:
                take=min(n*BS,len(part)-at);data[physical*BS:physical*BS+take]=part[at:at+take];at+=take
            assert at==len(part)
    rollback=bytearray(root);put_records(rollback,baseline)
    target=out/'current-rollback.img';target.write_bytes(gpt(rollback))
    yield boot_current(out,work,iso,target,'bios',1,'current-rollback',True)
    forged=bytearray(root);forged[exe_blocks[0]*BS]^=1
    parts=list(prepared['parts']);size=struct.unpack_from('<Q',parts[6],184)[0]
    actual=b''.join(forged[b*BS:(b+1)*BS] for b in exe_blocks)[:size]
    state=bytearray(parts[6]);state[152:184]=H(actual);parts[6]=sealed(state)
    grant=bytearray(parts[2]);grant[168:232]=H(parts[6])*2;parts[2]=sealed(grant)
    account=bytearray(parts[4]);account[448:480]=H(parts[2]);account[512:544]=H(parts[6]);parts[4]=sealed(account)
    put_records(forged,parts);target=out/'current-forged-state.img';target.write_bytes(gpt(forged))
    yield boot_current(out,work,iso,target,'bios',1,'current-forged-state',True)
    wrong=out/'current-mode.root';shutil.copyfile(out/'oracle-root.img',wrong)
    run(['debugfs','-w','-R','set_inode_field /usr/bin/demo mode 0104755',str(wrong)],'current-setid-fixture')
    target=out/'current-setid.img';target.write_bytes(gpt(wrong.read_bytes()))
    yield boot_current(out,work,iso,target,'bios',1,'current-setid',True)

runner.overlay=overlay
runner.build=build_current
runner.boot=boot_current
runner.extra_cases=extra_cases
if __name__=='__main__':runner.main()
