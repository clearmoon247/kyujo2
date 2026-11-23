"""Recompile a verified regional ELF, including each fragment's jump at offset zero."""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import yaml
from spimdisasm.elf32 import Elf32File, Elf32SectionHeaderNumber, Elf32SymbolTableType
import importlib.util

_spec = importlib.util.spec_from_file_location("extra_fragments", Path(__file__).with_name("extra-fragments.py"))
extra_fragments = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(extra_fragments)
# Code fragments that the US decomp leaves in its asset area: (ROM offset, linked address, section name,
# addresses that the resident code references by name; undefined_syms_auto.ld lists them as D_815xxxxx).
EXTRA_FRAGMENTS = [(0x15877A0, 0x81500000, "fragment89", (0x81509440, 0x81509674, 0x815097B0))]
# The Game Boy core polls and acknowledges SP_STATUS signal 3, which its RSP program sets when it finishes. The
# host runs that program as an ordinary task, so these accesses go through the host instead of the register:
# (instruction address, replace the instruction with a nop, C text placed before it).
FRAGMENT89_SP_STATUS = [
    (0x8150B728, False, "ctx->r9 = stadium2_gb_sp_status(rdram);"),
    (0x8150B730, True, None),
    (0x8150B738, True, "stadium2_gb_sp_clear();"),
    (0x81508510, True, "ctx->r14 = stadium2_gb_sp_status(rdram);"),
    (0x81508520, True, "ctx->r24 = stadium2_gb_sp_status(rdram);"),
    (0x8150852C, True, None),
    (0x81508534, True, "stadium2_gb_sp_clear();"),
    (0x81509714, True, "stadium2_gb_sp_clear();"),
]


def english_overlay_elf(directory, rom, fragments):
    """Restore BSS relocation records that the decomp linker resolved as absolute."""
    data = bytearray((directory / "game.elf").read_bytes())
    elf = Elf32File(data)
    section_symbols = {s.shndx: i for i, s in enumerate(elf.symtab)
                       if s.stType == Elf32SymbolTableType.SECTION.value}
    # The guest loader selects the target fragment from bits 20..27 of a
    # linked pointer. Its BSS can extend beyond the file-backed ELF section.
    targets = {}
    for fragment in fragments:
        base = fragment["vram"]
        reloc_offset, _, memory_size = struct.unpack_from(">III", rom, fragment["start"] + 20)
        section = elf.progbitsExecute["." + fragment["name"]]
        index = elf.sectionHeaders.sections.index(section)
        targets[base] = (reloc_offset, memory_size, section_symbols[index])
    repaired = 0
    external = 0
    for fragment in fragments:
        name = "." + fragment["name"]
        base = fragment["vram"]
        start = fragment["start"]
        reloc_offset, file_size, _ = struct.unpack_from(">III", rom, start + 20)
        table = start + reloc_offset
        count, = struct.unpack_from(">I", rom, table)
        if reloc_offset + 4 + count * 4 > file_size:
            raise ValueError(f"Invalid cartridge relocation table in {name}")
        cartridge_relocs = set(struct.unpack_from(f">{count}I", rom, table + 4))
        rels = elf.relPerName.get(name)
        if rels is None:
            continue
        for i, rel in enumerate(rels):
            sym = elf.symtab[rel.rSym]
            if sym.shndx != Elf32SectionHeaderNumber.ABS.value:
                continue
            target_base = sym.value & 0xFFF00000
            target = targets.get(target_base)
            if target is None or not target[0] <= sym.value - target_base <= target[1]:
                continue
            record = (rel.rType << 24) | (rel.offset - base)
            if target_base != base:
                # The cartridge marks references into another fragment with
                # bit 31. Its loader resolves these through the target's base.
                if rel.rType not in (5, 6):
                    continue
                record |= 0x80000000
            if record in cartridge_relocs:
                struct.pack_into(">I", data, rels.offset + 8 * i + 4,
                                 target[2] << 8 | rel.rType)
                repaired += 1
                external += target_base != base
    out = directory / "game-overlay-bss.elf"
    out.write_bytes(data)
    print(f"Recovered {repaired} cartridge-confirmed English overlay BSS relocations "
          f"({external} cross-fragment)", flush=True)
    return out.name

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--region", choices=["us", "jp"], default="us")
parser.add_argument("--recompiler", type=Path, default=root / ".work/recompiler-build/N64Recomp")
parser.add_argument("--output", type=Path)
parser.add_argument("--decomp", type=Path, help="regional source tree that produced the ELF")
args = parser.parse_args()
region = args.region
output = args.output or root / "build/recompiled" / region
source = args.decomp or (root / ("source" if region == "us" else "source/jp") / "pokestadiumgs-master")
rom = (source / "baseroms" / region / "baserom.z64").read_bytes()
checksums = {"us": "d8343e69a7dc63b869cf6361d87cde64444281d3", "jp": "05682e60b13479ca1c54656e5a5b1ee6d099c1a4"}
if hashlib.sha1(rom).hexdigest() != checksums[region]:
    raise SystemExit("ROM checksum mismatch")
fragments = [s for s in yaml.safe_load((source / f"pokestadiumgs-{region}.yaml").read_text())["segments"]
             if isinstance(s, dict) and s["name"].startswith("fragment")]
directory = root / "build/recomp-input" / region
# libultra strips the symbol for the static callback passed by sprintf to _Printf.
printf_callback = 0x8007BE88 if region == 'us' else 0x8007D768
manual = [f'    {{ name = "proutSprintf", section = ".text", vram = {printf_callback:#x}, size = 0x24 }},']
for section in fragments:
    insn, delay, magic = struct.unpack_from(">II8s", rom, section["start"])
    if insn >> 26 != 2 or delay != 0 or magic != b"FRAGMENT":
        raise ValueError(f"Unexpected entry in {section['name']}")
    if section["name"] == "fragment28":
        if insn != 0x0BF80008:
            raise ValueError("Unexpected fragment 28 entry")
        section["vram"] = 0x8FE00000
    manual.append(f'    {{ name = "{section["name"]}_entry", section = ".{section["name"]}", '
                  f'vram = {section["vram"]:#x}, size = 8 }},')
    # Fragment 26 exposes jump veneers immediately after its 32-byte header.
    for entry, following in zip(section["subsegments"][1:], section["subsegments"][2:]):
        if entry[1] != "textbin":
            continue
        for addr in range(entry[0], following[0], 8):
            op, slot = struct.unpack_from(">II", rom, addr)
            if op >> 26 != 2 or slot != 0:
                raise ValueError(f"Invalid jump veneer at ROM {addr:#x}")
            pc = section["vram"] + addr - section["start"]
            manual.append(f'    {{ name = "veneer_{pc:08X}", section = ".{section["name"]}", vram = {pc:#x}, size = 8 }},')
elf_name = english_overlay_elf(directory, rom, fragments) if region == "us" else "game.elf"
extra_names = []
spin_hooks = []
if region == "us":
    for rom_offset, vram, name, external in EXTRA_FRAGMENTS:
        fragment = extra_fragments.Fragment(rom, rom_offset, vram, external)
        extended = directory / "game-extra-fragments.elf"
        manual.extend(extra_fragments.append_fragment(directory / elf_name, extended, fragment, name))
        elf_name = extended.name
        extra_names.append(name)
        spin_hooks.extend(fragment.spin_loop_hooks(name))
        starts = [vram + start for start, _ in fragment.functions()]
        for address, nop, text in FRAGMENT89_SP_STATUS:
            owner = max(start for start in starts if start <= address)
            if nop:
                spin_hooks.append(f'[[patches.instruction]]\nfunc = "func_{owner:08X}"\nvram = {address:#x}\nvalue = 0\n')
            if text:
                spin_hooks.append(f'[[patches.hook]]\nfunc = "func_{owner:08X}"\nbefore_vram = {address:#x}\ntext = "{text}"\n')
        print(f"Added {name}: {len(fragment.functions())} functions", flush=True)
# Code labels that the decomp calls data (glabel D_...) but other fragments use as function pointers: the tails of
# their enclosing routines. Lab Pokemon viewing calls D_88000E4C.
for address, name, size in (("81700DA0", "fragment18", 0x10), ("84162A30", "fragment79", 0x10), ("867068E4", "fragment55", 0x8),
                            ("86B0B600", "fragment61", 0x8), ("87E03018", "fragment63", 0x8), ("88000E4C", "fragment29", 0x8)):
    if region == "us":
        manual.append(f'    {{ name = "func_{address}", section = ".{name}", vram = 0x{address}, size = {size:#x} }},')
(directory / "fragments.txt").write_text("".join(f'.{s["name"]}\n' for s in fragments) + "".join(f".{n}\n" for n in extra_names))
template = (root / f"recomp/{region}.toml").read_text()
config = template.replace('[input]', '[input]\nmanual_funcs = [\n' + '\n'.join(manual) + '\n]')
config = config.replace(f'../build/recomp-input/{region}/game.elf', elf_name).replace(
    f'../build/recompiled/{region}', os.path.relpath(output.resolve(), directory))
if spin_hooks:
    config += "\n# Polling loops in the cartridge's Game Boy core wait for other guest threads.\n" + "\n".join(spin_hooks)
config = config.replace('[input]', '[input]\nrelocatable_sections_path = "fragments.txt"\nbss_section_suffix = "_bss"')
path = directory / f"{region}.toml"
path.write_text(config)
for stale in output.glob("funcs_*.c"):
    stale.unlink()      # a shorter run must not leave the previous run's units behind
subprocess.run([args.recompiler, path], check=True)
