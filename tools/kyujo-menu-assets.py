"""Build the small standalone menu skin from the supplied US Stadium cartridge."""
from hashlib import sha1
from pathlib import Path
import struct
import sys

def yay0(data):
    magic, size, links, literals = struct.unpack_from(">4sIII", data)
    if magic != b"Yay0":
        raise ValueError("Invalid Yay0 header")
    pos, mask, bits = 16, 0, 0
    result = bytearray()
    while len(result) < size:
        if not bits:
            mask, = struct.unpack_from(">I", data, pos)
            pos += 4
            bits = 32
        if mask & 0x80000000:
            result.append(data[literals])
            literals += 1
        else:
            pair, = struct.unpack_from(">H", data, links)
            links += 2
            count = pair >> 12
            if not count:
                count = data[literals] + 16
                literals += 1
            distance = (pair & 4095) + 1
            for _ in range(count + 2):
                result.append(result[-distance])
        mask = (mask << 1) & 0xffffffff
        bits -= 1
    return result

def extract(rom, base):
    if rom[base:base+8] != b'PERS-SZP':
        raise ValueError("Unexpected menu asset")
    header, = struct.unpack_from(">I", rom, base + 8)
    return yay0(rom[base + header:])

def emit(out, name, blob):
    out.write(f'static const unsigned char {name}[] = {{\n')
    for off in range(0,len(blob),24):
        out.write(','.join(map(str, blob[off:off+24]))+',\n')
    out.write('};\n')

rom = Path(sys.argv[1]).read_bytes()
if sha1(rom).hexdigest() != 'ed7bef5a306f88c0a6e96b15e71fee2ef32058f3':
    sys.exit("Kyujo's menu skin requires the supplied US Stadium reference cartridge")
with Path(sys.argv[2]).open('w') as out:
    for index, name in [(2,'list_font'), (4,'title_font')]:
        offset, = struct.unpack_from(">I",rom,0x3ba1a0 + 16*index)
        emit(out,name,extract(rom,0x3ba190 + offset))
    emit(out,'glyph_table',rom[0x70250:0x70310])
    emit(out,'menu_tile',extract(rom,0x4eb0c0)[0xf780:0x10780])
