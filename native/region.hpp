#pragma once
#include <cstdint>

#ifdef KYUJO2_JP
constexpr uint64_t cartridge_hash = 0x8669bf0e5b7d06e0ULL;
constexpr auto cartridge_id = u8"stadium2-jp";
constexpr auto cartridge_file = "stadium2-jp.z64";
constexpr auto cartridge_name = "POKEMON STADIUM G&S";
constexpr auto cartridge_title = "Pokémon Stadium Gold & Silver";
constexpr auto cartridge_prompt = "Kyujo2 | Drop the Pokémon Stadium Gold & Silver (Japan) ROM here";
constexpr uint32_t jpeg_ucode_address = 0x80086b50u;
#else
constexpr uint64_t cartridge_hash = 0x13de5f210aa8d502ULL;
constexpr auto cartridge_id = u8"stadium2-us";
constexpr auto cartridge_file = "stadium2-us.z64";
constexpr auto cartridge_name = "POKEMON STADIUM 2";
constexpr auto cartridge_title = "Pokémon Stadium 2";
constexpr auto cartridge_prompt = "Kyujo2 | Drop the Pokémon Stadium 2 (USA) ROM here";
constexpr uint32_t jpeg_ucode_address = 0x80085270u;
#endif
