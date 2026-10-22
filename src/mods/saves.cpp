#include "mods/lp2.h"
#include "mods/package.h"

#include "core/log.h"
#include "core/utils.h"

#include <windows.h>
#include <shlobj.h>

#include <string>
#include <string.h>

namespace xls::mods {

namespace {

// Lostplanet2Save-capcom: 0x20-byte header (SHA-1 of the plain body as five little-endian words, magic,
// section order index, 0x1000), then the 0x5000-byte body XOR 0x73. The body holds 10 sections in one
// of four orders picked at random per save (order table DX9 0x173b1b8); the reader checks the SHA-1,
// the magic and the order index. lp2-tools/lp2save.py has the same logic and the details.
const uint32_t kMagic = 0x77cec928;
const size_t kHeader = 0x20, kBody = 0x5000;
const uint8_t kKey = 0x73;
const uint8_t kOrders[4][10] = {{9, 3, 5, 7, 6, 2, 1, 8, 4, 0}, {9, 6, 8, 7, 5, 4, 1, 2, 0, 3},
                                {9, 4, 7, 5, 3, 8, 2, 0, 6, 1}, {9, 2, 4, 1, 5, 3, 8, 6, 0, 7}};
// Section sizes from the readers; 8 = its own first u32 (4 when 0), 9 is empty.
const uint32_t kSizes[10] = {0x50, 0x2cc, 0x20, 0x2e4, 0x84, 0x770, 0x10, 0x2f50, 0, 0};

uint32_t U32(const uint8_t* p)
{
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

// The order whose layout puts a valid section 7 (u32 0x74000, u32 sum of the record's words) in place.
int FindOrder(const uint8_t* plain)
{
	for (int idx = 0; idx < 4; idx++) {
		size_t at = 0;
		for (uint8_t sec : kOrders[idx]) {
			if (sec == 7) {
				if (at + 0x2f50 <= kBody && U32(plain + at) == 0x74000) {
					uint32_t sum = 0;
					for (size_t i = 16; i < 16 + 0x2e50 + 0xe8; i += 4) {
						sum += U32(plain + at + i);
					}
					if (sum == U32(plain + at + 4)) {
						return idx;
					}
				}
				break;
			}
			at += sec == 8 ? (U32(plain + at) > 4 ? U32(plain + at) : 4) : kSizes[sec];
			if (at + 4 > kBody) {
				break;
			}
		}
	}
	return -1;
}

void Sha1(const uint8_t* data, size_t size, uint32_t h[5])
{
	auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
	h[0] = 0x67452301; h[1] = 0xefcdab89; h[2] = 0x98badcfe; h[3] = 0x10325476; h[4] = 0xc3d2e1f0;
	std::vector<uint8_t> m(data, data + size);
	m.push_back(0x80);
	while (m.size() % 64 != 56) {
		m.push_back(0);
	}
	uint64_t bits = (uint64_t)size * 8;
	for (int i = 7; i >= 0; i--) {
		m.push_back((uint8_t)(bits >> (i * 8)));
	}
	for (size_t b = 0; b < m.size(); b += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++) {
			w[i] = (uint32_t)m[b + i * 4] << 24 | m[b + i * 4 + 1] << 16 | m[b + i * 4 + 2] << 8 | m[b + i * 4 + 3];
		}
		for (int i = 16; i < 80; i++) {
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}
		uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5a827999; }
			else if (i < 40) { f = bb ^ c ^ d; k = 0x6ed9eba1; }
			else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8f1bbcdc; }
			else { f = bb ^ c ^ d; k = 0xca62c1d6; }
			uint32_t t = rol(a, 5) + f + e + k + w[i];
			e = d; d = c; c = rol(bb, 30); bb = a; a = t;
		}
		h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
	}
}

}

bool SavesConvert(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
{
	if (!data || size < kHeader + kBody) {
		return false;
	}
	std::vector<uint8_t> plain(data + size - kBody, data + size);
	for (uint8_t& b : plain) {
		b ^= kKey;
	}
	uint32_t h[5];
	Sha1(plain.data(), kBody, h);
	if (size == kHeader + kBody && memcmp(data, h, 20) == 0 && U32(data + 0x14) == kMagic) {
		return false; // already the current format
	}
	int order = FindOrder(plain.data());
	if (order < 0) {
		return false;
	}
	out.assign(kHeader + kBody, 0);
	memcpy(out.data(), h, 20);
	uint32_t tail[3] = {kMagic, (uint32_t)order, 0x1000};
	memcpy(out.data() + 20, tail, 12);
	memcpy(out.data() + kHeader, data + size - kBody, kBody);
	XLS_LOG_INFO("mods: saves: converted a %u-byte save from another game version (section order %d).", (unsigned)size, order);
	return true;
}

// Under GFWL the save lived in the GFWL gamertag's folder; ours is named after the Steam persona,
// so a player switching over would start fresh. When our folder has no save yet, the newest save
// of any other folder is copied in (the original is left alone). The file is not bound to the profile.
const wchar_t kSave[] = L"Lostplanet2.Lostplanet2Save-capcom";

static void ImportNewest(const std::wstring& root, const std::wstring& mine, const std::string& gamertag)
{
	WIN32_FIND_DATAW find;
	HANDLE it = FindFirstFileW((root + L"*").c_str(), &find);
	if (it == INVALID_HANDLE_VALUE) {
		return;
	}
	std::wstring newest;
	FILETIME newestTime = {};
	do {
		std::wstring name = find.cFileName;
		if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == L"." || name == L".." || _wcsicmp(name.c_str(), Utf8ToWide(gamertag.c_str()).c_str()) == 0) {
			continue;
		}
		WIN32_FILE_ATTRIBUTE_DATA save;
		if (GetFileAttributesExW((root + name + L"\\" + kSave).c_str(), GetFileExInfoStandard, &save) && CompareFileTime(&save.ftLastWriteTime, &newestTime) > 0) {
			newest = name;
			newestTime = save.ftLastWriteTime;
		}
	} while (FindNextFileW(it, &find));
	FindClose(it);
	if (newest.empty()) {
		return;
	}
	CreateDirectoryW(mine.c_str(), nullptr);
	if (CopyFileW((root + newest + L"\\" + kSave).c_str(), (mine + L"\\" + kSave).c_str(), TRUE)) {
		XLS_LOG_INFO("mods: saves: imported the save of %s into %s (the original is kept).", WideToUtf8(newest.c_str()).c_str(), gamertag.c_str());
	} else {
		XLS_LOG_WARN("mods: saves: could not copy the save of %s into %s (%lu).", WideToUtf8(newest.c_str()).c_str(), gamertag.c_str(), GetLastError());
	}
}

// The game hands XLiveUnprotectData only the save's protected header (the body is read apart), so a
// GFWL-era file is converted on disk, before the game reads it.
static void ConvertFile(const std::wstring& path)
{
	Bytes raw;
	std::vector<uint8_t> out;
	if (!ReadWholeFile(path, &raw) || !SavesConvert(raw.data(), raw.size(), out)) {
		return;
	}
	std::wstring keep = path + L".original";
	if (GetFileAttributesW(keep.c_str()) == INVALID_FILE_ATTRIBUTES && !CopyFileW(path.c_str(), keep.c_str(), TRUE)) {
		XLS_LOG_WARN("mods: saves: could not back up %s (%lu); left as it was.", WideToUtf8(path.c_str()).c_str(), GetLastError());
		return;
	}
	if (WriteWholeFile(path, Bytes(out.begin(), out.end()))) {
		XLS_LOG_INFO("mods: saves: %s converted to this version's format (the original is kept as .original).", WideToUtf8(path.c_str()).c_str());
	}
}

void SavesImport(const std::string& gamertag)
{
	wchar_t docs[MAX_PATH];
	if (gamertag.empty() || FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
		return;
	}
	std::wstring root = std::wstring(docs) + L"\\CAPCOM\\LOST PLANET 2\\";
	std::wstring mine = root + Utf8ToWide(gamertag.c_str());
	if (GetFileAttributesW((mine + L"\\" + kSave).c_str()) == INVALID_FILE_ATTRIBUTES) {
		ImportNewest(root, mine, gamertag);
	}
	ConvertFile(mine + L"\\" + kSave);
}

}
