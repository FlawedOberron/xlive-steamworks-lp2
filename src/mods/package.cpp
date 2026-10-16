#include "mods/package.h"

#include "third-party/puff.h"

#include <windows.h>

#include <string.h>

namespace xls::mods {

bool ReadWholeFile(const std::wstring& path, Bytes* out)
{
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	LARGE_INTEGER size{};
	bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (1LL << 31);
	if (ok) {
		out->resize((size_t)size.QuadPart);
		DWORD got = 0;
		ok = out->empty() || (ReadFile(file, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size());
	}
	CloseHandle(file);
	return ok;
}

bool WriteWholeFile(const std::wstring& path, const Bytes& data)
{
	for (size_t i = 3; i < path.size(); i++) {
		if (path[i] == L'\\' || path[i] == L'/') {
			CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
		}
	}
	std::wstring temp = path + L".tmp";
	HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	DWORD wrote = 0;
	bool ok = data.empty() || (WriteFile(file, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size());
	CloseHandle(file);
	return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

namespace {

uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void Put32(Bytes& b, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		b.push_back((uint8_t)(v >> (8 * i)));
	}
}

bool Inflate(const uint8_t* src, size_t srcSize, size_t outSize, Bytes* out)
{
	out->resize(outSize);
	unsigned long destLength = (unsigned long)outSize;
	unsigned long sourceLength = (unsigned long)srcSize;
	return puff(out->data(), &destLength, src, &sourceLength) == 0 && destLength == outSize;
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

}

bool Zip::Open(const std::wstring& path)
{
	m_entries.clear();
	if (!ReadWholeFile(path, &m_data) || m_data.size() < 22) {
		return false;
	}
	// End of central directory: last 'PK\5\6' within the final 64 KB + 22 bytes.
	size_t eocd = 0;
	bool found = false;
	for (size_t i = m_data.size() - 22 + 1; i-- > 0 && m_data.size() - i <= 65535 + 22;) {
		if (U32(&m_data[i]) == 0x06054b50) {
			eocd = i;
			found = true;
			break;
		}
	}
	if (!found) {
		return false;
	}
	uint32_t count = U16(&m_data[eocd + 10]);
	size_t at = U32(&m_data[eocd + 16]);
	for (uint32_t i = 0; i < count; i++) {
		if (at + 46 > m_data.size() || U32(&m_data[at]) != 0x02014b50) {
			return false;
		}
		const uint8_t* h = &m_data[at];
		ZipEntry e;
		e.method = U16(h + 10);
		e.compressedSize = U32(h + 20);
		e.size = U32(h + 24);
		uint16_t nameLength = U16(h + 28), extra = U16(h + 30), comment = U16(h + 32);
		e.localHeader = U32(h + 42);
		if (at + 46 + nameLength > m_data.size()) {
			return false;
		}
		e.name.assign((const char*)h + 46, nameLength);
		for (char& c : e.name) {
			c = c == '\\' ? '/' : c;
		}
		if (!e.name.empty() && e.name.back() != '/') {
			m_entries.push_back(e);
		}
		at += 46 + nameLength + extra + comment;
	}
	return true;
}

const ZipEntry* Zip::Find(const std::string& name) const
{
	for (const ZipEntry& e : m_entries) {
		if (e.name.size() == name.size() && _strnicmp(e.name.c_str(), name.c_str(), name.size()) == 0) {
			return &e;
		}
	}
	return nullptr;
}

bool Zip::Read(const ZipEntry& e, Bytes* out) const
{
	size_t h = e.localHeader;
	if (h + 30 > m_data.size() || U32(&m_data[h]) != 0x04034b50) {
		return false;
	}
	size_t data = h + 30 + U16(&m_data[h + 26]) + U16(&m_data[h + 28]);
	if (data + e.compressedSize > m_data.size()) {
		return false;
	}
	if (e.method == 0) {
		out->assign(m_data.begin() + data, m_data.begin() + data + e.size);
		return e.compressedSize == e.size;
	}
	return e.method == 8 && Inflate(&m_data[data], e.compressedSize, e.size, out);
}

bool ArcRead(const Bytes& f, std::vector<ArcEntry>* out)
{
	out->clear();
	if (f.size() < 8 || memcmp(f.data(), "ARC\0", 4) != 0 || U16(&f[4]) != 8) {
		return false;
	}
	uint32_t count = U16(&f[6]);
	for (uint32_t i = 0; i < count; i++) {
		size_t at = 8 + 80 * (size_t)i;
		if (at + 80 > f.size()) {
			return false;
		}
		const uint8_t* h = &f[at];
		ArcEntry e;
		e.name.assign((const char*)h, strnlen((const char*)h, 64));
		e.classHash = U32(h + 64);
		uint32_t stored = U32(h + 68), sizeField = U32(h + 72), offset = U32(h + 76);
		uint32_t size = sizeField & 0x1fffffff;
		e.flags = sizeField >> 29;
		if ((size_t)offset + stored > f.size()) {
			return false;
		}
		const uint8_t* src = &f[offset];
		if (stored != size || (stored >= 2 && src[0] == 0x78)) {
			// zlib stream: 2-byte header, raw deflate, adler32
			if (stored < 2 || !Inflate(src + 2, stored - 2, size, &e.data)) {
				return false;
			}
		}
		else {
			e.data.assign(src, src + stored);
		}
		out->push_back(std::move(e));
	}
	return true;
}

Bytes ArcWrite(const std::vector<ArcEntry>& entries)
{
	std::vector<Bytes> streams;
	for (const ArcEntry& e : entries) {
		Bytes z = {0x78, 0x01};
		size_t at = 0;
		do {
			size_t n = e.data.size() - at < 65535 ? e.data.size() - at : 65535;
			bool last = at + n == e.data.size();
			z.push_back(last ? 1 : 0);
			z.push_back((uint8_t)n);
			z.push_back((uint8_t)(n >> 8));
			z.push_back((uint8_t)~n);
			z.push_back((uint8_t)(~n >> 8));
			z.insert(z.end(), e.data.begin() + at, e.data.begin() + at + n);
			at += n;
		} while (at < e.data.size());
		uint32_t a = 1, b = 0;
		for (uint8_t c : e.data) {
			a = (a + c) % 65521;
			b = (b + a) % 65521;
		}
		uint32_t adler = b << 16 | a;
		for (int i = 3; i >= 0; i--) {
			z.push_back((uint8_t)(adler >> (8 * i)));
		}
		streams.push_back(std::move(z));
	}
	size_t head = 8 + 80 * entries.size();
	size_t offset = entries.empty() ? head : (head + 0x7fff) / 0x8000 * 0x8000;
	Bytes out = {'A', 'R', 'C', 0, 8, 0, (uint8_t)entries.size(), (uint8_t)(entries.size() >> 8)};
	for (size_t i = 0; i < entries.size(); i++) {
		char name[64] = {};
		strncpy(name, entries[i].name.c_str(), 63);
		out.insert(out.end(), name, name + 64);
		Put32(out, entries[i].classHash);
		Put32(out, (uint32_t)streams[i].size());
		Put32(out, (uint32_t)entries[i].data.size() | entries[i].flags << 29);
		Put32(out, (uint32_t)offset);
		offset += streams[i].size();
	}
	out.resize(entries.empty() ? head : (head + 0x7fff) / 0x8000 * 0x8000);
	for (const Bytes& z : streams) {
		out.insert(out.end(), z.begin(), z.end());
	}
	return out;
}

uint32_t ClassHash(const char* name)
{
	uint32_t crc = 0xffffffff;
	for (const char* p = name; *p; p++) {
		crc ^= (uint8_t)*p;
		for (int k = 0; k < 8; k++) {
			crc = crc & 1 ? (crc >> 1) ^ 0xedb88320 : crc >> 1;
		}
	}
	// zlib's crc32 is ~crc; the game stores crc32 ^ ~0, i.e. this register value.
	return crc & 0x7fffffff;
}

bool MessageSet(Bytes* file, uint32_t index, const std::string& text)
{
	Bytes& f = *file;
	if (f.size() < 12 || memcmp(f.data(), "MES\0", 4) != 0) {
		return false;
	}
	size_t at = 12 + (size_t)U32(&f[8]) + 1;
	Bytes out(f.begin(), f.begin() + at);
	bool changed = false;
	while (at + 8 <= f.size()) {
		uint32_t count = U32(&f[at + 4]);
		out.insert(out.end(), f.begin() + at, f.begin() + at + 8);
		at += 8;
		for (uint32_t i = 0; i < count; i++) {
			if (at + 4 > f.size()) {
				return false;
			}
			uint32_t length = U32(&f[at]);
			if (at + 4 + length + 1 > f.size()) {
				return false;
			}
			if (i == index) {
				Put32(out, (uint32_t)text.size());
				out.insert(out.end(), text.begin(), text.end());
				out.push_back(0);
				changed = true;
			}
			else {
				out.insert(out.end(), f.begin() + at, f.begin() + at + 4 + length + 1);
			}
			at += 4 + length + 1;
		}
	}
	if (changed) {
		f.swap(out);
	}
	return changed;
}

int Renumber(uint8_t* data, size_t size, int from, int to)
{
	static const char* const kPrefixes[] = {"area_online_", "rader_map_7_"};
	char fromDigits[2] = {(char)('0' + from / 10), (char)('0' + from % 10)};
	char toDigits[2] = {(char)('0' + to / 10), (char)('0' + to % 10)};
	int count = 0;
	for (const char* prefix : kPrefixes) {
		size_t n = strlen(prefix);
		for (size_t i = 0; i + n + 2 <= size; i++) {
			if (Lower((char)data[i]) != prefix[0]) {
				continue;
			}
			size_t k = 1;
			while (k < n && Lower((char)data[i + k]) == prefix[k]) {
				k++;
			}
			if (k == n && data[i + n] == (uint8_t)fromDigits[0] && data[i + n + 1] == (uint8_t)fromDigits[1]) {
				data[i + n] = (uint8_t)toDigits[0];
				data[i + n + 1] = (uint8_t)toDigits[1];
				count++;
				i += n + 1;
			}
		}
	}
	return count;
}

std::string RenumberName(const std::string& name, int from, int to)
{
	std::string out = name;
	Renumber((uint8_t*)&out[0], out.size(), from, to);
	return out;
}

}
