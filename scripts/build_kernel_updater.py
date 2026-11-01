#!/usr/bin/env python3
"""Compile a media-specific freestanding updater; never opens physical devices."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def build(candidate,predecessor,out):
    candidate=candidate.resolve();predecessor=predecessor.resolve();out=out.resolve()
    assert candidate.is_file() and predecessor.is_file() and not candidate.is_symlink()
    assert candidate.stat().st_size<4*1024*1024 and predecessor.stat().st_size==8*1024*1024
    out.mkdir(parents=True,exist_ok=False)
    (out/'candidate.bin').write_bytes(candidate.read_bytes())
    predecessor_hash=hashlib.sha256(predecessor.read_bytes()).hexdigest()
    (out/'binding.c').write_text('const char kernel_predecessor[]="'+predecessor_hash+'";\n')
    (out/'candidate.asm').write_text('bits 64\nsection .rodata\nglobal kernel_candidate,kernel_candidate_end\nkernel_candidate:\nincbin "'+str(out/'candidate.bin')+'"\nkernel_candidate_end:\nsection .note.GNU-stack noalloc noexec nowrite progbits\n')
    flags=['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-pie','-fno-stack-protector',
           '-mno-red-zone','-mno-mmx','-mno-sse','-mno-sse2','-ffunction-sections','-fdata-sections',
           '-fstack-usage','-fno-asynchronous-unwind-tables']
    for name in ('src/include','src/lib','src/kernel','src/fs','src/mm','src/drivers','src/arch/x86_64','user/tools'):
        flags+=['-I'+str(ROOT/name)]
    objects=[]
    for i,source in enumerate(('user/tools/kernel_update.c','src/lib/kernel_update.c','src/lib/install_fat.c',
                               'src/kernel/root_boot.c','src/fs/gpt.c','src/lib/crc32.c','src/lib/string.c',
                               'user/tools/digest.c','user/tools/common.c',str(out/'binding.c'))):
        obj=out/f'{i}.o';subprocess.run(flags+['-c',str(ROOT/source) if not source.startswith('/') else source,'-o',str(obj)],check=True);objects.append(obj)
    for source,defines,name in ((out/'candidate.asm',[],'candidate'),(ROOT/'user/tools/start.asm',['-DTOOL_ENTRY=kernel_update_main'],'start')):
        obj=out/(name+'.o');subprocess.run(['nasm','-f','elf64',*defines,str(source),'-o',str(obj)],check=True);objects.append(obj)
    elf=out/'kernel-updater.elf'
    subprocess.run(['ld','-m','elf_x86_64','-nostdlib','-static','--gc-sections','-z','noexecstack','-T',str(ROOT/'user/shell.ld'),*map(str,objects),'-o',str(elf)],check=True)
    # Immutable VFS exec snapshot uses ordinary kmalloc's 2 MiB batch cap.
    assert elf.stat().st_size<2*1024*1024-4096
    from create_installer_media import elf_load_image
    _,segments=elf_load_image(elf)
    assert sum(((va+memory+4095)//4096-va//4096) for flags,va,pa,size,memory,alignment,digest in segments)<=1024
    for path in out.glob('*.su'):
        for line in path.read_text().splitlines():
            fields=line.split('\t');assert int(fields[1])<=1024,(path,line)
    sources=['src/include/kernel_update.h','src/lib/kernel_update.c','src/include/install_fat.h','src/lib/install_fat.c',
             'src/kernel/root_boot.c','src/fs/gpt.c','src/lib/crc32.c','src/lib/string.c','user/tools/kernel_update.c',
             'user/tools/common.c','user/tools/digest.c','scripts/build_kernel_updater.py']
    (out/'manifest.json').write_text(json.dumps({'result':'COMPILE_PASS','predecessor_sha256':predecessor_hash,
        'candidate_sha256':hashlib.sha256(candidate.read_bytes()).hexdigest(),
        'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),
        'sources':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in sources}},indent=2)+'\n')
    return elf

def main():
    p=argparse.ArgumentParser();p.add_argument('--candidate',type=Path,required=True);p.add_argument('--predecessor',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    print(build(a.candidate,a.predecessor,a.out))
if __name__=='__main__':main()
