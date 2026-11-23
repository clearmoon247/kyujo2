#!/usr/bin/env bash
# Preserve section-relative symbols for N64Recomp. The ROM decomp's fallback
# linker assignments can override definitions supplied by actual object files.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
region=${1:-us}
case "$region" in
    us) cd "$root/source/pokestadiumgs-master" ;;
    jp) cd "${KYUJO2_JP_DECOMP:-$root/source/jp/pokestadiumgs-master}" ;;
    *) echo "Expected us or jp" >&2; exit 2 ;;
esac
out="$root/build/recomp-input/$region"
mkdir -p "$out"
# The decomp marks fragment 28's base as unknown. Its ROM header jumps to
# 0x8FE00020, the instruction at offset 0x20 in this fragment.
sed 's/\.fragment28 0x80000000 :/.fragment28 0x8FE00000 :/' "build/pokestadiumgs-$region.ld" > "$out/layout.ld"
ld_args=(-T "$out/layout.ld")
for name in hardware_regs undefined_syms unused_syms auto/undefined_syms_auto auto/undefined_funcs_auto; do
    input="build/linker_scripts/$region/$name.ld"
    case "$name" in
        undefined_syms|auto/*)
            target="$out/${name##*/}.ld"
            sed -E 's/^([[:alnum:]_]+) = (0x[[:xdigit:]]+);$/PROVIDE(\1 = \2);/' "$input" > "$target"
            ld_args+=(-T "$target") ;;
        *) ld_args+=(-T "$input") ;;
    esac
done
if [[ "$region" == jp ]]; then
    # The JP splitter also uses this absolute address in IPL3 and other fragments.
    # Its accidental function symbol on fragment 28 must not relocate those uses.
    ld_args+=(--defsym=func_80000000=0x80000000)
fi
mips-linux-gnu-ld --no-check-sections --accept-unknown-input-arch --emit-relocs \
    "${ld_args[@]}" -T build/linker_scripts/common_undef_syms.ld -Map "$out/game.map" build/lib/libultra.a -o "$out/game.elf"
mips-linux-gnu-objcopy -O binary --gap-fill=0xFF "$out/game.elf" "$out/game.z64"
cmp "$out/game.z64" "baseroms/$region/baserom.z64"
