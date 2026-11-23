"""Append code fragments that the regional decomp did not split to a recompiler input ELF.

The US cartridge has one relocatable code fragment outside the decomp's coverage: ROM 0x15877A0, linked at
0x81500000 and loaded as fragment id 5 (the Game Boy emulator that GB Tower starts). Its header and cartridge
relocation table have the same layout as the decomp's fragments, so the section, a REL section built from the
cartridge relocations, and a section symbol are enough for N64Recomp. Function boundaries are returned as
manual_funcs entries.
"""
import collections
import struct

ELF_HEADER = ">16sHHIIIIIHHHHHH"
SHDR = ">IIIIIIIIII"
PHDR = ">IIIIIIII"
SHT_PROGBITS, SHT_SYMTAB, SHT_REL = 1, 2, 9
SHF_WRITE, SHF_ALLOC, SHF_EXEC = 1, 2, 4
STT_SECTION = 3
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


def _sext16(value):
    return value - 0x10000 if value & 0x8000 else value


class Fragment:
    """One cartridge fragment: raw section bytes, relocation table and discovered entry points."""

    # Routines that save the caller's $ra and leave the Game Boy core until a later frame (fragment 89).
    YIELD_ROUTINES = (0xB7B4,)

    def __init__(self, rom, start, vram, external=()):
        self.rom, self.start, self.vram = rom, start, vram
        self.external = tuple(external)
        insn, delay, magic = struct.unpack_from(">II8s", rom, start)
        if insn != 0x08000000 | (((vram + 0x20) >> 2) & 0x3FFFFFF) or delay or magic != b"FRAGMENT":
            raise ValueError(f"No fragment header at ROM {start:#x}")
        self.reloc_offset, self.file_size, self.memory_size = struct.unpack_from(">III", rom, start + 20)
        self.data = bytes(rom[start:start + self.file_size])
        count, = struct.unpack_from(">I", self.data, self.reloc_offset)
        raw = struct.unpack_from(f">{count}I", self.data, self.reloc_offset + 4)
        self.relocs = {}
        self.reloc_list = []
        for record in raw:
            if record & 0x80000000:
                raise ValueError("Cross-fragment relocations in an extra fragment are not supported")
            self.relocs[record & 0xFFFFFF] = (record >> 24) & 0x7F
            self.reloc_list.append((record & 0xFFFFFF, (record >> 24) & 0x7F))
        if self.reloc_offset + 4 + 4 * count > self.file_size:
            raise ValueError("Invalid relocation table")
        self.code_end = self._find_code_end()
        self.entry_points = self.targets()

    def word(self, offset):
        return struct.unpack_from(">I", self.data, offset)[0]

    def _find_code_end(self):
        """The text ends after the last instruction that carries a code relocation (HI16, LO16 or JAL)
        and the function return that follows it."""
        last = max(o for o, t in self.relocs.items() if t in (R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16))
        offset = last
        while self.word(offset) != 0x03E00008:
            offset += 4
        return offset + 8

    def targets(self):
        """Every address a relocated reference, call or function pointer can reach in the code."""
        base = self.vram
        found = collections.defaultdict(set)
        found[0x20].add("entry")
        for address in self.external:
            found[address - base].add("external")      # referenced by name from the game's resident code
        hi_offsets = {o for o, t in self.relocs.items() if t == R_MIPS_HI16}
        for offset in range(0x20, self.code_end, 4):
            word = self.word(offset)
            if word & 0xFC00003F == 9 and (word >> 11) & 31 != 0:
                found[offset + 8].add("continuation")      # the code after a jalr, which the callee may jump to
            if word >> 26 == 2 or word & 0xFC00003F == 8 or word == 0x10000000 or word >> 16 == 0x1000:
                # The instruction after an unconditional jump and its delay slot is a label: the core's
                # unrolled handlers advance a register by the block size to name the next one.
                if offset + 8 < self.code_end:
                    found[offset + 8].add("label")
            if word >> 26 in (2, 3):
                target = ((word & 0x3FFFFFF) << 2) | 0x80000000
                if base <= target < base + self.code_end:
                    found[target - base].add("call")
                    if word >> 26 == 3 and target - base in self.YIELD_ROUTINES:
                        found[offset + 8].add("continuation")   # the core resumes here on a later frame
            elif word >> 26 == 1 and (word >> 16) & 31 in (0x10, 0x11, 0x12, 0x13):      # bltzal, bgezal and likely forms
                target = offset + 4 + (_sext16(word & 0xFFFF) << 2)
                found[target].add("call")
        # The cartridge keeps each HI16 record in front of the LO16 that completes its address.
        hilo = {}
        pending = []
        for offset, kind in self.reloc_list:
            if kind == R_MIPS_HI16:
                pending.append(offset)
            elif kind == R_MIPS_LO16:
                if not pending:
                    raise ValueError(f"Unpaired LO16 relocation at {offset:#x}")
                high = (self.word(pending[0]) & 0xFFFF) << 16
                target = (high + _sext16(self.word(offset) & 0xFFFF)) & 0xFFFFFFFF
                for item in pending + [offset]:
                    hilo[item] = target
                pending = []
                if base <= target < base + self.code_end:
                    found[target - base].add("address")
        if pending:
            raise ValueError("Unpaired HI16 relocation")
        self.hilo = hilo
        self.jump_tables = self.find_jump_tables()
        in_table = set()
        for table, size in self.jump_tables.items():
            in_table.update(range(table, table + 4 * size, 4))
        for offset, kind in self.relocs.items():
            if kind == R_MIPS_32 and offset not in in_table:
                value = self.word(offset)
                if base <= value < base + self.code_end:
                    found[value - base].add("pointer")
        return found

    def find_jump_tables(self):
        """Switch tables: a `jr` through a register loaded from a relocated table. Returns offset -> entries."""
        tables = {}
        self.jump_sites = []
        for offset in range(0x20, self.code_end, 4):
            word = self.word(offset)
            if word & 0xFC00003F != 8 or (word >> 21) & 31 == 31:
                continue
            register = (word >> 21) & 31
            table = bound = None
            for previous in range(offset - 4, max(offset - 0x40, 0x1F), -4):
                candidate = self.word(previous)
                if table is None and candidate >> 26 == 0x23 and (candidate >> 16) & 31 == register \
                        and previous in self.hilo:
                    table = self.hilo[previous] - self.vram
                if table is not None and candidate >> 26 in (0xA, 0xB):      # slti / sltiu
                    bound = candidate & 0xFFFF
                    break
            if table is None:
                continue
            if bound is None:
                bound = 0
                while self.relocs.get(table + 4 * bound) == R_MIPS_32:
                    bound += 1
            tables[table] = max(tables.get(table, 0), bound)
            self.jump_sites.append((offset, table))
        ordered = sorted(tables)
        for index, table in enumerate(ordered):
            limit = (ordered[index + 1] - table) // 4 if index + 1 < len(ordered) else tables[table]
            run = 0
            while self.relocs.get(table + 4 * run) == R_MIPS_32:
                run += 1
            tables[table] = min(tables[table], limit, run)
        return tables

    def branch_target(self, offset):
        """Offset targeted by the branch or jump at `offset`, or None."""
        word = self.word(offset)
        op = word >> 26
        if op == 2:
            return (((word & 0x3FFFFFF) << 2) | 0x80000000) - self.vram
        if op in (1, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or (op == 0x11 and (word >> 21) & 31 == 8):
            return offset + 4 + (_sext16(word & 0xFFFF) << 2)
        return None

    def switch_spans(self):
        """Offsets that one switch routine must contain: its `jr` and every case label of its table."""
        spans = []
        for site, table in self.jump_sites:
            size = self.jump_tables.get(table, 0)
            labels = [self.word(table + 4 * i) - self.vram for i in range(size)]
            if labels:
                spans.append((min([site] + labels), max([site] + labels)))
        return spans

    def functions(self):
        hard = {"entry", "call", "pointer", "external", "address", "continuation"}
        spans = self.switch_spans()
        inside = lambda s: any(lo < s <= hi for lo, hi in spans)
        starts = {s for s, kinds in self.entry_points.items()
                  if s < self.code_end and (kinds & hard or not inside(s))}
        overlapping = set()
        while True:
            ordered = sorted(starts)
            added = False
            for index, start in enumerate(ordered):
                end = ordered[index + 1] if index + 1 < len(ordered) else self.code_end
                for offset in range(start, end, 4):
                    target = self.branch_target(offset)
                    if target is None or not 0x20 <= target < self.code_end or start <= target < end:
                        continue
                    if inside(target):
                        overlapping.add(target)     # also a case label: a second routine that overlaps the switch
                    elif target not in starts:
                        starts.add(target)          # a label shared by two routines becomes a routine of its own
                        added = True
            if not added:
                break
        ordered = sorted(starts)
        out = []
        for index, start in enumerate(ordered):
            end = ordered[index + 1] if index + 1 < len(ordered) else self.code_end
            out.append((start, end - start))
        for start in sorted(overlapping - starts):
            after = [s for s in ordered if s > start]
            out.append((start, (after[0] if after else self.code_end) - start))
        return sorted(out)

    def spin_loop_hooks(self, name):
        """[[patches.hook]] entries that yield inside tiny polling loops.

        The runtime schedules guest threads cooperatively, so a loop that only re-reads memory until another
        thread changes it never lets that thread run."""
        function_starts = [start for start, _ in self.functions()]
        hooks = []
        for offset in range(0x20, self.code_end, 4):
            word = self.word(offset)
            op = word >> 26
            target = self.branch_target(offset)
            if target is None or op == 2 or not 0 <= offset - target <= 0x14:
                continue
            # A polling loop only loads: any arithmetic would be a loop counter or pointer that advances.
            body = [self.word(o) for o in range(target, offset + 8, 4) if o != offset]
            if not all(w == 0 or 0x20 <= (w >> 26) <= 0x27 or (w >> 26) == 0xF for w in body):
                continue
            rs, rt = (word >> 21) & 31, (word >> 16) & 31
            reg = lambda n: "0" if n == 0 else f"ctx->r{n}"
            if op in (4, 0x14):
                condition = f"{reg(rs)} == {reg(rt)}"
            elif op in (5, 0x15):
                condition = f"{reg(rs)} != {reg(rt)}"
            elif op in (6, 0x16):
                condition = f"SIGNED({reg(rs)}) <= 0"
            elif op in (7, 0x17):
                condition = f"SIGNED({reg(rs)}) > 0"
            elif op == 1 and rt in (0, 2):
                condition = f"SIGNED({reg(rs)}) < 0"
            elif op == 1 and rt in (1, 3):
                condition = f"SIGNED({reg(rs)}) >= 0"
            else:
                continue
            owner = max(start for start in function_starts if start <= offset)
            hooks.append(f'[[patches.hook]]\nfunc = "func_{self.vram + owner:08X}"\n'
                         f'before_vram = {self.vram + offset:#x}\n'
                         f'text = "if ({condition}) yield_to_ready(rdram);"\n')
        return hooks

    def target_of(self, offset, kind):
        word = self.word(offset)
        if kind == R_MIPS_32:
            return word
        if kind == R_MIPS_26:
            return ((word & 0x3FFFFFF) << 2) | 0x80000000
        return self.hilo[offset]


def append_fragment(source, destination, fragment, name):
    """Write `destination`: `source` plus one section named .`name` for `fragment`.

    Returns manual_funcs lines for N64Recomp."""
    data = bytearray(source.read_bytes())
    (ident, e_type, machine, version, entry, phoff, shoff, flags, ehsize, phentsize,
     phnum, shentsize, shnum, shstrndx) = struct.unpack_from(ELF_HEADER, data, 0)
    sections = [list(struct.unpack_from(SHDR, data, shoff + i * shentsize)) for i in range(shnum)]
    segments = [list(struct.unpack_from(PHDR, data, phoff + i * phentsize)) for i in range(phnum)]
    symtab_index = next(i for i, s in enumerate(sections) if s[1] == SHT_SYMTAB)
    symtab = sections[symtab_index]
    shstr = sections[shstrndx]
    section_names = bytes(data[shstr[4]:shstr[4] + shstr[5]])

    def section_name(header):
        end = section_names.index(b"\0", header[0])
        return section_names[header[0]:end].decode()

    symbols = bytearray(data[symtab[4]:symtab[4] + symtab[5]])
    section_symbol = {}
    for i in range(symtab[5] // 16):
        _, value, _, info, _, shndx = struct.unpack_from(">IIIBBH", symbols, i * 16)
        if info & 0xF == STT_SECTION and shndx not in section_symbol:
            section_symbol[shndx] = i

    new_index = shnum
    base = fragment.vram
    # The new section symbol is appended to the end of the symbol table.
    new_symbol = len(symbols) // 16
    symbols += struct.pack(">IIIBBH", 0, base, 0, STT_SECTION, 0, new_index)

    entries = bytearray()
    for offset, kind in fragment.reloc_list:
        target = fragment.target_of(offset, kind)
        if not base <= target < base + fragment.memory_size:
            raise ValueError(f"Relocation at {offset:#x} targets {target:#x} outside the fragment")
        entries += struct.pack(">II", base + offset, new_symbol << 8 | kind)

    def align(position, boundary=0x10):
        return (position + boundary - 1) & ~(boundary - 1)

    position = align(len(data), 0x10000)
    data.extend(b"\0" * (position - len(data)))
    section_offset = len(data)
    data += fragment.data
    rel_offset = align(len(data), 4)
    data.extend(b"\0" * (rel_offset - len(data)))
    data += entries
    symtab_offset = align(len(data), 4)
    data.extend(b"\0" * (symtab_offset - len(data)))
    data += symbols
    new_names = section_names + f".{name}\0.rel.{name}\0".encode()
    name_offset = len(data)
    data += new_names
    section_start = len(section_names)
    rel_name = section_start + len(f".{name}\0")
    segment_offset = align(len(data), 4)
    data.extend(b"\0" * (segment_offset - len(data)))
    segments.append([1, section_offset, base, fragment.start, fragment.file_size, fragment.file_size, 7, 0x10000])
    for segment in segments:
        data += struct.pack(PHDR, *segment)
    header_offset = align(len(data), 4)
    data.extend(b"\0" * (header_offset - len(data)))

    sections[symtab_index][4], sections[symtab_index][5] = symtab_offset, len(symbols)
    sections[shstrndx][4], sections[shstrndx][5] = name_offset, len(new_names)
    sections.append([section_start, SHT_PROGBITS, SHF_WRITE | SHF_ALLOC | SHF_EXEC, base, section_offset,
                     fragment.file_size, 0, 0, 16, 0])
    sections.append([rel_name, SHT_REL, 0x40, 0, rel_offset, len(entries), symtab_index, new_index, 4, 8])
    for header in sections:
        data += struct.pack(SHDR, *header)
    struct.pack_into(ELF_HEADER, data, 0, ident, e_type, machine, version, entry, segment_offset, header_offset,
                     flags, ehsize, phentsize, len(segments), shentsize, len(sections), shstrndx)
    destination.write_bytes(data)
    lines = [f'    {{ name = "{name}_entry", section = ".{name}", vram = {base:#x}, size = 8 }},']
    for start, size in fragment.functions():
        lines.append(f'    {{ name = "func_{base + start:08X}", section = ".{name}", vram = {base + start:#x}, size = {size:#x} }},')
    return lines
