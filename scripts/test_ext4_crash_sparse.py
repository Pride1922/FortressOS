"""Every frozen schedule in a small trace compared to the calibrated full model."""
from ext4_crash_model import Disk, Disconnected
from ext4_crash_sparse import SparseDisk
from ext4_crash_campaign import PROFILES
from test_ext4_crash_recovery import faults


def main():
    checked = 0
    for ss in (512, 4096):
        base = b'Z'*(8*ss)
        events, payload = [], b''
        for kind, lba in (('write',0),('write',2),('write',0),('write',3),('flush',0),
                          ('write',1),('write',4),('flush',0)):
            events.append({'index':len(events),'kind':kind,'lba':lba,'payload_offset':len(payload)})
            if kind=='write':payload += bytes([len(events)])*ss
        for profile in PROFILES:
            for _, fault, config in faults(events, ss, profile):
                full = Disk(base, ss, fault=fault, **config); sparse = SparseDisk(base, ss, **config)
                for event in events[:fault.event+1]:
                    data = payload[event['payload_offset']:event['payload_offset']+ss]
                    try:
                        if event['kind']=='write':full.write(event['lba'],data)
                        else:full.flush()
                    except Disconnected:pass
                    try:sparse.apply(event,payload,fault if event['index']==fault.event else None)
                    except Disconnected:pass
                assert sparse.read_stable(0,len(base))==full.stable,(ss,profile,fault)
                assert sparse.offline and full.offline
                checked += 1
    print(f'Sparse/full model equivalence PASS: {checked} faults, all six profiles, both sectors')


if __name__=='__main__':main()
