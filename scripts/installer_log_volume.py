"""Fixed FAT16 log slot: metadata is immutable; only the 4096 file bytes change."""
import struct

SECTORS = 16384
DATA_LBA = 161
FILE_BYTES = 4096


def metadata():
    data = bytearray(DATA_LBA * 512)
    data[:3] = b'\xeb\x3c\x90'
    data[3:11] = b'FOSLOG01'
    struct.pack_into('<HBHBHHBHHHII', data, 11, 512, 1, 1, 2, 512,
                     SECTORS, 0xf8, 64, 32, 64, 0, 0)
    data[36:39] = b'\x80\0\x29'
    struct.pack_into('<I', data, 39, 0x464f534c)
    data[43:54] = b'FORTRESSLOG'
    data[54:62] = b'FAT16   '
    data[510:512] = b'\x55\xaa'
    fat = bytearray(64 * 512)
    struct.pack_into('<HH', fat, 0, 0xfff8, 0xffff)
    for cluster in range(2, 10):
        struct.pack_into('<H', fat, cluster * 2, cluster + 1 if cluster < 9 else 0xffff)
    data[512:65*512] = fat
    data[65*512:129*512] = fat
    root = 129 * 512
    data[root:root+11] = b'FORTRESSLOG'
    data[root+11] = 8
    short = b'FORTRE~1LOG'
    checksum = 0
    for byte in short:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 255
    name = list('fortress-install.log'.encode('utf-16le')) + [0, 0]
    name += [255] * (52 - len(name))
    for index, ordinal in enumerate((2, 1)):
        at = root + (index + 1) * 32
        chunk = bytes(name[(ordinal-1)*26:ordinal*26])
        data[at] = ordinal | (0x40 if ordinal == 2 else 0)
        data[at+11:at+14] = bytes((15, 0, checksum))
        data[at+1:at+11] = chunk[:10]
        data[at+14:at+26] = chunk[10:22]
        data[at+28:at+32] = chunk[22:26]
    at = root + 96
    data[at:at+11] = short
    data[at+11] = 0x20
    struct.pack_into('<HI', data, at+26, 2, FILE_BYTES)
    return bytes(data)


def create(path):
    with path.open('xb') as stream:
        stream.truncate(SECTORS * 512)
        stream.write(metadata())
        stream.write(b'No installer session recorded yet.\n'.ljust(FILE_BYTES-1, b' ') + b'\n')
