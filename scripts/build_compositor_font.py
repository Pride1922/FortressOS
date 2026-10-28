"""Bake DejaVu Sans into a bounded ASCII alpha atlas (Pillow, host only).

Usage: python scripts/build_compositor_font.py /path/to/DejaVuSans.ttf
The checked-in atlas means normal OS builds do not need Pillow or font files.
"""
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[1]
out = root / "user/compositor/assets"
out.mkdir(parents=True, exist_ok=True)
font = ImageFont.truetype(sys.argv[1], 14)
atlas = bytearray()
advances = bytearray()
for code in range(32, 127):
    char = chr(code)
    cell = Image.new("L", (16, 18))
    ImageDraw.Draw(cell).text((1, -1), char, font=font, fill=255)
    advance = round(font.getlength(char)) + 1
    assert 1 <= advance <= 16
    atlas.extend(cell.tobytes())
    advances.append(advance)
(out / "ui-alpha.bin").write_bytes(atlas)
(out / "ui-advances.bin").write_bytes(advances)
