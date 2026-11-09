// The navmesh LP2's own bot AI walks on, for the stock online maps (stockarea.bin, made by lp2-tools/lp2stocknav.py
// --area). Online areas load no navmesh (only the training areas, aArea_TUTOREAL_*, name one in their rScrList), and
// without one cAIPlanMoveHmAction never moves a bot: they stood and shot. When the game opens aArea_ONLINE_NN.arc,
// packages.cpp hands it here and the game gets a copy with the navmesh list, way points and an empty intelligence box
// added and the area's rScrList naming them (types 29 / 30 / 31, as the training areas and map packages do). The
// rScrList change ships as COPY / ADD ops against the player's own file, so no game file is in the DLL; every stock
// entry is copied still compressed, so this holds about one archive's worth of memory, not its unpacked 50 MB.
#include "mods/lp2.h"
#include "mods/package.h"

#include "core/log.h"

#include "third-party/puff.h"

#include <windows.h>

#include <string.h>

#include <string>
#include <vector>

#ifndef XLS_STOCKAREA_BIN
#error "XLS_STOCKAREA_BIN (CMakeLists.txt) names src/mods/stockarea.bin"
#endif

asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".globl _xls_stockarea_begin\n"
    "_xls_stockarea_begin:\n"
    ".incbin \"" XLS_STOCKAREA_BIN "\"\n"
    ".globl _xls_stockarea_end\n"
    "_xls_stockarea_end:\n"
    ".text\n");
extern "C" const uint8_t xls_stockarea_begin[], xls_stockarea_end[];

namespace xls::mods {

namespace {

uint32_t Crc32(const uint8_t* p, size_t n)
{
	uint32_t crc = ~0u;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++) {
			crc = crc >> 1 ^ (0xedb88320u & (0u - (crc & 1)));
		}
	}
	return ~crc;
}

struct Cursor {
	const uint8_t* p;
	const uint8_t* end;
	bool ok = true;
	const uint8_t* take(size_t n)
	{
		if (!ok || (size_t)(end - p) < n) {
			ok = false;
			return nullptr;
		}
		const uint8_t* at = p;
		p += n;
		return at;
	}
	uint32_t u(int bytes)
	{
		const uint8_t* q = take(bytes);
		uint32_t v = 0;
		for (int i = 0; q && i < bytes; i++) {
			v |= (uint32_t)q[i] << (8 * i);
		}
		return v;
	}
};

struct Added {
	std::string name, cls;
	int kind; // 0 new (raw deflate), 1 ops against the stock entry of that name
	uint32_t size, crc;
	const uint8_t* data;
	uint32_t length;
};

// map's entries in the blob
bool Find(int map, std::vector<Added>* out)
{
	Cursor c{xls_stockarea_begin, xls_stockarea_end};
	const uint8_t* magic = c.take(4);
	if (!magic || memcmp(magic, "SAR1", 4) != 0) {
		return false;
	}
	uint32_t maps = c.u(4);
	for (uint32_t m = 0; c.ok && m < maps; m++) {
		int id = (int)c.u(4);
		uint32_t n = c.u(4);
		for (uint32_t i = 0; c.ok && i < n; i++) {
			Added a;
			uint32_t len = c.u(2);
			const uint8_t* s = c.take(len);
			a.name.assign(s ? (const char*)s : "", s ? len : 0);
			len = c.u(2);
			s = c.take(len);
			a.cls.assign(s ? (const char*)s : "", s ? len : 0);
			a.kind = (int)c.u(1);
			a.size = c.u(4);
			a.crc = c.u(4);
			a.length = c.u(4);
			a.data = c.take(a.length);
			if (id == map && c.ok) {
				out->push_back(a);
			}
		}
		if (id == map) {
			return c.ok && !out->empty();
		}
	}
	return false;
}

bool Ops(const Bytes& stock, const Added& a, Bytes* out)
{
	Cursor c{a.data, a.data + a.length};
	auto varint = [&](uint32_t* v) {
		*v = 0;
		for (int shift = 0; shift < 35; shift += 7) {
			const uint8_t* b = c.take(1);
			if (!b) {
				return false;
			}
			*v |= (uint32_t)(*b & 0x7f) << shift;
			if (*b < 0x80) {
				return true;
			}
		}
		return false;
	};
	out->clear();
	out->reserve(a.size);
	while (c.p < c.end) {
		uint32_t n, at;
		if (!varint(&n)) {
			return false;
		}
		uint32_t length = n >> 1;
		if (n & 1) {
			if (!varint(&at) || at > stock.size() || length > stock.size() - at) {
				return false;
			}
			out->insert(out->end(), stock.begin() + at, stock.begin() + at + length);
		}
		else {
			const uint8_t* lit = c.take(length);
			if (!lit) {
				return false;
			}
			out->insert(out->end(), lit, lit + length);
		}
	}
	return out->size() == a.size && Crc32(out->data(), out->size()) == a.crc;
}

// data as a zlib stream of stored blocks (as ArcWrite does)
void Stored(const Bytes& data, Bytes* out)
{
	out->push_back(0x78);
	out->push_back(0x01);
	size_t at = 0;
	do {
		size_t n = data.size() - at < 65535 ? data.size() - at : 65535;
		const uint8_t block[5] = {(uint8_t)(at + n == data.size()), (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)~n, (uint8_t)(~n >> 8)};
		out->insert(out->end(), block, block + 5);
		out->insert(out->end(), data.begin() + at, data.begin() + at + n);
		at += n;
	} while (at < data.size());
	uint32_t a = 1, b = 0;
	for (uint8_t c : data) {
		a = (a + c) % 65521;
		b = (b + a) % 65521;
	}
	const uint8_t adler[4] = {(uint8_t)(b >> 8), (uint8_t)b, (uint8_t)(a >> 8), (uint8_t)a};
	out->insert(out->end(), adler, adler + 4);
}

void Put32(Bytes& b, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		b.push_back((uint8_t)(v >> (8 * i)));
	}
}

struct Row {
	char name[64];
	uint32_t hash, stored, sizeField;
	const uint8_t* src; // the stock stream, or
	Bytes own;          // ours
};

}

bool StockAreaHas(int map)
{
	std::vector<Added> added;
	return Find(map, &added);
}

bool StockAreaGenerate(int map, const std::wstring& source, const std::wstring& out)
{
	std::vector<Added> added;
	Bytes f;
	if (!Find(map, &added) || !ReadWholeFile(source, &f) || f.size() < 8 || memcmp(f.data(), "ARC\0", 4) != 0 ||
	    (f[4] | f[5] << 8) != 8) {
		return false;
	}
	uint32_t count = f[6] | f[7] << 8;
	std::vector<Row> rows;
	rows.reserve(count + added.size());
	int patched = 0;
	for (uint32_t i = 0; i < count; i++) {
		size_t at = 8 + 80 * (size_t)i;
		if (at + 80 > f.size()) {
			return false;
		}
		const uint8_t* h = &f[at];
		Row r;
		memcpy(r.name, h, 64);
		memcpy(&r.hash, h + 64, 4);
		memcpy(&r.stored, h + 68, 4);
		memcpy(&r.sizeField, h + 72, 4);
		uint32_t offset;
		memcpy(&offset, h + 76, 4);
		if ((size_t)offset + r.stored > f.size()) {
			return false;
		}
		r.src = &f[offset];
		std::string name(r.name, strnlen(r.name, 64));
		for (const Added& a : added) {
			if (a.kind != 1 || _stricmp(a.name.c_str(), name.c_str()) != 0 || r.hash != ClassHash(a.cls.c_str())) {
				continue;
			}
			uint32_t size = r.sizeField & 0x1fffffff;
			Bytes stock(size), mine;
			unsigned long got = size, packed = r.stored >= 2 ? r.stored - 2 : 0;
			if (r.stored < 2 || r.src[0] != 0x78 || puff(stock.data(), &got, r.src + 2, &packed) != 0 || got != size ||
			    !Ops(stock, a, &mine)) {
				XLS_LOG_WARN("mods: stock nav: map %d's %s isn't the one the patch was made for; no navmesh there.", map, name.c_str());
				return false;
			}
			Stored(mine, &r.own);
			r.src = nullptr;
			r.stored = (uint32_t)r.own.size();
			r.sizeField = (uint32_t)mine.size() | (r.sizeField & 0xe0000000u);
			patched++;
		}
		rows.push_back(std::move(r));
	}
	if (patched == 0) {
		return false;
	}
	for (const Added& a : added) {
		if (a.kind != 0) {
			continue;
		}
		Bytes raw(a.size);
		unsigned long size = a.size, packed = a.length;
		if (puff(raw.data(), &size, a.data, &packed) != 0 || size != a.size || Crc32(raw.data(), raw.size()) != a.crc) {
			return false;
		}
		Row r{};
		strncpy(r.name, a.name.c_str(), 63);
		r.hash = ClassHash(a.cls.c_str());
		Stored(raw, &r.own);
		r.src = nullptr;
		r.stored = (uint32_t)r.own.size();
		r.sizeField = a.size | 2u << 29;
		rows.push_back(std::move(r));
	}
	if (rows.size() > 0xffff) {
		return false;
	}
	Bytes o = {'A', 'R', 'C', 0, 8, 0, (uint8_t)rows.size(), (uint8_t)(rows.size() >> 8)};
	size_t offset = (8 + 80 * rows.size() + 0x7fff) / 0x8000 * 0x8000;
	for (const Row& r : rows) {
		o.insert(o.end(), r.name, r.name + 64);
		Put32(o, r.hash);
		Put32(o, r.stored);
		Put32(o, r.sizeField);
		Put32(o, (uint32_t)offset);
		offset += r.stored;
	}
	o.resize((8 + 80 * rows.size() + 0x7fff) / 0x8000 * 0x8000);
	o.reserve(offset);
	for (const Row& r : rows) {
		const uint8_t* p = r.src ? r.src : r.own.data();
		o.insert(o.end(), p, p + r.stored);
	}
	Bytes().swap(f);
	return WriteWholeFile(out, o);
}

}
