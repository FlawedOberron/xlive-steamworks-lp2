// Bot zones for LP2's own online maps (stocknav_data.h, made by lp2-tools/lp2stocknav.py from the stock collision).
// Online areas have no navmesh of their own (only the training areas do), so without these bots on stock maps only had
// areas from the start points and stood still. Map packages bring their own (map.json "nav_zones"). Used in every
// online-area match, System Link or online alike: gravity.cpp hands them to the bots at area load.
#include "mods/lp2.h"
#include "mods/packages.h"
#include "mods/stocknav_data.h"

#include "core/log.h"

#include "third-party/puff.h"

#include <string.h>

#include <map>
#include <string>
#include <vector>

namespace xls::mods {

namespace {

struct Reader {
	const uint8_t* p;
	const uint8_t* end;
	bool ok = true;
	template <typename T>
	T get()
	{
		T v{};
		if (p + sizeof(T) > end) {
			ok = false;
			return v;
		}
		memcpy(&v, p, sizeof(T));
		p += sizeof(T);
		return v;
	}
};

bool Decode(const StockNavBlob& b, NavZones* out)
{
	std::vector<uint8_t> raw(b.size);
	unsigned long size = b.size, packed = b.packed;
	if (puff(raw.data(), &size, b.data, &packed) != 0 || size != b.size) {
		return false;
	}
	Reader r{raw.data(), raw.data() + raw.size()};
	uint32_t zones = r.get<uint32_t>();
	for (uint32_t i = 0; r.ok && i < zones; i++) {
		NavZone z;
		z.name = "z" + std::to_string(i);
		for (float& f : z.pos) {
			f = r.get<float>();
		}
		for (float& f : z.size) {
			f = r.get<float>();
		}
		uint16_t loops = r.get<uint16_t>();
		for (uint16_t l = 0; r.ok && l < loops; l++) {
			uint16_t n = r.get<uint16_t>();
			std::vector<std::array<float, 3>> pts(n);
			for (auto& q : pts) {
				for (float& f : q) {
					f = r.get<float>();
				}
			}
			z.outline.push_back(std::move(pts));
		}
		out->zones.push_back(std::move(z));
	}
	uint32_t links = r.get<uint32_t>();
	for (uint32_t i = 0; r.ok && i < links; i++) {
		NavLink l;
		l.from = r.get<uint16_t>();
		l.to = r.get<uint16_t>();
		l.kind = r.get<uint8_t>();
		l.length = r.get<float>();
		for (float& f : l.a) {
			f = r.get<float>();
		}
		for (float& f : l.b) {
			f = r.get<float>();
		}
		if (l.from < (int)zones && l.to < (int)zones && l.kind < kNavKinds) {
			out->links.push_back(l);
		}
	}
	return r.ok;
}

std::map<int, NavZones> g_cache; // decoded maps (area load, game thread)

}

bool StockNavZones(int map, NavZones* out)
{
	auto hit = g_cache.find(map);
	if (hit == g_cache.end()) {
		for (const StockNavBlob& b : kStockNav) {
			if (b.map != map) {
				continue;
			}
			NavZones nav;
			if (Decode(b, &nav)) {
				hit = g_cache.emplace(map, std::move(nav)).first;
			}
			else {
				XLS_LOG_WARN("mods: stock nav: map %d's zones didn't decode, bots use the start points there.", map);
			}
			break;
		}
	}
	if (hit == g_cache.end()) {
		return false;
	}
	*out = hit->second;
	return true;
}

}
