// GxtText: minimal SA GXT main-table reader for the Linux native track.
// Round 7 (R6e): proves menu strings come from real GXT bytes. Format spec
// is re-derived here from game_sa/Text/* (used as documentation ONLY, never
// linked): u16 version + u16 encoding (8 = 8-bit GxtChar, cf. Text.cpp
// CheckFileEncoding/GAME_ENCODING), then chunks {magic[4], size u32 LE}:
// TABL (12-byte records {name[8], offset u32 LE}, first record is MAIN),
// then the MAIN TKEY ({offset u32 LE into TDAT data, hash u32 LE} per entry,
// sorted by hash, cf. CKeyArray::Load/Search) and TDAT (raw NUL-terminated
// bytes, cf. CData::Load). Lookup hashes with the CRC-32/IEEE uppercase
// algorithm from game_sa/Core/KeyGen.h (copied constants, same polynomial
// 0xEDB88320) and binary-searches exactly like CKeyArray::BinarySearch.
// File access is OS_File* only; no game_sa/ linkage.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct GxtEntry {
    uint32_t hash = 0;   // CRC32-uppercase of the key (TKEY field 2)
    uint32_t offset = 0; // byte offset into TDAT data (TKEY field 1)
};

struct GxtTable {
    std::string file; // e.g. "text/american.gxt" (relative to game dir)
    uint16_t version = 0;
    std::vector<GxtEntry> entries; // sorted by hash (file order)
    std::vector<char> tdat;        // TDAT payload (NUL-terminated strings)
};

// Maps a --lang name to a GXT file under text/. english -> american.gxt
// (the 1.0 US English table; there is no english.gxt on disk).
bool GxtText_FileForLang(const char* lang, char* out, std::size_t outSize);

// Loads the MAIN table of text/<lang>.gxt relative to gameDir.
bool GxtText_Load(const char* gameDir, const char* lang, GxtTable& out, char* err,
                  std::size_t errSize);

// Finds `key` (case-insensitive, like CText::Get) in the MAIN table.
// Returns false when absent; no fallback string is invented.
bool GxtText_Find(const GxtTable& table, const char* key, std::string& textOut);

// CRC32-uppercase hash of a key (CKeyGen::GetUppercaseKey equivalent).
uint32_t GxtText_Hash(const char* key);
