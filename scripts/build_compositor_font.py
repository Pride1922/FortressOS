"""Bake DejaVu Sans into bounded ASCII alpha atlases (Pillow, host only).

Usage: python scripts/build_compositor_font.py /path/to/DejaVuSans.ttf
The checked-in atlases mean normal OS builds do not need Pillow or font files.
"""
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[1]
out = root / "user/libui/assets"
out.mkdir(parents=True, exist_ok=True)
ttf_path = sys.argv[1]

# 1. Body (14px, cell 16x18, line 18) - bit-for-bit matches existing ui-alpha.bin
font14 = ImageFont.truetype(ttf_path, 14)
atlas14 = bytearray()
advances14 = bytearray()
for code in range(32, 127):
    char = chr(code)
    cell = Image.new("L", (16, 18))
    ImageDraw.Draw(cell).text((1, -1), char, font=font14, fill=255)
    adv = round(font14.getlength(char)) + 1
    assert 1 <= adv <= 16
    atlas14.extend(cell.tobytes())
    advances14.append(adv)
(out / "ui-alpha.bin").write_bytes(atlas14)
(out / "ui-advances.bin").write_bytes(advances14)

# 2. Title (18px, cell 20x22, line 22)
font18 = ImageFont.truetype(ttf_path, 18)
atlas18 = bytearray()
advances18 = bytearray()
for code in range(32, 127):
    char = chr(code)
    cell = Image.new("L", (20, 22))
    ImageDraw.Draw(cell).text((1, -1), char, font=font18, fill=255)
    adv = round(font18.getlength(char)) + 1
    assert 1 <= adv <= 20
    atlas18.extend(cell.tobytes())
    advances18.append(adv)
(out / "ui-title-alpha.bin").write_bytes(atlas18)
(out / "ui-title-advances.bin").write_bytes(advances18)

# 3. Caption (12px, cell 14x16, line 16)
font12 = ImageFont.truetype(ttf_path, 12)
atlas12 = bytearray()
advances12 = bytearray()
for code in range(32, 127):
    char = chr(code)
    cell = Image.new("L", (14, 16))
    ImageDraw.Draw(cell).text((1, -1), char, font=font12, fill=255)
    adv = round(font12.getlength(char)) + 1
    assert 1 <= adv <= 14
    atlas12.extend(cell.tobytes())
    advances12.append(adv)
(out / "ui-caption-alpha.bin").write_bytes(atlas12)
(out / "ui-caption-advances.bin").write_bytes(advances12)
