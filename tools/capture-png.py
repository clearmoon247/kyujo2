"""Convert a 24-bit SDL BMP capture to lossless PNG."""
import argparse
import struct
from pathlib import Path
import png

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("bmp", type=Path)
parser.add_argument("png", type=Path)
args = parser.parse_args()
data = args.bmp.read_bytes()
offset, = struct.unpack_from("<I", data, 10)
width, height, planes, depth, compression = struct.unpack_from("<iiHHI", data, 18)
if data[:2] != b"BM" or width < 1 or height <= 0 or (planes, depth, compression) != (1, 24, 0):
    raise ValueError("Expected a bottom-up 24-bit SDL BMP")
stride = (width * 3 + 3) & ~3
if len(data) < offset + stride * height:
    raise ValueError("Incomplete framebuffer capture")
def rows():
    for y in reversed(range(height)):
        start = offset + stride * y
        row = bytearray(data[start:start + width * 3])
        row[0::3], row[2::3] = row[2::3], row[0::3]
        yield row
with args.png.open("wb") as output:
    png.Writer(width, height, greyscale=False).write(output, rows())
