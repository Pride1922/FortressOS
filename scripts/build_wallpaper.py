#!/usr/bin/env python3
"""scripts/build_wallpaper.py - Convert PNG wallpaper into optimized Fortress Wallpaper format (FWP1)
"""

import sys
import struct
import zlib
from pathlib import Path

def convert_wallpaper(png_path: Path, out_path: Path):
    with open(png_path, 'rb') as f:
        sig = f.read(8)
        assert sig == b'\x89PNG\r\n\x1a\n', "Invalid PNG signature"
        w, h = 0, 0
        idat = b''
        while True:
            chunk = f.read(8)
            if not chunk:
                break
            length, ctype = struct.unpack('>I4s', chunk)
            data = f.read(length)
            f.read(4) # crc
            if ctype == b'IHDR':
                w, h = struct.unpack('>II', data[:8])
            elif ctype == b'IDAT':
                idat += data
            elif ctype == b'IEND':
                break

    decomp = zlib.decompress(idat)
    stride = 1 + w * 3

    # Scan for non-black bounding box
    min_x, max_x, min_y, max_y = w, 0, h, 0
    for y in range(h):
        off = y * stride + 1
        for x in range(w):
            r, g, b = decomp[off + x*3], decomp[off + x*3 + 1], decomp[off + x*3 + 2]
            if r > 10 or g > 10 or b > 10:
                min_x = min(min_x, x)
                max_x = max(max_x, x)
                min_y = min(min_y, y)
                max_y = max(max_y, y)

    if max_x < min_x:
        min_x, max_x, min_y, max_y = 0, w - 1, 0, h - 1

    crop_w = max_x - min_x + 1
    crop_h = max_y - min_y + 1
    bg_color = 0xff000000

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, 'wb') as out:
        # Header: magic(4), orig_w(4), orig_h(4), bg_color(4), crop_w(4), crop_h(4)
        out.write(b'FWP1')
        out.write(struct.pack('<IIIII', w, h, bg_color, crop_w, crop_h))

        # Write cropped pixels (ARGB little-endian u32: B, G, R, A)
        for y in range(min_y, max_y + 1):
            off = y * stride + 1
            for x in range(min_x, max_x + 1):
                r, g, b = decomp[off + x*3], decomp[off + x*3 + 1], decomp[off + x*3 + 2]
                val = 0xff000000 | (r << 16) | (g << 8) | b
                out.write(struct.pack('<I', val))

    print(f"[WALLPAPER] Converted {png_path} ({w}x{h}) -> {out_path} (crop {crop_w}x{crop_h}, {out_path.stat().st_size} bytes)")

if __name__ == '__main__':
    src = Path("assets/wallpapers/hello-world.png")
    dst = Path("build/initramfs/usr/share/wallpapers/hello-world.bin")
    if len(sys.argv) > 1:
        src = Path(sys.argv[1])
    if len(sys.argv) > 2:
        dst = Path(sys.argv[2])
    convert_wallpaper(src, dst)
