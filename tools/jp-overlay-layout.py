"""Prepare the JP sequel's overlay assembly and data split for its native link."""
import argparse
from pathlib import Path
import struct
import yaml

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path, help="Japanese decompilation working tree")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
ends = {}
for line in (root / "recomp/jp-code-ends.txt").read_text().splitlines():
    if line and not line.startswith("#"):
        name, offset = line.split()
        ends[name] = int(offset, 16)
config = yaml.safe_load("".join((args.source / f"yamls/jp/{file}.yaml").read_text() for file in ("header", "rom")))
rom = (args.source / "baseroms/jp/baserom.z64").read_bytes()
for segment in config["segments"]:
    if not isinstance(segment, dict) or not segment["name"].startswith("fragment"):
        continue
    name = segment["name"]
    number = int(name.removeprefix("fragment"))
    start = segment["start"]
    code = start + 32
    end = ends.pop(name)
    relocation, length = struct.unpack_from(">II", rom, start + 20)
    if not (code < end <= start + relocation < start + length):
        raise ValueError(f"Invalid code or relocation extent for {name}")
    prefix = f"fragments/{number}/{name}"
    parts = [[start, "textbin", prefix + "_header"]]
    if number == 26:
        parts.append([code, "textbin", prefix + "_veneers"])
        code += 0x160
    parts.extend([[code, "asm", prefix + "_code"], [end, "data", prefix + "_data"]])
    segment["subsegments"] = parts
    if number == 28:
        segment["vram"] = 0x8fe00000
    if number == 88:
        asset_start = start + length
if ends:
    raise ValueError(f"Unmapped overlay boundaries: {sorted(ends)}")
config["segments"].insert(-1, [asset_start, "bin", "cartridge_assets"])
config["options"]["asm_emit_size_directive"] = True
(args.source / "pokestadiumgs-jp.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
