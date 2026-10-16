// File formats the mod loader reads and writes: .zip packages, MT Framework ARC archives and
// rFlashMessage text tables. Lost Planet 2 specific, used by packages.cpp.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace xls::mods {

using Bytes = std::vector<uint8_t>;

bool ReadWholeFile(const std::wstring& path, Bytes* out);
bool WriteWholeFile(const std::wstring& path, const Bytes& data); // creates parent folders

// Stored and deflated entries of a .zip (no zip64, no encryption).
struct ZipEntry {
	std::string name; // '/' separated, as stored
	uint32_t method;
	uint32_t compressedSize;
	uint32_t size;
	uint32_t localHeader;
};
class Zip {
public:
	bool Open(const std::wstring& path);
	const std::vector<ZipEntry>& Entries() const { return m_entries; }
	bool Read(const ZipEntry& entry, Bytes* out) const;
	const ZipEntry* Find(const std::string& name) const; // case-insensitive

private:
	Bytes m_data;
	std::vector<ZipEntry> m_entries;
};

// ARC v8: 'ARC\0' u16 8 u16 count, count x {char name[64], u32 class hash, u32 stored size,
// u32 size | flags << 29, u32 offset}, data from the next 0x8000 boundary, entries zlib streams.
struct ArcEntry {
	std::string name; // '\' separated
	uint32_t classHash;
	uint32_t flags; // top 3 bits of the size field
	Bytes data;     // uncompressed
};
bool ArcRead(const Bytes& file, std::vector<ArcEntry>* out);
// Writes every entry as a zlib stream of stored blocks, which the game's inflate reads like any other.
Bytes ArcWrite(const std::vector<ArcEntry>& entries);
uint32_t ClassHash(const char* className); // crc32 ^ ~0, low 31 bits

// rFlashMessage: 'MES\0' u32 12 u32 nameLength name\0, then blocks to the end of the file:
// u32 language, u32 count, count x {u32 length, UTF-8 text, \0}. Sets message index in every block.
bool MessageSet(Bytes* file, uint32_t index, const std::string& text);

// Replaces the two-digit map number after each prefix (any case), e.g. Area_ONLINE_00 -> _13,
// in place. Returns the number of replacements.
int Renumber(uint8_t* data, size_t size, int from, int to);
std::string RenumberName(const std::string& name, int from, int to);

}
