#!/usr/bin/env python3
"""Compare the actual C TZif reader with Python's independent zoneinfo reader."""
import ctypes as C
from datetime import datetime, timezone
from pathlib import Path
import subprocess
import struct
import tempfile
import tarfile
from zoneinfo import ZoneInfo
import build_zoneinfo

ROOT=Path(__file__).resolve().parent.parent
class Result(C.Structure):
    _fields_=[('offset',C.c_int32),('daylight',C.c_bool),('abbreviation',C.c_char*32)]

def main():
    directory=ROOT/'build/zoneinfo'
    build_zoneinfo.build(directory)
    build_zoneinfo.pack(directory,ROOT/'build/zoneinfo.tzdb')
    # Exercise the production packager, including root-owned data and executable
    # modes, without touching the retained release archive.
    with tempfile.TemporaryDirectory(prefix='fortress-tz-packaging-') as temporary:
        stage=Path(temporary)/'tree';stage.mkdir()
        for name in ('bin','sbin','etc','docs','usr/share'):(stage/name).mkdir(parents=True,exist_ok=True)
        (stage/'bin/date').write_bytes(b'fixture')
        (stage/'usr/share/zoneinfo.tzdb').write_bytes((ROOT/'build/zoneinfo.tzdb').read_bytes())
        archive=Path(temporary)/'root.tar'
        subprocess.run(['python3',str(ROOT/'scripts/create_initramfs.py'),str(stage),str(archive)],check=True)
        with tarfile.open(archive) as packed:
            entry=packed.getmember('usr/share/zoneinfo.tzdb')
            assert entry.uid==entry.gid==0 and entry.mode==0o644
            assert packed.extractfile(entry).read()==(ROOT/'build/zoneinfo.tzdb').read_bytes()
            assert packed.getmember('bin/date').mode==0o755
            assert len(packed.getmembers())<510
    print('PASS production USTAR timezone packaging and ownership; installer entry limit unchanged')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Isrc/include','user/timezone.c','-o','build/timezone.so'],cwd=ROOT,check=True)
    lib=C.CDLL(str(ROOT/'build/timezone.so'))
    lib.tz_tzif.argtypes=[C.c_void_p,C.c_size_t,C.c_int64,C.POINTER(Result)]
    lib.tz_tzif.restype=C.c_bool
    count=0
    for path in sorted(directory.rglob('*')):
        if not path.is_file() or path.name in ('LICENSE','version'):continue
        data=path.read_bytes();assert len(data)<16384,path
        with path.open('rb') as file:zone=ZoneInfo.from_file(file)
        wire=C.create_string_buffer(data)
        instants=set()
        # Every explicit transition: immediately before, at and after the edge.
        counts=struct.unpack_from('>6I',data,20)
        first=44+counts[3]*5+counts[4]*6+counts[5]+counts[2]*8+counts[1]+counts[0]
        times=struct.unpack_from('>I',data,first+32)[0]
        for instant, in struct.iter_unpack('>q',data[first+44:first+44+times*8]):
            if 0<=instant<=4133980799:
                instants.update((instant-1,instant,instant+1))
        # Historical, post-2038, far-future POSIX footer and both hemispheres.
        for year in (1970,2000,2026,2038,2040,2100,2400):
            for month in (1,3,4,7,10,11,12):
                utc=datetime(year,month,15,12,tzinfo=timezone.utc)
                instants.add(int(utc.timestamp()))
        # Far-future footer boundary rules: sample every day across the year.
        future=int(datetime(2400,1,1,tzinfo=timezone.utc).timestamp())
        instants.update(future+day*86400 for day in range(366))
        for stamp in sorted(instants):
            utc=datetime.fromtimestamp(stamp,timezone.utc)
            expected=utc.astimezone(zone);actual=Result()
            assert lib.tz_tzif(wire,len(data),stamp,C.byref(actual)),(path,utc,'rejected')
            assert actual.offset==int(expected.utcoffset().total_seconds()),(path,utc,actual.offset,expected)
            assert actual.abbreviation.decode()==expected.tzname(),(path,utc,actual.abbreviation,expected)
            count+=1
    print(f'PASS {count} independent zoneinfo comparisons across all bundled zones')

if __name__=='__main__':main()
