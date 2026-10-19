#include "mods/parts.h"

#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/memberdata.h"
#include "core/steam.h"
#include "core/utils.h"
#include "mods/package.h"

#include <windows.h>
#include <shlobj.h>

#include <wctype.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <map>
#include <set>
#include <mutex>

// Part sets come from the Blender exporter (lp2-tools/lp2_blender_export.py): a folder
// LPC_<id>\ with lpc.json {"id", "name", "faction", "parts": {"body": {"file", "template"}}}
// and the .rModel files, dropped into Parts\ as is or zipped. Zips are unpacked into
// ModCache\parts\<id>\ (refreshed when the zip changes); folders are used in place.

namespace xls::mods {

namespace {

const char* const kSlotNames[kPartSlotCount] = {"head", "body", "back", "legs"};
const char* const kFactions[] = {"RND", "FJ", "SPE", "NEV", "FF"}; // index = the game's faction byte
const char* const kCacheVersion = "lp2-parts 1";
const size_t kNameMaxBytes = 43;

std::once_flag g_once;
std::vector<PartSet> g_sets;

bool IsModel(const Bytes& b)
{
	return b.size() > 0x80 && memcmp(b.data(), "MOD\0", 4) == 0;
}

std::string ZipStamp(const std::wstring& zip)
{
	WIN32_FILE_ATTRIBUTE_DATA a{};
	GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &a);
	char buf[160];
	snprintf(buf, sizeof(buf), "%s size %lu:%lu time %lu:%lu", kCacheVersion, a.nFileSizeHigh, a.nFileSizeLow,
	         a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
	return buf;
}

// lpc.json -> set (models still unchecked); false with a logged reason when unusable.
bool ParseManifest(const Bytes& text, const std::wstring& source, PartSet* set)
{
	Json root;
	std::string error;
	if (!Json::Parse(std::string(text.begin(), text.end()), root, &error)) {
		XLS_LOG_WARN("mods: parts %ls: lpc.json: %s", source.c_str(), error.c_str());
		return false;
	}
	set->id = root["id"].GetString(std::string());
	set->name = root["name"].GetString(std::string());
	set->faction = root["faction"].GetString("ALL");
	set->source = source;
	bool idOk = !set->id.empty() && std::all_of(set->id.begin(), set->id.end(), [](char c) { return isalnum((unsigned char)c) != 0; });
	if (!idOk) {
		XLS_LOG_WARN("mods: parts %ls: \"id\" must be letters and digits.", source.c_str());
		return false;
	}
	if (set->name.empty() || set->name.size() > kNameMaxBytes) {
		XLS_LOG_WARN("mods: parts %ls: \"name\" must be 1-%zu bytes.", source.c_str(), kNameMaxBytes);
		return false;
	}
	// "colours": "ABD" or ["A", "B", "D"] (menu order); older "colour": "B" = that one only
	std::string letters;
	const Json& cs = root["colours"];
	if (cs.IsArray()) {
		for (size_t i = 0; i < cs.Size(); i++) {
			letters += cs.At(i).GetString("");
		}
	}
	else {
		letters = cs.GetString(root["colour"].GetString("A"));
	}
	for (char c : letters) {
		int v = toupper((unsigned char)c) - 'A';
		if (v >= 0 && v < 4 && std::find(set->colours.begin(), set->colours.end(), v) == set->colours.end()) {
			set->colours.push_back(v);
		}
	}
	if (set->colours.empty()) {
		set->colours.push_back(0);
	}
	set->colour = set->colours[0];
	set->factionIndex = -1;
	if (_stricmp(set->faction.c_str(), "ALL") != 0) {
		for (int i = 0; i < (int)(sizeof(kFactions) / sizeof(kFactions[0])); i++) {
			if (_stricmp(set->faction.c_str(), kFactions[i]) == 0) {
				set->factionIndex = i;
			}
		}
		if (set->factionIndex < 0) {
			XLS_LOG_WARN("mods: parts %ls: unknown faction \"%s\".", source.c_str(), set->faction.c_str());
			return false;
		}
	}
	return true;
}

// Set bundle: the files a set is made of (lpc.json, its models and textures; flat names), sorted
// by lower-case name: "LPCB" u32 count, then per file u16 name length, name, u32 size, data. What
// players send each other; its FNV-1a 64 (16 hex) names the set's content on every PC.
const size_t kMaxBundle = 64u << 20;

bool MakeBundle(const std::wstring& dir, const PartSet& set, Bytes* out)
{
	std::map<std::string, std::wstring> files; // lower-case name -> path
	auto add = [&](const std::wstring& path) {
		std::wstring leaf = path.substr(path.find_last_of(L"\\/") + 1);
		std::string name = WideToUtf8(leaf.c_str());
		std::string lower = name;
		for (char& c : lower) {
			c = (char)tolower((unsigned char)c);
		}
		files[lower] = path;
	};
	add(dir + L"lpc.json");
	for (int s = 0; s < kPartSlotCount; s++) {
		if (!set.models[s].empty()) {
			add(set.models[s]);
		}
		if (!set.chains[s].empty()) {
			add(set.chains[s]);
		}
		if (!set.chainCols[s].empty()) {
			add(set.chainCols[s]);
		}
		for (const auto& t : set.textures[s]) {
			add(t.second);
		}
	}
	out->assign({'L', 'P', 'C', 'B'});
	auto put32 = [&](uint32_t v) {
		for (int i = 0; i < 4; i++) {
			out->push_back((uint8_t)(v >> (i * 8)));
		}
	};
	put32((uint32_t)files.size());
	for (const auto& f : files) {
		Bytes data;
		if (!ReadWholeFile(f.second, &data) || out->size() + data.size() > kMaxBundle) {
			return false;
		}
		out->push_back((uint8_t)f.first.size());
		out->push_back((uint8_t)(f.first.size() >> 8));
		out->insert(out->end(), f.first.begin(), f.first.end());
		put32((uint32_t)data.size());
		out->insert(out->end(), data.begin(), data.end());
	}
	return true;
}

std::string BundleHash(const Bytes& bundle)
{
	uint64_t h = Fnv1a64(bundle.data(), bundle.size());
	char text[17];
	snprintf(text, sizeof(text), "%016llx", (unsigned long long)h);
	return text;
}

// Writes a bundle's files into dir (flat, plain names only).
bool UnpackBundle(const Bytes& b, const std::wstring& dir)
{
	if (b.size() < 8 || memcmp(b.data(), "LPCB", 4) != 0) {
		return false;
	}
	auto get32 = [&](size_t o) { return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; };
	uint32_t count = get32(4);
	size_t o = 8;
	SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
	for (uint32_t i = 0; i < count; i++) {
		if (o + 2 > b.size()) {
			return false;
		}
		size_t n = b[o] | b[o + 1] << 8;
		if (o + 2 + n + 4 > b.size()) {
			return false;
		}
		std::string name((const char*)&b[o + 2], n);
		uint32_t size = get32(o + 2 + n);
		o += 2 + n + 4;
		if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos || o + size > b.size()) {
			return false;
		}
		if (!WriteWholeFile(dir + Utf8ToWide(name.c_str()), Bytes(b.begin() + o, b.begin() + o + size))) {
			return false;
		}
		o += size;
	}
	return o == b.size();
}

void LoadFolder(const std::wstring& dir, const std::wstring& label, std::vector<PartSet>* into = nullptr)
{
	Bytes text;
	if (!ReadWholeFile(dir + L"lpc.json", &text)) {
		return; // not a part set
	}
	PartSet set;
	if (!ParseManifest(text, label, &set)) {
		return;
	}
	Json root;
	std::string error;
	Json::Parse(std::string(text.begin(), text.end()), root, &error);
	int count = 0;
	for (int s = 0; s < kPartSlotCount; s++) {
		const Json& part = root["parts"][kSlotNames[s]];
		if (part.IsNull()) {
			continue;
		}
		std::string file = part["file"].GetString(std::string(kSlotNames[s]) + ".rModel");
		if (file.find_first_of("/\\:") != std::string::npos) {
			XLS_LOG_WARN("mods: parts %ls: %s: file must be a plain name.", label.c_str(), kSlotNames[s]);
			continue;
		}
		std::wstring path = dir + Utf8ToWide(file.c_str());
		Bytes model;
		if (!ReadWholeFile(path, &model) || !IsModel(model)) {
			XLS_LOG_WARN("mods: parts %ls: %s: %s is missing or not an rModel.", label.c_str(), kSlotNames[s], file.c_str());
			continue;
		}
		set.models[s] = path;
		set.templates[s] = part["template"].GetString(std::string());
		std::string chain = part["chain"].GetString(std::string());
		if (!chain.empty()) {
			Bytes head;
			std::wstring cp = dir + Utf8ToWide(chain.c_str());
			if (chain.find_first_of("/\\:") != std::string::npos || !ReadWholeFile(cp, &head) || head.size() < 16 || memcmp(head.data(), "CHN\0", 4) != 0) {
				XLS_LOG_WARN("mods: parts %ls: %s: chain %s is missing or not an rChain (plain file name).", label.c_str(), kSlotNames[s], chain.c_str());
			}
			else {
				set.chains[s] = cp;
			}
		}
		std::string chainCol = part["chaincol"].GetString(std::string());
		if (!chainCol.empty()) {
			Bytes head;
			std::wstring cp = dir + Utf8ToWide(chainCol.c_str());
			if (chainCol.find_first_of("/\\:") != std::string::npos || !ReadWholeFile(cp, &head) || head.size() < 16 || memcmp(head.data(), "CCL\0", 4) != 0) {
				XLS_LOG_WARN("mods: parts %ls: %s: chaincol %s is missing or not an rChainCol (plain file name).", label.c_str(), kSlotNames[s], chainCol.c_str());
			}
			else {
				set.chainCols[s] = cp;
			}
		}
		for (const auto& tex : part["textures"].Items()) {
			std::string f = tex.second.GetString(std::string());
			if (f.empty() || f.find_first_of("/\\:") != std::string::npos) {
				XLS_LOG_WARN("mods: parts %ls: %s: texture %s must be a plain file name.", label.c_str(), kSlotNames[s], tex.first.c_str());
				continue;
			}
			std::wstring tp = dir + Utf8ToWide(f.c_str());
			Bytes head;
			if (!ReadWholeFile(tp, &head) || head.size() < 0x14 || memcmp(head.data(), "TEX\0", 4) != 0) {
				XLS_LOG_WARN("mods: parts %ls: %s: %s is missing or not an rTexture.", label.c_str(), kSlotNames[s], f.c_str());
				continue;
			}
			set.textures[s].push_back({tex.first, tp});
		}
		for (const auto& byColour : part["material_overrides"].Items()) {
			int c = byColour.first.size() == 1 ? toupper((unsigned char)byColour.first[0]) - 'A' : -1;
			if (c < 0 || c > 3) {
				XLS_LOG_WARN("mods: parts %ls: %s: material_overrides key \"%s\" is not A-D.", label.c_str(), kSlotNames[s], byColour.first.c_str());
				continue;
			}
			for (const auto& byMaterial : byColour.second.Items()) {
				int m = byMaterial.first == "*" ? -1 : atoi(byMaterial.first.c_str());
				for (const auto& byWord : byMaterial.second.Items()) {
					// named: spec_power (word 18, highlight size: low = broad sheen, high = pinpoint),
					// spec_colour (words 20-22, specular RGB multiplier; a number or [r, g, b]);
					// or a raw word index
					std::vector<std::pair<int, double>> words;
					const Json& v = byWord.second;
					if (byWord.first == "spec_power") {
						if (v.IsNumber()) {
							words.push_back({18, v.GetNumber(0)});
						}
					}
					else if (byWord.first == "spec_colour") {
						for (int i = 0; i < 3; i++) {
							const Json& e = v.IsArray() && v.Size() == 3 ? v.At(i) : v;
							if (e.IsNumber()) {
								words.push_back({20 + i, e.GetNumber(0)});
							}
						}
					}
					else if (!byWord.first.empty() && isdigit((unsigned char)byWord.first[0])) {
						if (v.IsNumber()) {
							words.push_back({atoi(byWord.first.c_str()), v.GetNumber(0)});
						}
					}
					else {
						XLS_LOG_WARN("mods: parts %ls: %s: unknown material setting \"%s\".", label.c_str(), kSlotNames[s], byWord.first.c_str());
					}
					for (const auto& w : words) {
						if (w.first < 0 || w.first >= 40) {
							continue;
						}
						MaterialOverride o;
						o.colour = c;
						o.material = m;
						o.word = w.first;
						o.value = (float)w.second;
						set.overrides[s].push_back(o);
					}
				}
			}
		}
		count++;
	}
	if (count == 0) {
		XLS_LOG_WARN("mods: parts %ls: no usable parts, skipped.", label.c_str());
		return;
	}
	std::vector<PartSet>* target = into ? into : &g_sets;
	for (const PartSet& other : *target) {
		if (_stricmp(other.id.c_str(), set.id.c_str()) == 0) {
			XLS_LOG_WARN("mods: parts %ls: id \"%s\" already used by %ls, skipped.", label.c_str(), set.id.c_str(), other.source.c_str());
			return;
		}
	}
	std::string slots;
	for (int s = 0; s < kPartSlotCount; s++) {
		if (!set.models[s].empty()) {
			slots += (slots.empty() ? "" : ", ") + std::string(kSlotNames[s]);
		}
	}
	Bytes bundle;
	if (MakeBundle(dir, set, &bundle)) {
		set.hash = BundleHash(bundle);
	}
	XLS_LOG_INFO("mods: part set \"%s\" (%s, %s, %s): %s.", set.name.c_str(), set.id.c_str(), set.faction.c_str(), set.hash.c_str(), slots.c_str());
	target->push_back(set);
}

void LoadZip(const std::wstring& file)
{
	std::wstring zipPath = Cfg().partsDir + file;
	Zip zip;
	if (!zip.Open(zipPath)) {
		XLS_LOG_WARN("mods: parts %ls: not a readable zip, skipped.", file.c_str());
		return;
	}
	// lpc.json at the top or one folder down (a zipped LPC_<id> folder)
	const ZipEntry* manifest = nullptr;
	std::string prefix;
	for (const ZipEntry& e : zip.Entries()) {
		size_t slash = e.name.find('/');
		std::string leaf = e.name.substr(e.name.find_last_of('/') + 1);
		if (_stricmp(leaf.c_str(), "lpc.json") == 0 && (slash == std::string::npos || slash == e.name.size() - 9)) {
			manifest = &e;
			prefix = e.name.substr(0, e.name.size() - 8);
			break;
		}
	}
	Bytes text;
	PartSet probe;
	if (!manifest || !zip.Read(*manifest, &text) || !ParseManifest(text, file, &probe)) {
		XLS_LOG_WARN("mods: parts %ls: no valid lpc.json, skipped.", file.c_str());
		return;
	}
	std::wstring dir = Cfg().modCacheDir + L"parts\\" + Utf8ToWide(probe.id.c_str()) + L"\\";
	std::string stamp = ZipStamp(zipPath);
	Bytes old;
	if (!(ReadWholeFile(dir + L"stamp.txt", &old) && std::string(old.begin(), old.end()) == stamp)) {
		int files = 0;
		for (const ZipEntry& e : zip.Entries()) {
			if (e.name.compare(0, prefix.size(), prefix) != 0 || e.name.back() == '/') {
				continue;
			}
			std::string rel = e.name.substr(prefix.size());
			if (rel.find('/') != std::string::npos || rel.find("..") != std::string::npos) {
				continue; // the set's files are flat
			}
			Bytes data;
			if (!zip.Read(e, &data) || !WriteWholeFile(dir + Utf8ToWide(rel.c_str()), data)) {
				XLS_LOG_WARN("mods: parts %ls: cannot unpack %s.", file.c_str(), e.name.c_str());
				return;
			}
			files++;
		}
		WriteWholeFile(dir + L"stamp.txt", Bytes(stamp.begin(), stamp.end()));
		XLS_LOG_INFO("mods: parts %ls: unpacked %d files.", file.c_str(), files);
	}
	LoadFolder(dir, file);
	if (!g_sets.empty() && g_sets.back().id == probe.id) {
		g_sets.back().source = zipPath;
	}
}

std::vector<std::wstring> List(const std::wstring& dir, bool folders)
{
	std::vector<std::wstring> out;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + (folders ? L"*" : L"*.zip")).c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return out;
	}
	do {
		bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (isDir == folders && wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
			out.push_back(fd.cFileName);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
	return out;
}

void Load()
{
	ConfigEnsureLoaded();
	for (const std::wstring& d : List(Cfg().partsDir, true)) {
		LoadFolder(Cfg().partsDir + d + L"\\", d);
	}
	for (const std::wstring& z : List(Cfg().partsDir, false)) {
		LoadZip(z);
	}
	XLS_LOG_INFO("mods: parts: %zu custom part set(s) registered.", g_sets.size());
}

// --- Generated DLC packages (the C++ side of lp2-tools/dlcpart.py).
//
// The game shows DLC part slots 20-69 one by one in each category's list and loads slot n of a
// category from <package>\Content\nativePC\arcXmlTmpPC\CustomParts\<pfx>2e<nn>.arc (also seen as
// 0e) plus the colour archive <pfx>2e<nn>00.arc, with the names in the package's sname.bin. Model
// paths inside are remapped to model\hm\hmf6\<pfx>f6<nn>\... (FUN_00f664b0), and DLC parts always
// wear colour A. A package per part keeps sname.bin under the 1024 bytes the game reads.

const char* const kPrefixes[kPartSlotCount] = {"hd", "bd", "bk", "lg"};
const int kSnameCategory[kPartSlotCount] = {1, 2, 3, 4};  // head, body, back, legs
const int kSnameFloats[kPartSlotCount] = {3, 3, 12, 0};
const int kCarrierItem = 69; // the carrier's default slot (saves from before the per-loadout aliases)
const int kAliasFirst = 20;  // carrier aliases: two DLC slots per faction (5) and loadout (2), 20-39
const int kAliasCount = 20;

bool IsCarrierItem(int item)
{
	return item == kCarrierItem || (item >= kAliasFirst && item < kAliasFirst + kAliasCount);
}

// The aliases a faction's loadout wears (save: part ids at game +0x50794 + (faction + loadout*5)*8).
// Two per loadout, worn in turn: a set switch then changes the archive name, so the game frees the
// old set's archive and opens the new one (same name: it hands the old one back).
int AliasItem(int faction, int loadout, int alt)
{
	return kAliasFirst + (((faction < 0 || faction > 4 ? 0 : faction) * 2 + (loadout & 1)) * 2) + (alt & 1);
}

// The slot a choice is kept under: a loadout's two aliases share one.
int ChoiceItem(int item)
{
	return item >= kAliasFirst && item < kAliasFirst + kAliasCount ? item & ~1 : item;
}
const char* const kGenVersion = "lp2-partpkg 14";

std::once_flag g_pkgOnce;
std::vector<PartPackage> g_packages;

void ReplaceAll(std::string* s, const std::string& from, const std::string& to)
{
	for (size_t at = s->find(from); at != std::string::npos; at = s->find(from, at + to.size())) {
		s->replace(at, from.size(), to);
	}
}

void ReplaceAll(Bytes* b, const std::string& from, const std::string& to)
{
	if (b->size() < from.size()) {
		return;
	}
	for (size_t i = 0; i + from.size() <= b->size(); i++) {
		if (memcmp(b->data() + i, from.data(), from.size()) == 0) {
			memcpy(b->data() + i, to.data(), to.size()); // same length: offsets stay valid
			i += from.size() - 1;
		}
	}
}

std::string Hex2(int v)
{
	char t[4];
	snprintf(t, sizeof(t), "%02x", v & 0xff);
	return t;
}

// Clones one template archive into a DLC one: renames (same length), the exported model in place
// of the template's, and (colour archives) the chosen colour texture renamed to colour A.
bool EndsWith(const std::string& s, const std::string& tail)
{
	return s.size() >= tail.size() && _stricmp(s.c_str() + s.size() - tail.size(), tail.c_str()) == 0;
}

bool CloneArc(const std::wstring& src, const std::wstring& dst, const std::vector<std::pair<std::string, std::string>>& pairs,
              const std::string& modelName, const Bytes* model, const std::string& colourFrom,
              const std::vector<std::pair<std::string, const Bytes*>>& textures = {}, const Bytes* chain = nullptr, const Bytes* chainCol = nullptr)
{
	Bytes file;
	std::vector<ArcEntry> entries;
	if (!ReadWholeFile(src, &file) || !ArcRead(file, &entries)) {
		return false;
	}
	// the set's own rChain replaces the template's beside the model (the game loads the chain from
	// the model's path, so the stand-in model must carry it, not just the lpc copy)
	auto own = [&](const char* cls, const Bytes* data) {
		if (!data) {
			return;
		}
		auto it = std::find_if(entries.begin(), entries.end(), [&](const ArcEntry& e) {
			return e.classHash == ClassHash(cls) && e.name == modelName;
		});
		if (it == entries.end()) {
			ArcEntry c{};
			c.name = modelName;
			c.classHash = ClassHash(cls);
			entries.push_back(c);
			it = entries.end() - 1;
		}
		it->data = *data;
	};
	own("rChain", chain);
	own("rChainCol", chainCol);
	for (ArcEntry& e : entries) {
		if (model && e.name == modelName && e.classHash == ClassHash("rModel")) {
			e.data = *model;
		}
		if (e.classHash == ClassHash("rTexture")) {
			for (const auto& t : textures) {
				if (EndsWith(e.name, t.first)) {
					e.data = *t.second; // custom texture in place of the template's (name kept)
				}
			}
		}
		for (const auto& p : pairs) {
			ReplaceAll(&e.name, p.first, p.second);
			ReplaceAll(&e.data, p.first, p.second);
		}
		if (!colourFrom.empty()) {
			ReplaceAll(&e.name, colourFrom, "_BM-00_A");
			ReplaceAll(&e.data, colourFrom, "_BM-00_A");
		}
	}
	return WriteWholeFile(dst, ArcWrite(entries));
}

std::string SnameBin(const std::vector<int>& slots, int item, const std::string& name)
{
	char line[16];
	snprintf(line, sizeof(line), "%02d\r\n", (int)slots.size());
	std::string t = std::string("\xef\xbb\xbf") + line;
	for (int slot : slots) {
		snprintf(line, sizeof(line), "%02d\r\n%02d\r\n", kSnameCategory[slot], item);
		t += line;
		for (int i = 0; i < 7; i++) {
			t += name + "\r\n";
		}
		for (int i = 0; i < kSnameFloats[slot]; i++) {
			t += "0\r\n";
		}
	}
	return t; // the game reads 1024 bytes: 4 entries of "Custom set" are about 340
}

std::string FileStamp(const std::wstring& path)
{
	WIN32_FILE_ATTRIBUTE_DATA a{};
	GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
	char buf[96];
	snprintf(buf, sizeof(buf), "%lu:%lu %lu:%lu", a.nFileSizeHigh, a.nFileSizeLow, a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
	return buf;
}


// The carrier route: the part's model and every texture it names under a path of our own
// (model\hm\lpc\<hash>\<pfx><hash>...), added to the slot's DLC archives (the part lookup only
// searches loaded archives). Texture names in the model sit in fixed 64-byte records, so they can
// be rewritten to any name that fits.
bool BuildLpcEntries(const std::string& base, const Bytes& modelIn, const std::vector<ArcEntry>& textures, const std::string& tmpl,
                     const std::string& colourName, std::vector<ArcEntry>* archive)
{
	Bytes model = modelIn;
	if (model.size() < 0x80) {
		return false;
	}
	uint32_t ntex = *(uint32_t*)(model.data() + 0x1C);
	uint32_t oTex = *(uint32_t*)(model.data() + 0x2C);
	if (oTex + ntex * 64 > model.size()) {
		return false;
	}
	for (uint32_t i = 0; i < ntex; i++) {
		char* rec = (char*)model.data() + oTex + i * 64;
		std::string name(rec, strnlen(rec, 64));
		const ArcEntry* tex = nullptr;
		for (const ArcEntry& e : textures) {
			if (_stricmp(e.name.c_str(), name.c_str()) == 0) {
				tex = &e;
			}
		}
		bool isColour = name.find("_BM-00") != std::string::npos && name.find(tmpl) != std::string::npos;
		if (isColour) {
			for (const ArcEntry& e : textures) {
				if (_stricmp(e.name.c_str(), colourName.c_str()) == 0) {
					tex = &e;
				}
			}
		}
		if (!tex) {
			continue; // not one of the part's own textures: keep the game's path
		}
		size_t at = name.rfind(tmpl);
		std::string tail = at == std::string::npos ? "_" + std::to_string(i) : name.substr(at + tmpl.size());
		if (isColour) {
			tail = "_BM-00_A";
		}
		std::string loose = base + tail;
		if (loose.size() >= 64) {
			return false;
		}
		memset(rec, 0, 64);
		memcpy(rec, loose.data(), loose.size());
		ArcEntry t = *tex;
		t.name = loose;
		archive->push_back(t);
	}
	// the colour map is also asked for on its own (kind 1): always provide it
	for (const ArcEntry& e : textures) {
		if (_stricmp(e.name.c_str(), colourName.c_str()) == 0) {
			ArcEntry c = e;
			c.name = base + "_BM-00_A";
			archive->push_back(c);
		}
	}
	ArcEntry m;
	m.name = base;
	m.classHash = ClassHash("rModel");
	m.flags = 0;
	m.data = model;
	archive->push_back(m);
	return true;
}

// Applies one colour's material overrides to a model copy (material records: 160 bytes each after
// the texture names; count u16 @0x0a, texture count u32 @0x1c, texture table @0x2c).
bool PatchMaterials(Bytes* model, const std::vector<MaterialOverride>& overrides, int colour)
{
	if (model->size() < 0x30) {
		return false;
	}
	uint16_t nmat = *(uint16_t*)(model->data() + 0x0a);
	uint32_t ntex = *(uint32_t*)(model->data() + 0x1c);
	uint32_t oTex = *(uint32_t*)(model->data() + 0x2c);
	size_t oMat = (size_t)oTex + (size_t)ntex * 64;
	if (oMat + (size_t)nmat * 160 > model->size()) {
		return false;
	}
	for (const MaterialOverride& o : overrides) {
		if (o.colour != colour || o.material >= nmat) {
			continue;
		}
		for (int m = 0; m < nmat; m++) {
			if (o.material == m || (o.material < 0 && (*(uint32_t*)(model->data() + oMat + m * 160) & 0xff) == 0x43)) {
				memcpy(model->data() + oMat + m * 160 + o.word * 4, &o.value, 4); // "*": lit materials only (type byte 0x43)
			}
		}
	}
	return true;
}

// carrierTok (carrier builds): the set's own 6-character stand-in for the slot's <pfx>0e45 / 2e45 /
// f645 names, so the effect provider, extra models and textures differ between the carrier's sets
// (the game caches resources by name). Same length as the names it replaces.
bool BuildPackage(const PartSet& set, int slot, int item, PartPackage* out, const std::string& carrierTok = std::string())
{
	bool carrierBuild = !carrierTok.empty();
	std::string tmpl = set.templates[slot];
	size_t dot = tmpl.find('.');
	if (dot != std::string::npos) {
		tmpl.resize(dot);
	}
	if (tmpl.size() != 6 || _strnicmp(tmpl.c_str(), kPrefixes[slot], 2) != 0) {
		XLS_LOG_WARN("mods: parts %s: %s: template \"%s\" is not a %s part.", set.id.c_str(), kSlotNames[slot], tmpl.c_str(), kPrefixes[slot]);
		return false;
	}
	for (char& c : tmpl) {
		c = (char)tolower((unsigned char)c);
	}
	std::string pfx = tmpl.substr(0, 2), sset = tmpl.substr(2, 2), svar = tmpl.substr(4, 2);
	char hash[16];
	std::string hkey = set.id + ":" + kSlotNames[slot];
	snprintf(hash, sizeof(hash), "%08x", Fnv1a32(hkey.data(), hkey.size()));
	std::string lpcBase = "model\\hm\\lpc\\" + std::string(hash) + "\\" + pfx + hash;
	std::wstring srcDir = ExeDirectory() + L"nativePC\\arcXmlTmpPC\\CustomParts\\";
	std::wstring root = Cfg().modCacheDir + L"parts\\pkg\\" + Utf8ToWide(set.id.c_str()) + L"_" + Utf8ToWide(kSlotNames[slot]) + (carrierBuild ? L"_c\\" : L"\\");
	std::wstring dstDir = root + L"Content\\nativePC\\arcXmlTmpPC\\CustomParts\\";
	char stampBuf[256];
	snprintf(stampBuf, sizeof(stampBuf), "%s item %d colour %d template %s model %s", kGenVersion, item, set.colour, tmpl.c_str(), FileStamp(set.models[slot]).c_str());
	std::string stamp = std::string(stampBuf) + (carrierBuild ? " token " + carrierTok : "");
	for (const auto& t : set.textures[slot]) {
		stamp += " " + t.first + "=" + FileStamp(t.second);
	}
	for (const MaterialOverride& o : set.overrides[slot]) {
		char ob[64];
		snprintf(ob, sizeof(ob), " m%c%d.%d=%g", 'A' + o.colour, o.material, o.word, o.value);
		stamp += ob;
	}
	Bytes old;
	bool fresh = ReadWholeFile(root + L"stamp.txt", &old) && std::string(old.begin(), old.end()) == stamp;
	if (!fresh) {
		Bytes model;
		if (!ReadWholeFile(set.models[slot], &model)) {
			return false;
		}
		std::string modelName = "model\\hm\\hm" + sset + "\\" + tmpl + "\\" + tmpl;
		static const char kColours[] = "ABCD";
		// custom textures: normal map and mask in the main archive, the colour map (the chosen
		// letter's, else BM_A) in the colour archive; names as in the template (pre-rename)
		std::vector<Bytes> texData;
		texData.reserve(8);
		std::vector<std::pair<std::string, const Bytes*>> mainTex, colourTex;
		auto find = [&](const char* role) -> const std::wstring* {
			for (const auto& t : set.textures[slot]) {
				if (_stricmp(t.first.c_str(), role) == 0) {
					return &t.second;
				}
			}
			return nullptr;
		};
		auto add = [&](const std::wstring* path, std::vector<std::pair<std::string, const Bytes*>>* list, const std::string& tail) {
			if (!path) {
				return;
			}
			texData.emplace_back();
			if (ReadWholeFile(*path, &texData.back())) {
				list->push_back({tail, &texData.back()});
			}
		};
		add(find("NM"), &mainTex, tmpl + "_NM-00");
		add(find("MM"), &mainTex, tmpl + "_MM-00");
		{
			char role[8] = "BM_A";
			role[3] = kColours[set.colour];
			const std::wstring* bm = find(role);
			add(bm ? bm : find("BM_A"), &colourTex, tmpl + "_BM-00_" + kColours[set.colour]);
		}
		Bytes ownChain;
		const Bytes* chainData = !set.chains[slot].empty() && ReadWholeFile(set.chains[slot], &ownChain) ? &ownChain : nullptr;
		Bytes ownCol;
		const Bytes* colData = !set.chainCols[slot].empty() && ReadWholeFile(set.chainCols[slot], &ownCol) ? &ownCol : nullptr;
		for (int dset : {0x0e, 0x2e}) {
			std::string d = Hex2(dset), dv = Hex2(item);
			std::vector<std::pair<std::string, std::string>> pairs = {
				{modelName, "model\\hm\\hmf6\\" + (carrierBuild ? carrierTok : pfx + "f6" + dv) + "\\" + (carrierBuild ? carrierTok : pfx + "f6" + dv)},
				{"hm" + sset + "\\" + tmpl, "hm" + d + "\\" + (carrierBuild ? carrierTok : pfx + d + dv)},
				{tmpl, carrierBuild ? carrierTok : pfx + d + dv},
			};
			std::wstring tag = Utf8ToWide((pfx + d + dv).c_str());
			if (!CloneArc(srcDir + Utf8ToWide(tmpl.c_str()) + L".arc", dstDir + tag + L".arc", pairs, modelName, &model, std::string(), mainTex, chainData, colData)) {
				XLS_LOG_WARN("mods: parts %s: %s: cannot clone %s.arc.", set.id.c_str(), kSlotNames[slot], tmpl.c_str());
				return false;
			}
			// colour: the chosen variant's texture archive becomes colour A
			std::wstring colourSrc = srcDir + Utf8ToWide(tmpl.c_str()) + Utf8ToWide(Hex2(set.colour).c_str()) + L".arc";
			std::string from = std::string("_BM-00_") + kColours[set.colour];
			if (!CloneArc(colourSrc, dstDir + tag + L"00.arc", pairs, std::string(), nullptr, set.colour ? from : std::string(), colourTex)) {
				XLS_LOG_WARN("mods: parts %s: %s: no colour %c archive for %s.", set.id.c_str(), kSlotNames[slot], kColours[set.colour], tmpl.c_str());
			}
			// the game opens <tag><colour>.arc for the colour worn (a missing one is a fatal error);
			// the textures come from the lpc copy, so these only have to exist: the template's
			// colour c, else a copy of colour A's
			for (int c = 1; c < 4; c++) {
				std::wstring dst = dstDir + tag + Utf8ToWide(Hex2(c).c_str()) + L".arc";
				if (!CloneArc(srcDir + Utf8ToWide((tmpl + Hex2(c)).c_str()) + L".arc", dst, pairs, std::string(), nullptr, std::string())) {
					CopyFileW((dstDir + tag + L"00.arc").c_str(), dst.c_str(), FALSE);
				}
			}
		}
		{
			// lpc copy: the template's textures (main + chosen colour archive) with ours in place
			std::vector<ArcEntry> texs, colourEntries;
			Bytes file;
			if (ReadWholeFile(srcDir + Utf8ToWide(tmpl.c_str()) + L".arc", &file)) {
				ArcRead(file, &texs);
			}
			if (ReadWholeFile(srcDir + Utf8ToWide(tmpl.c_str()) + Utf8ToWide(Hex2(set.colour).c_str()) + L".arc", &file)) {
				ArcRead(file, &colourEntries);
				texs.insert(texs.end(), colourEntries.begin(), colourEntries.end());
			}
			// bone physics: the game loads the part's rChain / rChainCol from the model's path, so
			// the lpc model gets the template's (or the set's own rChain) beside it
			std::string modelName = "model\\hm\\hm" + sset + "\\" + tmpl + "\\" + tmpl;
			std::vector<ArcEntry> chains;
			for (const ArcEntry& e : texs) {
				if ((e.classHash == ClassHash("rChain") || e.classHash == ClassHash("rChainCol")) && _stricmp(e.name.c_str(), modelName.c_str()) == 0) {
					chains.push_back(e);
				}
			}
			if (!set.chains[slot].empty()) {
				Bytes own;
				if (ReadWholeFile(set.chains[slot], &own)) {
					auto it = std::find_if(chains.begin(), chains.end(), [](const ArcEntry& e) { return e.classHash == ClassHash("rChain"); });
					if (it == chains.end()) {
						ArcEntry c{};
						c.classHash = ClassHash("rChain");
						chains.push_back(c);
						it = chains.end() - 1;
					}
					it->data = own;
				}
			}
			if (!set.chainCols[slot].empty()) {
				Bytes own;
				if (ReadWholeFile(set.chainCols[slot], &own)) {
					auto it = std::find_if(chains.begin(), chains.end(), [](const ArcEntry& e) { return e.classHash == ClassHash("rChainCol"); });
					if (it == chains.end()) {
						ArcEntry c{};
						c.classHash = ClassHash("rChainCol");
						chains.push_back(c);
						it = chains.end() - 1;
					}
					it->data = own;
				}
			}
			auto addChains = [&](std::vector<ArcEntry>* to, const std::string& name) {
				for (ArcEntry c : chains) {
					c.name = name;
					to->push_back(c);
				}
			};
			std::vector<ArcEntry> only;
			for (ArcEntry& e : texs) {
				if (e.classHash != ClassHash("rTexture")) {
					continue;
				}
				for (const auto& t : mainTex) {
					if (EndsWith(e.name, t.first)) {
						e.data = *t.second;
					}
				}
				for (const auto& t : colourTex) {
					if (EndsWith(e.name, t.first)) {
						e.data = *t.second;
					}
				}
				only.push_back(e);
			}
			std::string colourName = "model\\hm\\hm" + sset + "\\" + tmpl + "\\" + tmpl + "_BM-00_" + kColours[set.colour];
			std::vector<ArcEntry> lpc;
			if (!BuildLpcEntries(lpcBase, model, only, tmpl, colourName, &lpc)) {
				XLS_LOG_WARN("mods: parts %s: %s: lpc resources failed.", set.id.c_str(), kSlotNames[slot]);
			}
			else {
				addChains(&lpc, lpcBase);
				// every colour as <lpc>_BM-00_A..D (the path hook picks one): the part's own BM_<c>,
				// else its BM_A (a retextured part's UVs don't fit the template's colours), else the
				// template's colour c
				lpc.erase(std::remove_if(lpc.begin(), lpc.end(), [&](const ArcEntry& e) { return e.name == lpcBase + "_BM-00_A"; }), lpc.end());
				for (int c = 0; c < 4; c++) {
					std::string tail = tmpl + "_BM-00_" + kColours[c];
					char role[8] = "BM_A";
					role[3] = kColours[c];
					const std::wstring* own = find(role);
					if (!own) {
						own = find("BM_A");
					}
					std::vector<ArcEntry> ce;
					if (ReadWholeFile(srcDir + Utf8ToWide((tmpl + Hex2(c)).c_str()) + L".arc", &file)) {
						ArcRead(file, &ce);
					}
					for (ArcEntry& e : ce) {
						if (e.classHash == ClassHash("rTexture") && EndsWith(e.name, tail)) {
							Bytes data;
							if (own && ReadWholeFile(*own, &data)) {
								e.data = data;
							}
							e.name = lpcBase + "_BM-00_" + kColours[c];
							lpc.push_back(e);
							break;
						}
					}
				}
				// a model copy per colour (<lpc>_<letter>) for material overrides and its own mask /
				// normal map (texture roles MM_<letter>, NM_<letter>)
				for (int c = 0; c < 4; c++) {
					bool any = false;
					for (const MaterialOverride& o : set.overrides[slot]) {
						any |= o.colour == c;
					}
					std::string mmRole = std::string("MM_") + kColours[c], nmRole = std::string("NM_") + kColours[c];
					any |= find(mmRole.c_str()) != nullptr || find(nmRole.c_str()) != nullptr;
					if (!any) {
						continue;
					}
					auto base = std::find_if(lpc.begin(), lpc.end(), [&](const ArcEntry& e) { return e.name == lpcBase && e.classHash == ClassHash("rModel"); });
					if (base == lpc.end()) {
						break;
					}
					ArcEntry v = *base;
					v.name = lpcBase + "_" + kColours[c];
					if (!PatchMaterials(&v.data, set.overrides[slot], c)) {
						XLS_LOG_WARN("mods: parts %s: %s: material overrides for colour %c don't fit the model.", set.id.c_str(), kSlotNames[slot], kColours[c]);
						continue;
					}
					std::vector<ArcEntry> extra;
					for (const char* kindTail : {"_MM-00", "_NM-00"}) {
						const std::wstring* own = find(((kindTail[1] == 'M' ? mmRole : nmRole)).c_str());
						if (!own) {
							continue;
						}
						std::string from = lpcBase + kindTail, to = from + "_" + kColours[c];
						auto tex = std::find_if(lpc.begin(), lpc.end(), [&](const ArcEntry& e) { return e.name == from; });
						Bytes data;
						if (tex == lpc.end() || to.size() >= 64 || !ReadWholeFile(*own, &data)) {
							XLS_LOG_WARN("mods: parts %s: %s: can't use %s for colour %c.", set.id.c_str(), kSlotNames[slot], kindTail + 1, kColours[c]);
							continue;
						}
						Bytes rec(64, 0);
						memcpy(rec.data(), from.data(), from.size());
						Bytes repl(64, 0);
						memcpy(repl.data(), to.data(), to.size());
						for (size_t i = 0; i + 64 <= v.data.size(); i++) {
							if (memcmp(v.data.data() + i, rec.data(), 64) == 0) {
								memcpy(v.data.data() + i, repl.data(), 64); // the model's texture record
							}
						}
						ArcEntry t = *tex;
						t.name = to;
						t.data = data;
						extra.push_back(t);
					}
					lpc.push_back(v);
					addChains(&lpc, v.name);
					lpc.insert(lpc.end(), extra.begin(), extra.end());
				}
				// into the slot's own archives, which the game loads for a DLC part
				for (int dset : {0x0e, 0x2e}) {
					std::wstring arcPath = dstDir + Utf8ToWide((pfx + Hex2(dset) + Hex2(item)).c_str()) + L".arc";
					Bytes file;
					std::vector<ArcEntry> entries;
					if (ReadWholeFile(arcPath, &file) && ArcRead(file, &entries)) {
						entries.insert(entries.end(), lpc.begin(), lpc.end());
						WriteWholeFile(arcPath, ArcWrite(entries));
					}
				}
			}
			DeleteFileW((dstDir + L"lpc" + Utf8ToWide(hash) + L".arc").c_str()); // left by lp2-partpkg 6/7
		}
		std::string sname = SnameBin({slot}, item, set.name);
		WriteWholeFile(root + L"Content\\nativePC\\sname.bin", Bytes(sname.begin(), sname.end()));
		WriteWholeFile(root + L"stamp.txt", Bytes(stamp.begin(), stamp.end()));
		XLS_LOG_INFO("mods: parts %s: %s built as DLC slot %d (template %s, colour %c).", set.id.c_str(), kSlotNames[slot], item, tmpl.c_str(), kColours[set.colour]);
	}
	out->setId = set.id;
	out->slot = slot;
	out->item = item;
	out->root = root;
	out->lpcPath = lpcBase;
	out->colours = set.colours;
	out->faction = set.factionIndex;
	out->name = set.name;
	out->carrierToken = carrierTok;
	for (const MaterialOverride& o : set.overrides[slot]) {
		out->modelVariants |= (uint8_t)(1 << o.colour);
	}
	for (const auto& t : set.textures[slot]) {
		if (t.first.size() == 4 && (t.first.compare(0, 3, "MM_") == 0 || t.first.compare(0, 3, "NM_") == 0) && t.first[3] >= 'A' && t.first[3] <= 'D') {
			out->modelVariants |= (uint8_t)(1 << (t.first[3] - 'A'));
		}
	}
	out->displayName = Utf8ToWide((set.name + " (" + kSlotNames[slot] + ")").c_str());
	std::string key = std::string("lpc:") + set.id + ":" + kSlotNames[slot];
	uint64_t h = Fnv1a64(key.data(), key.size());
	uint32_t h2 = Fnv1a32(key.data(), key.size());
	memcpy(out->contentId, "LPC", 3);
	memcpy(out->contentId + 4, &h, 8);
	memcpy(out->contentId + 12, &h2, 4);
	return true;
}

void RemoveTree(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::wstring n = fd.cFileName;
			if (n == L"." || n == L"..") {
				continue;
			}
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				RemoveTree(dir + n + L"\\");
			}
			else {
				DeleteFileW((dir + n).c_str());
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

// Carrier state: the per-set carrier builds per category, and the set each carrier slot wears
// (index into g_carrierSets) per slot and category: the slot is the loadout's alias (or 69), so the
// part code itself says which loadout a character wears and every lookup goes by it. Kept in
// ModCache\parts\carrier.txt as "<slot> <category> <set id>" lines; g_carrierWorn (the last set
// chosen, "<category> <set id>" lines) is worn by a slot with no choice yet.
std::vector<PartPackage> g_carrierSets[kPartSlotCount];
std::atomic<int> g_carrierWorn[kPartSlotCount];
std::mutex g_carrierMutex;
std::map<int, int> g_carrierChoice; // item * kPartSlotCount + category -> index

int CarrierWornIndex(int cat, int item)
{
	std::lock_guard<std::mutex> lock(g_carrierMutex);
	auto it = g_carrierChoice.find(ChoiceItem(item) * kPartSlotCount + cat);
	return it != g_carrierChoice.end() ? it->second : (int)g_carrierWorn[cat];
}

std::wstring LowerW(std::wstring s)
{
	for (wchar_t& c : s) {
		c = (wchar_t)towlower(c);
	}
	return s;
}

std::wstring CarrierFile()
{
	return Cfg().modCacheDir + L"parts\\carrier.txt";
}

int CarrierSetIndex(int cat, const char* id)
{
	for (size_t i = 0; i < g_carrierSets[cat].size(); i++) {
		if (g_carrierSets[cat][i].setId == id) {
			return (int)i;
		}
	}
	return -1;
}

void LoadCarrierChoice()
{
	Bytes data;
	if (!ReadWholeFile(CarrierFile(), &data)) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_carrierMutex);
	std::string text(data.begin(), data.end());
	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		pos = end == std::string::npos ? text.size() : end + 1;
		int item = -1;
		char cat[16], id[64];
		bool perSlot = sscanf(line.c_str(), "%d %15s %63s", &item, cat, id) == 3 && IsCarrierItem(item);
		if (!perSlot && sscanf(line.c_str(), "%15s %63s", cat, id) != 2) {
			continue;
		}
		for (int s = 0; s < kPartSlotCount; s++) {
			int i = _stricmp(cat, kSlotNames[s]) == 0 ? CarrierSetIndex(s, id) : -1;
			if (i < 0) {
				continue;
			}
			if (perSlot) {
				g_carrierChoice[ChoiceItem(item) * kPartSlotCount + s] = i;
			}
			else {
				g_carrierWorn[s] = i;
			}
		}
	}
}

// Carrier slot `item` wears set index i in category cat; saves the choices.
void PublishParts();

void SetCarrierWorn(int cat, int item, int i)
{
	{
		std::lock_guard<std::mutex> lock(g_carrierMutex);
		g_carrierChoice[ChoiceItem(item) * kPartSlotCount + cat] = i;
		g_carrierWorn[cat] = i;
		std::string text;
		for (int s = 0; s < kPartSlotCount; s++) {
			if (!g_carrierSets[s].empty()) {
				text += std::string(kSlotNames[s]) + " " + g_carrierSets[s][g_carrierWorn[s]].setId + "\n";
			}
		}
		for (const auto& c : g_carrierChoice) {
			int s = c.first % kPartSlotCount;
			char line[128];
			snprintf(line, sizeof(line), "%d %s %s\n", c.first / kPartSlotCount, kSlotNames[s], g_carrierSets[s][c.second].setId.c_str());
			text += line;
		}
		WriteWholeFile(CarrierFile(), Bytes(text.begin(), text.end()));
		XLS_LOG_INFO("mods: parts: carrier slot %d %s wears %s.", item, kSlotNames[cat], g_carrierSets[cat][i].setId.c_str());
	}
	PublishParts(); // the other players see the new choice
}

DWORD WINAPI WarmCarrierCache(LPVOID)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
	size_t bytes = 0;
	DWORD start = GetTickCount();
	std::vector<char> buf(1 << 20);
	for (const auto& sets : g_carrierSets) {
		for (const PartPackage& pkg : sets) {
			std::wstring dir = pkg.root + L"Content\\nativePC\\arcXmlTmpPC\\CustomParts\\";
			WIN32_FIND_DATAW fd;
			HANDLE find = FindFirstFileW((dir + L"*.arc").c_str(), &fd);
			if (find == INVALID_HANDLE_VALUE) {
				continue;
			}
			do {
				HANDLE f = CreateFileW((dir + fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
				if (f == INVALID_HANDLE_VALUE) {
					continue;
				}
				DWORD got = 0;
				while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) {
					bytes += got;
				}
				CloseHandle(f);
			} while (FindNextFileW(find, &fd));
			FindClose(find);
		}
	}
	XLS_LOG_INFO("mods: parts: warmed %zu MB of carrier archives in %lu ms.", bytes >> 20, (unsigned long)(GetTickCount() - start));
	return 0;
}

void PartsSyncInit();

void BuildPackages()
{
	PartsLoad();
	RemoveTree(Cfg().modCacheDir + L"parts\\loose\\"); // loose copies from lp2-partpkg 6/7
	// Sets in id order. Every set is worn through the carrier (no slot of its own any more).
	std::vector<const PartSet*> order;
	for (const PartSet& s : g_sets) {
		order.push_back(&s);
	}
	std::sort(order.begin(), order.end(), [](const PartSet* a, const PartSet* b) { return _stricmp(a->id.c_str(), b->id.c_str()) < 0; });
	// per-set slot packages from before the carrier (pkg\<id>_<category>\); carrier builds end in _c
	std::wstring pkgDir = Cfg().modCacheDir + L"parts\\pkg\\";
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW((pkgDir + L"*").c_str(), &fd);
	if (find != INVALID_HANDLE_VALUE) {
		do {
			std::wstring n = fd.cFileName;
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n != L"." && n != L".." && (n.size() < 2 || n.compare(n.size() - 2, 2, L"_c") != 0) && (n.compare(0, 8, L"carrier_") != 0 || !iswdigit(n[8]))) {
				RemoveTree(pkgDir + n + L"\\");
			}
		} while (FindNextFileW(find, &fd));
		FindClose(find);
	}
	// The carrier: every set again in slot 69 (its own folder per set and category). One package per
	// category is registered with the game; opens of its part archives go to the worn set's build.
	for (int s = 0; s < kPartSlotCount; s++) {
		std::set<std::string> tokens;
		for (const PartSet* set : order) {
			// <pfx>z + 3 letters g-z (never a game part's hex name), from the set id; bumped on a clash
			std::string key = set->id + ":" + kSlotNames[s];
			uint32_t h = Fnv1a32(key.data(), key.size()) % 8000;
			std::string tok;
			do {
				tok = std::string(kPrefixes[s]) + "z" + (char)('g' + h / 400) + (char)('g' + h / 20 % 20) + (char)('g' + h % 20);
				h = (h + 1) % 8000;
			} while (!tokens.insert(tok).second);
			PartPackage pkg;
			if (!set->models[s].empty() && BuildPackage(*set, s, kCarrierItem, &pkg, tok)) {
				pkg.setHash = set->hash;
				g_carrierSets[s].push_back(pkg);
			}
		}
		XLS_LOG_INFO("mods: parts: %s carrier (DLC slots %d-%d, %d) holds %zu set(s).", kSlotNames[s], kAliasFirst, kAliasFirst + kAliasCount - 1, kCarrierItem,
		             g_carrierSets[s].size());
	}
	// One package per carrier slot (the aliases and 69), shared by the categories: its sname.bin
	// and empty stand-ins for the archives; the overlay serves the worn set's archives by name
	// (PartsCarrierRedirect). The game enumerates at most 72 content packages (FUN_008722b0), so
	// one per slot and category (84) left the last legs slots unmounted. Each category keeps its
	// own record here (same root and content id); the content scan registers the folder once.
	std::vector<int> items;
	for (int i = 0; i < kAliasCount; i++) {
		items.push_back(kAliasFirst + i);
	}
	items.push_back(kCarrierItem);
	for (int item : items) {
		std::wstring root = Cfg().modCacheDir + L"parts\\pkg\\carrier_" + std::to_wstring(item) + L"\\";
		std::wstring dir = root + L"Content\\nativePC\\arcXmlTmpPC\\CustomParts\\";
		std::vector<int> cats;
		for (int s = 0; s < kPartSlotCount; s++) {
			if (!g_carrierSets[s].empty()) {
				cats.push_back(s);
			}
		}
		if (cats.empty()) {
			break; // no sets at all
		}
		SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
		std::string key = std::string("lpc-carrier:") + std::to_string(item);
		uint64_t h = Fnv1a64(key.data(), key.size());
		uint32_t h2 = Fnv1a32(key.data(), key.size());
		for (int s : cats) {
			PartPackage reg = g_carrierSets[s][0];
			reg.carrier = true;
			reg.item = item;
			reg.root = root;
			for (int dset : {0x0e, 0x2e}) {
				std::string base = std::string(kPrefixes[s]) + Hex2(dset) + Hex2(item);
				for (const std::string& tail : {std::string(), std::string("00"), std::string("01"), std::string("02"), std::string("03")}) {
					std::wstring f = dir + Utf8ToWide((base + tail).c_str()) + L".arc";
					if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) {
						WriteWholeFile(f, Bytes());
					}
				}
			}
			reg.displayName = L"Custom sets " + std::to_wstring(item);
			memcpy(reg.contentId + 4, &h, 8);
			memcpy(reg.contentId + 12, &h2, 4);
			g_packages.push_back(reg);
		}
		std::string sname = SnameBin(cats, item, "Custom set");
		WriteWholeFile(root + L"Content\\nativePC\\sname.bin", Bytes(sname.begin(), sname.end()));
	}
	LoadCarrierChoice();
	PartsSyncInit();
	XLS_LOG_INFO("mods: parts: %zu part package(s) ready.", g_packages.size());
	// Warm the OS file cache with every carrier build's archives (read once, low priority), so a
	// set's first load in the menu doesn't wait on the disk (it shows nothing until loaded).
	HANDLE warm = CreateThread(nullptr, 0, WarmCarrierCache, nullptr, 0, nullptr);
	if (warm) {
		CloseHandle(warm);
	}
}

}

void PartsLoad()
{
	std::call_once(g_once, Load);
}

const std::vector<PartSet>& PartSets()
{
	PartsLoad();
	return g_sets;
}

const std::vector<PartPackage>& PartPackages()
{
	std::call_once(g_pkgOnce, BuildPackages);
	return g_packages;
}

namespace {

// Unlocks: the game keeps per-category part unlock masks in its game object (DX11 *0x18520c0,
// DX9 *0x1842020): 32 bytes each at +0x50710 head, +0x50730 body, +0x50770 back, +0x50750 legs,
// bit 0x80000000 >> (id & 31) of dword id >> 5 (FUN_007de7c0 sets one). A fresh save has the DLC
// ids locked, so a thread keeps the bits of the generated parts (id 180 + item) set; profile
// loads replace the masks, the next pass sets them again (and the save then keeps them).
const uint32_t kUnlockMask[kPartSlotCount] = {0x50710, 0x50730, 0x50770, 0x50750};

DWORD WINAPI UnlockThread(LPVOID param)
{
	uint32_t gamePtr = (uint32_t)(uintptr_t)param;
	bool logged = false;
	for (;;) {
		Sleep(2000);
		uint8_t* game = *(uint8_t**)(uintptr_t)gamePtr;
		if (!game) {
			continue;
		}
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(game + 0x50770, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
			continue;
		}
		int set = 0;
		// custom slots from before the carrier (40-68) stay locked: the list then skips them
		for (int s = 0; s < kPartSlotCount; s++) {
			for (int item = kAliasFirst + kAliasCount; item < kCarrierItem; item++) {
				int id = 180 + item;
				InterlockedAnd((volatile LONG*)(game + kUnlockMask[s] + (id >> 5) * 4), ~(LONG)(0x80000000u >> (id & 31)));
			}
		}
		for (const PartPackage& pkg : g_packages) {
			int id = 180 + pkg.item;
			volatile LONG* word = (volatile LONG*)(game + kUnlockMask[pkg.slot] + (id >> 5) * 4);
			LONG bit = (LONG)(0x80000000u >> (id & 31));
			if (!(*word & bit)) {
				InterlockedOr(word, bit);
				set++;
			}
		}
		if (set && !logged) {
			XLS_LOG_INFO("mods: parts: unlocked %d custom part(s) in the profile.", set);
			logged = true;
		}
	}
}

}

void PartsStartUnlocker()
{
	char exe[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	uint32_t gamePtr = strstr(exe, "LP2DX11") ? 0x18520c0 : strstr(exe, "LP2DX9") ? 0x1842020 : 0;
	if (!gamePtr || PartPackages().empty()) {
		return;
	}
	HANDLE thread = CreateThread(nullptr, 0, UnlockThread, (LPVOID)(uintptr_t)gamePtr, 0, nullptr);
	if (thread) {
		CloseHandle(thread);
	}
}

namespace {

// The colour a part wears for a code's colour byte: that one when the set has it, else its first.
int WornColour(const PartPackage& pkg, uint8_t colour)
{
	for (int c : pkg.colours) {
		if (c == colour) {
			return c;
		}
	}
	return pkg.colours.empty() ? 0 : pkg.colours[0];
}

bool IsCarrier(uint32_t code)
{
	return ((code & 0xff) & 0xdf) == 0x0e && IsCarrierItem((code >> 16) & 0xff);
}

// The set carrier slot `item` wears in category cat (its carrier build), or nullptr.
const PartPackage* CarrierWorn(uint32_t cat, int item)
{
	if (cat >= kPartSlotCount || g_carrierSets[cat].empty()) {
		return nullptr;
	}
	int i = CarrierWornIndex((int)cat, item);
	return &g_carrierSets[cat][i >= 0 && i < (int)g_carrierSets[cat].size() ? i : 0];
}

std::atomic<int> g_lastCarrierCat{0}; // the colour-arrows predicate gets no category
thread_local bool t_inPartStep = false; // the menu's part list: old custom slots stay what they are

// Our generated part a code (set | colour << 8 | variant << 16) names in category cat, or nullptr.
const PartPackage* FindPackage(uint32_t cat, uint32_t code)
{
	if (cat >= kPartSlotCount || ((code & 0xff) & 0xdf) != 0x0e) {
		return nullptr;
	}
	if (IsCarrier(code)) {
		return CarrierWorn(cat, (code >> 16) & 0xff);
	}
	for (const PartPackage& pkg : g_packages) {
		if (pkg.slot == (int)cat && pkg.item == (int)((code >> 16) & 0xff)) {
			return &pkg;
		}
	}
	return nullptr;
}

// --- Per-player carrier sets (sync; notes: lp2-tools/notes/parts-carrier-and-sync-plan.md §6).
// A carrier code only names an alias (faction, loadout), so without more a remote player on our
// alias shows OUR set. Every player publishes their carrier choices as lobby member data "parts"
// (PartsMemberValue); the receiving side keeps them per member and resolves a player slot p
// (game+0x28[p], 0-15) to its member by the XUID in the player's info (offset found at run time
// from our own entry) or, for the test stand-in, by ModCache\parts\fake_member.txt ("slot <p>" then
// a member value, e.g. "1;d.body=spe3"). A match loads every player's parts in one stage loop
// (DX11 7d4260), one archive request per player and category; a player with a set of their own
// gets the archive "CustomParts\<token><set>[<colour>]" (the overlay serves the set's build,
// PartsCarrierRedirect), and their unit's paths (parts+4 = unit, unit parts at unit+0x6f70, player
// slot at parts+0x70) the set's names. The colour travels in the part code itself.
const int kMaxPlayers = 16;
const uint64_t kFakeMember = 0x01100001fffffff0ull; // the fake member's (made up) Steam id

// A member's choices: per alias pair and category, and their last-worn set per category (by id).
struct MemberParts {
	std::map<int, std::string> choice; // ChoiceItem * kPartSlotCount + category -> "<set id>[:<hash>]"
	std::string worn[kPartSlotCount];
};

std::mutex g_membersMutex;
std::map<uint64_t, MemberParts> g_members; // Steam id -> choices
int g_fakeSlot = -1;                       // the fake member plays this slot
int g_infoXuidOffset = -1;                 // player info -> XUID (found from our own entry)
std::atomic<bool> g_stageSeen{false};      // a stage load has started (PartsPlayer diagnostics)
uint32_t g_unitParts = 0;                  // unit -> its parts object (DX11 0x6f70, DX9 0x6f60); 0 = unknown exe
uint8_t** g_gameObject = nullptr;
using SlotMapFn = int(__attribute__((thiscall)) *)(void*, int);
SlotMapFn g_slotMap = nullptr;             // unit slot >= 100 -> player slot (DX11 7e5d10)
thread_local int t_stagePlayers[kMaxPlayers];
thread_local int t_stagePlayerCount = -1; // inside the stage loop: players present, in request order
thread_local int t_stageRequest = 0;
thread_local const PartPackage* t_requestSet = nullptr; // the request being built wears this set

// Our member value: "1" then ";<item>.<cat>=<set id>:<hash>" per carrier choice and
// ";d.<cat>=<set id>:<hash>" per last-worn set (hash: the set bundle's, MakeBundle).
std::string PartsMemberValue()
{
	std::string value = "1";
	std::lock_guard<std::mutex> lock(g_carrierMutex);
	for (int s = 0; s < kPartSlotCount; s++) {
		if (!g_carrierSets[s].empty()) {
			const PartPackage& worn = g_carrierSets[s][g_carrierWorn[s]];
			value += std::string(";d.") + kSlotNames[s] + "=" + worn.setId + ":" + worn.setHash;
		}
	}
	for (const auto& c : g_carrierChoice) {
		int s = c.first % kPartSlotCount;
		const PartPackage& chosen = g_carrierSets[s][c.second];
		value += ";" + std::to_string(c.first / kPartSlotCount) + "." + kSlotNames[s] + "=" + chosen.setId + ":" + chosen.setHash;
	}
	return value;
}

void PublishParts()
{
	xls::MemberDataSet("parts", PartsMemberValue());
}

bool ParseMemberParts(const std::string& value, MemberParts* out)
{
	if (value.compare(0, 1, "1") != 0 || (value.size() > 1 && value[1] != ';')) {
		return false;
	}
	size_t pos = 2;
	while (pos < value.size()) {
		size_t end = value.find(';', pos);
		std::string entry = value.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		pos = end == std::string::npos ? value.size() : end + 1;
		size_t dot = entry.find('.'), eq = entry.find('=');
		if (dot == std::string::npos || eq == std::string::npos || eq < dot) {
			continue;
		}
		std::string where = entry.substr(0, dot), cat = entry.substr(dot + 1, eq - dot - 1), id = entry.substr(eq + 1);
		for (int s = 0; s < kPartSlotCount; s++) {
			if (_stricmp(cat.c_str(), kSlotNames[s]) != 0) {
				continue;
			}
			if (where == "d") {
				out->worn[s] = id;
			}
			else if (int item = atoi(where.c_str()); IsCarrierItem(item)) {
				out->choice[ChoiceItem(item) * kPartSlotCount + s] = id;
			}
		}
	}
	return true;
}

// --- Other players' sets we don't have: fetched as bundles into ModCache\parts\remote\<hash>\,
// built like ours (own carrier token, internal id "<id>-<hash 8>" so names never meet a local
// set's) and kept apart: worn only by the players who sent them, never in our menu.
std::mutex g_remoteMutex;
std::deque<PartPackage> g_remoteSets[kPartSlotCount]; // deque: pointers stay valid as sets arrive
std::map<std::string, int> g_remoteState;             // hash -> 0 fetching, 1 ready, -1 failed
std::vector<PartSet> g_fakeSets;                      // the fake member's library (fake_library\)

const PartPackage* RemoteSet(uint32_t cat, const std::string& hash)
{
	std::lock_guard<std::mutex> lock(g_remoteMutex);
	for (const PartPackage& pkg : g_remoteSets[cat]) {
		if (pkg.setHash == hash) {
			return &pkg;
		}
	}
	return nullptr;
}

// A carrier token for category s no other set (ours or remote) uses, from key.
std::string FreeToken(int s, const std::string& key)
{
	std::set<std::string> used;
	for (const PartPackage& pkg : g_carrierSets[s]) {
		used.insert(pkg.carrierToken);
	}
	for (const PartPackage& pkg : g_remoteSets[s]) {
		used.insert(pkg.carrierToken);
	}
	uint32_t h = Fnv1a32(key.data(), key.size()) % 8000;
	std::string tok;
	do {
		tok = std::string(kPrefixes[s]) + "z" + (char)('g' + h / 400) + (char)('g' + h / 20 % 20) + (char)('g' + h % 20);
		h = (h + 1) % 8000;
	} while (used.count(tok));
	return tok;
}

// The transport: the bundle with this hash from member. The fake member reads its library; real
// players need the P2P transfer (not built yet).
bool FetchBundle(uint64_t member, const std::string& hash, Bytes* bundle)
{
	if (member == kFakeMember) {
		for (const PartSet& set : g_fakeSets) {
			if (set.hash == hash) {
				std::wstring dir = set.source + L"\\";
				return MakeBundle(dir, set, bundle);
			}
		}
		return false;
	}
	XLS_LOG_INFO("mods: parts: set %s from player %llu: transfers between players are not built yet.", hash.c_str(), (unsigned long long)member);
	return false;
}

struct RemoteJob {
	uint64_t member;
	std::string hash;
};

DWORD WINAPI RemoteSetThread(LPVOID param)
{
	std::unique_ptr<RemoteJob> job((RemoteJob*)param);
	const std::string& hash = job->hash;
	std::wstring dir = Cfg().modCacheDir + L"parts\\remote\\" + Utf8ToWide(hash.c_str()) + L"\\";
	bool ok = GetFileAttributesW((dir + L"verified").c_str()) != INVALID_FILE_ATTRIBUTES;
	if (!ok) {
		DWORD start = GetTickCount();
		Bytes bundle;
		if (!FetchBundle(job->member, hash, &bundle)) {
			XLS_LOG_WARN("mods: parts: could not get set %s from player %llu.", hash.c_str(), (unsigned long long)job->member);
		}
		else if (BundleHash(bundle) != hash) {
			XLS_LOG_WARN("mods: parts: set %s from player %llu doesn't match its hash, dropped.", hash.c_str(), (unsigned long long)job->member);
		}
		else if (!UnpackBundle(bundle, dir)) {
			XLS_LOG_WARN("mods: parts: set %s: bad bundle, dropped.", hash.c_str());
			RemoveTree(dir);
		}
		else {
			WriteWholeFile(dir + L"verified", Bytes());
			ok = true;
			XLS_LOG_INFO("mods: parts: got set %s (%zu bytes) from player %llu in %lu ms.", hash.c_str(), bundle.size(), (unsigned long long)job->member, GetTickCount() - start);
		}
	}
	std::vector<PartSet> sets;
	if (ok) {
		LoadFolder(dir, L"remote " + Utf8ToWide(hash.c_str()), &sets);
	}
	int built = 0;
	if (sets.size() == 1 && sets[0].hash == hash) {
		PartSet set = sets[0];
		std::string shown = set.id;
		set.id += "-" + hash.substr(0, 8); // its build folder and model names apart from any local set
		for (int s = 0; s < kPartSlotCount; s++) {
			if (set.models[s].empty()) {
				continue;
			}
			std::string tok;
			{
				std::lock_guard<std::mutex> lock(g_remoteMutex);
				tok = FreeToken(s, hash + ":" + kSlotNames[s]);
			}
			PartPackage pkg;
			if (BuildPackage(set, s, kCarrierItem, &pkg, tok)) {
				pkg.setHash = hash;
				pkg.remote = true;
				std::lock_guard<std::mutex> lock(g_remoteMutex);
				g_remoteSets[s].push_back(pkg);
				built++;
			}
		}
		XLS_LOG_INFO("mods: parts: player set %s (%s) ready, %d part(s); worn from the next load.", shown.c_str(), hash.c_str(), built);
	}
	else if (ok) {
		XLS_LOG_WARN("mods: parts: set %s didn't load (%zu set(s), hash %s).", hash.c_str(), sets.size(), sets.empty() ? "-" : sets[0].hash.c_str());
	}
	std::lock_guard<std::mutex> lock(g_remoteMutex);
	g_remoteState[hash] = built ? 1 : -1;
	return 0;
}

// Starts getting set `hash` from member (once per hash).
void EnsureRemote(uint64_t member, const std::string& hash)
{
	{
		std::lock_guard<std::mutex> lock(g_remoteMutex);
		if (!g_remoteState.emplace(hash, 0).second) {
			return;
		}
	}
	HANDLE thread = CreateThread(nullptr, 0, RemoteSetThread, new RemoteJob{member, hash}, 0, nullptr);
	if (thread) {
		CloseHandle(thread);
	}
}

// What a member's entry "<set id>[:<hash>]" stands for here: our own set when we have that exact
// set (or the id, when the entry has no hash), else the downloaded one; fetches it when missing.
const PartPackage* ResolveEntry(uint64_t member, uint32_t cat, const std::string& entry, bool fetch)
{
	size_t colon = entry.find(':');
	std::string id = entry.substr(0, colon), hash = colon == std::string::npos ? std::string() : entry.substr(colon + 1);
	int i = id.empty() ? -1 : CarrierSetIndex((int)cat, id.c_str());
	if (i >= 0 && (hash.empty() || g_carrierSets[cat][i].setHash == hash)) {
		return &g_carrierSets[cat][i];
	}
	if (hash.empty()) {
		return nullptr;
	}
	for (const PartPackage& pkg : g_carrierSets[cat]) {
		if (pkg.setHash == hash) {
			return &pkg; // the same set under another id
		}
	}
	if (const PartPackage* remote = RemoteSet(cat, hash)) {
		return remote;
	}
	if (fetch) {
		EnsureRemote(member, hash);
	}
	return nullptr;
}

void OnMemberParts(uint64_t steamId, XUID xuid, const std::string& value)
{
	MemberParts parts;
	if (!value.empty() && !ParseMemberParts(value, &parts)) {
		XLS_LOG_WARN("mods: parts: member %llu: unknown parts data \"%.40s\".", (unsigned long long)steamId, value.c_str());
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_membersMutex);
		if (value.empty()) {
			g_members.erase(steamId);
			return;
		}
		g_members[steamId] = parts;
	}
	XLS_LOG_INFO("mods: parts: member %llu (xuid 0x%016llx): %zu choice(s): %s", (unsigned long long)steamId, (unsigned long long)xuid, parts.choice.size(), value.c_str());
	// get what we don't have now (in the lobby), not at the match's load
	for (int s = 0; s < kPartSlotCount; s++) {
		ResolveEntry(steamId, s, parts.worn[s], true);
	}
	for (const auto& c : parts.choice) {
		ResolveEntry(steamId, c.first % kPartSlotCount, c.second, true);
	}
}

// The test stand-in for a second client (ModCache\parts\fake_member.txt): "slot <p>" and a parts
// line; without one, it wears every set of ModCache\parts\fake_library\ (set folders this PC
// doesn't have in Parts\, so they come through the download path).
void LoadFakeMember()
{
	Bytes data;
	if (!ReadWholeFile(Cfg().modCacheDir + L"parts\\fake_member.txt", &data)) {
		return;
	}
	std::wstring lib = Cfg().modCacheDir + L"parts\\fake_library\\";
	for (const std::wstring& d : List(lib, true)) {
		size_t before = g_fakeSets.size();
		LoadFolder(lib + d + L"\\", L"fake library " + d, &g_fakeSets);
		if (g_fakeSets.size() > before) {
			g_fakeSets.back().source = lib + d;
		}
	}
	std::string text(data.begin(), data.end());
	int slot = -1;
	std::string value;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		pos = end == std::string::npos ? text.size() : end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
			line.pop_back();
		}
		if (sscanf(line.c_str(), "slot %d", &slot) == 1 || line.empty() || line[0] == '#') {
			continue;
		}
		value = line;
	}
	if (value.empty() && !g_fakeSets.empty()) {
		value = "1";
		for (const PartSet& set : g_fakeSets) {
			for (int s = 0; s < kPartSlotCount; s++) {
				if (!set.models[s].empty()) {
					value += std::string(";d.") + kSlotNames[s] + "=" + set.id + ":" + set.hash;
				}
			}
		}
	}
	if (slot < 0 || slot >= kMaxPlayers || value.empty()) {
		XLS_LOG_WARN("mods: parts: fake_member.txt needs \"slot <0-15>\" and a parts line (or sets in fake_library).");
		return;
	}
	g_fakeSlot = slot;
	XLS_LOG_INFO("mods: parts: fake member plays slot %d (%zu library set(s)).", slot, g_fakeSets.size());
	xls::MemberDataInject(kFakeMember, "parts", value);
}

void PartsSyncInit()
{
	xls::MemberDataListen("parts", OnMemberParts);
	PublishParts();
	LoadFakeMember();
}

// The Steam id behind a player info's XUID. The game keeps the OFFLINE xuid (XUserGetSigninInfo
// with XUSER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY), which we build as the offline flag | Steam account id.
uint64_t InfoSteamId(XUID xuid)
{
	if ((xuid & 0xF000000000000000ull) == XUID_OFFLINE_FLAG && (uint32_t)xuid != 0) {
		return CSteamID((uint32)(xuid & 0xFFFFFFFF), k_EUniversePublic, k_EAccountTypeIndividual).ConvertToUint64();
	}
	return xls::SteamIdFromXuid(xuid).ConvertToUint64();
}

// The member playing slot p, or 0.
uint64_t SlotMember(int p)
{
	if (p == g_fakeSlot) {
		return kFakeMember;
	}
	uint8_t* game = g_gameObject ? *g_gameObject : nullptr;
	uint8_t* info = game && p >= 0 && p < kMaxPlayers ? *(uint8_t**)(game + 0x28 + p * 4) : nullptr;
	if (!info || g_infoXuidOffset < 0) {
		return 0;
	}
	uint64_t steamId = InfoSteamId(*(XUID*)(info + g_infoXuidOffset));
	return steamId == xls::SteamLocalId().ConvertToUint64() ? 0 : steamId; // our own: the menu's choice
}

// The set player p wears on carrier alias `item` in category cat, or nullptr (= the alias's own
// choice: our own player, no data, or a set not here (yet)).
const PartPackage* PlayerSet(int p, uint32_t cat, int item)
{
	if (p < 0 || p >= kMaxPlayers || cat >= kPartSlotCount) {
		return nullptr;
	}
	uint64_t member = SlotMember(p);
	if (!member) {
		return nullptr;
	}
	std::string entry;
	{
		std::lock_guard<std::mutex> lock(g_membersMutex);
		auto it = g_members.find(member);
		if (it == g_members.end()) {
			return nullptr;
		}
		auto c = it->second.choice.find(ChoiceItem(item) * kPartSlotCount + (int)cat);
		entry = c != it->second.choice.end() ? c->second : it->second.worn[cat];
	}
	const PartPackage* pkg = entry.empty() ? nullptr : ResolveEntry(member, cat, entry, true);
	if (!pkg && !entry.empty()) {
		static std::set<std::string> missing;
		static std::mutex mutex;
		std::lock_guard<std::mutex> lock(mutex);
		if (missing.insert(entry).second) {
			XLS_LOG_WARN("mods: parts: player %d wears %s %s, not here (yet): shows our choice.", p, kSlotNames[cat], entry.c_str());
		}
	}
	return pkg;
}

// Finds where a player info keeps the XUID: our own XUID in our own entry (once).
void FindInfoXuid(uint8_t* game)
{
	if (g_infoXuidOffset >= 0 || !xls::SteamReady()) {
		return;
	}
	XUID ours = xls::XuidFromSteamId(xls::SteamLocalId());
	XUID oursOffline = XUID_OFFLINE_FLAG | (ours & 0xFFFFFFFF); // what the game keeps (xuser.cpp)
	for (int p = 0; p < kMaxPlayers; p++) {
		uint8_t* info = *(uint8_t**)(game + 0x28 + p * 4);
		MEMORY_BASIC_INFORMATION mbi;
		if (!info || !VirtualQuery(info, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
			continue;
		}
		size_t room = (uint8_t*)mbi.BaseAddress + mbi.RegionSize - info;
		for (size_t o = 0; o + 8 <= 0x900 && o + 8 <= room; o += 4) { // infos are 0x900 apart
			XUID x = *(XUID*)(info + o);
			if (x == ours || x == oursOffline) {
				g_infoXuidOffset = (int)o;
				XLS_LOG_INFO("mods: parts: player %d's info holds our %s xuid at +0x%x.", p, x == ours ? "online" : "offline", (unsigned)o);
				return;
			}
		}
	}
	static bool logged = false;
	if (!logged) {
		logged = true;
		XLS_LOG_WARN("mods: parts: our xuid 0x%016llx not found in the player infos, online or offline (remote players fall back).", (unsigned long long)ours);
	}
}

// The player whose controller drives unit, or -1: the game's own lookup (DX11 f87f40) walks the
// player infos (game+0x28[p]) -> +0x480 -> +0x40 = that player's unit. Works for every unit class
// (match units embed their parts at +0x2700, not at the +0x6f70 of the 8cc550 class).
int OwnerPlayer(uint8_t* owner)
{
	uint8_t* game = g_gameObject ? *g_gameObject : nullptr;
	auto live = [](uint8_t* o) { return o && ((*(uint32_t*)(o + 4) & 7) == 1 || (*(uint32_t*)(o + 4) & 7) == 2); };
	if (!game || !live(owner)) {
		return -1;
	}
	for (int p = 0; p < kMaxPlayers; p++) {
		uint8_t* info = *(uint8_t**)(game + 0x28 + p * 4);
		if (!info || !(info[8] & 1)) {
			continue;
		}
		uint8_t* x = *(uint8_t**)(info + 0x480);
		if (live(x) && *(uint8_t**)(x + 0x40) == owner) {
			return p;
		}
	}
	return -1;
}

// The player slot of the unit a parts object belongs to, or -1 (menu previews, unknown exe).
int PartsPlayer(uint8_t* parts)
{
	if (!g_unitParts) {
		return -1;
	}
	uint8_t* owner = *(uint8_t**)(parts + 4);
	if (!owner || owner + g_unitParts != parts) {
		int player = g_stageSeen ? OwnerPlayer(owner) : -1;
		static std::mutex mutex;
		static std::set<uint64_t> seen;
		if (g_stageSeen) {
			std::lock_guard<std::mutex> lock(mutex);
			if (seen.size() < 32 && seen.insert((uint64_t)(uintptr_t)owner << 8 | (uint8_t)player).second) {
				XLS_LOG_INFO("mods: parts: owner 0x%p (parts at +0x%x) -> player %d.", owner, (unsigned)(parts - owner), player);
			}
		}
		return player;
	}
	int raw = *(int*)(parts + 0x70);
	int player = raw;
	if (raw >= 100) {
		uint8_t* game = g_gameObject ? *g_gameObject : nullptr;
		player = game && g_slotMap ? g_slotMap(game, raw - 100) : -1;
	}
	static std::atomic<int> logged{0};
	if (logged++ < 32) {
		XLS_LOG_INFO("mods: parts: unit 0x%p slot %d -> player %d.", owner, raw, player);
	}
	return player >= 0 && player < kMaxPlayers ? player : -1;
}

// The set the carrier part a parts object wears in category cat (alias item) stands for.
const PartPackage* CarrierFor(uint8_t* parts, uint32_t cat, int item)
{
	if (const PartPackage* own = PlayerSet(PartsPlayer(parts), cat, item)) {
		return own;
	}
	return CarrierWorn(cat, item);
}

// --- Path hook: after the game builds a part's resource path (thiscall(parts, out, kind, cat)),
// a category wearing one of our generated DLC slots gets its model (kind 0) and colour texture
// (kind 1) from the lpc copy in the slot archives. parts+0x44+cat*4: set byte (DLC = 0x0e / 0x2e), +0x46: variant
// (the DLC slot). Category order hd, bd, bk, lg = PartSlot.
using PathFn = void(__attribute__((thiscall)) *)(uint8_t*, char*, int, uint32_t);
PathFn g_pathOriginal = nullptr;

void __attribute__((thiscall)) PartPath(uint8_t* parts, char* out, int kind, uint32_t cat)
{
	g_pathOriginal(parts, out, kind, cat);
	if (cat >= 4 || kind < 0 || kind > 5 || kind == 2) {
		return;
	}
	uint8_t set = parts[0x44 + cat * 4];
	uint8_t variant = parts[0x46 + cat * 4];
	if (kind >= 3) {
		// effect provider (3) and the other models (4, 5): the worn set's own names
		if ((set & 0xdf) == 0x0e && IsCarrierItem(variant)) {
			if (const PartPackage* worn = CarrierFor(parts, cat, variant); worn && worn->carrierToken.size() == 6) {
				std::string path = out;
				for (const char* mid : {"0e", "2e", "f6"}) {
					ReplaceAll(&path, std::string(kPrefixes[cat]) + mid + Hex2(variant), worn->carrierToken);
				}
				memcpy(out, path.c_str(), path.size() + 1); // same length
			}
		}
		return;
	}
	if ((set & 0xdf) != 0x0e) {
		return;
	}
	const PartPackage* found = IsCarrier(set | (uint32_t)variant << 16) ? CarrierFor(parts, cat, variant) : FindPackage(cat, set | (uint32_t)variant << 16);
	if (found) {
		const PartPackage& pkg = *found;
		if (!pkg.lpcPath.empty()) {
			char letter = (char)('A' + WornColour(pkg, parts[0x45 + cat * 4]));
			static uint8_t seen[kPartSlotCount] = {0xff, 0xff, 0xff, 0xff};
			if (kind == 1 && seen[cat] != parts[0x45 + cat * 4]) {
				seen[cat] = parts[0x45 + cat * 4];
				XLS_LOG_INFO("mods: parts: %s %s colour byte %u -> %c.", pkg.setId.c_str(), kSlotNames[cat], seen[cat], letter);
			}
			std::string path = pkg.lpcPath + (kind == 1 ? std::string("_BM-00_") + letter : "");
			if (kind == 0 && (pkg.modelVariants >> (letter - 'A') & 1)) {
				path += std::string("_") + letter; // this colour's model copy (material overrides)
			}
			if (path.size() < 64) {
				memcpy(out, path.c_str(), path.size() + 1);
			}
		}
	}
}

// --- Customise menu colours (DX11; notes: lp2-tools/notes/parts-menu.md). The game won't step a
// DLC part's colour and shows its swatch as "" ("LOST PLANET 2"). For our parts with more than
// one colour: the colour step moves through the set's colours, and the swatch shows the letter.
// Colour step FUN_00eeb9a0 thiscall(menu, cat, code, dir): after its DLC / single-colour checks
// (0xeeb9d4) it adds dir to menu+0x54+cat (wrap 0-3), stores it in the codes, reloads the preview,
// sets the swatch and plays the sound; we enter there with the colour primed to land on ours.
using ColourStepFn = void(__attribute__((thiscall)) *)(uint8_t*, uint32_t, uint32_t, int);
using SwatchFn = void(__attribute__((thiscall)) *)(uint8_t*, uint32_t, uint32_t, uint32_t);
ColourStepFn g_colourStepOriginal = nullptr;
ColourStepFn g_colourStepTail = nullptr;
SwatchFn g_swatchOriginal = nullptr;

void SetChosenColour(const PartPackage& pkg, uint32_t code, int colour);

uint32_t MenuCode(uint8_t* menu, uint32_t cat)
{
	return *(uint32_t*)(menu + 0xa8 + (cat + *(int32_t*)(menu + 0xa4) * 8) * 4);
}

void __attribute__((thiscall)) ColourStep(uint8_t* menu, uint32_t cat, uint32_t code, int dir)
{
	const PartPackage* pkg = FindPackage(cat, code);
	if (!pkg || pkg->colours.size() < 2) {
		g_colourStepOriginal(menu, cat, code, dir);
		return;
	}
	int n = (int)pkg->colours.size();
	int worn = WornColour(*pkg, (code >> 8) & 0xff);
	int at = (int)(std::find(pkg->colours.begin(), pkg->colours.end(), worn) - pkg->colours.begin());
	int step = (int8_t)dir < 0 ? -1 : 1;
	int next = pkg->colours[((at + step) % n + n) % n];
	menu[0x54 + cat] = (uint8_t)(next - step); // the game adds step: lands on next, no wrap
	XLS_LOG_INFO("mods: parts: %s %s colour %c -> %c.", pkg->setId.c_str(), kSlotNames[cat], 'A' + worn, 'A' + next);
	SetChosenColour(*pkg, code, next);
	g_colourStepTail(menu, cat, code, dir);
}

// Swatch FUN_00ee5060 thiscall(menu, index, cat, colour): colour 0-3 -> "A".."D", else "".
void __attribute__((thiscall)) Swatch(uint8_t* menu, uint32_t index, uint32_t cat, uint32_t colour)
{
	if (colour > 3 && cat < kPartSlotCount) {
		uint32_t code = MenuCode(menu, cat);
		const PartPackage* pkg = FindPackage(cat, code);
		if (pkg && pkg->colours.size() > 1) {
			colour = (uint32_t)WornColour(*pkg, (code >> 8) & 0xff);
		}
	}
	g_swatchOriginal(menu, index, cat, colour);
}

// Colour-locked predicate FUN_00ee4c40 stdcall(code): true hides the colour row's arrows (stock:
// single-colour parts and sets 0x0c/0x0d/0x0e). It gets no category; a set's parts share one DLC
// slot and one colour list, so the slot alone finds the set.
using ColourLockedFn = uint8_t(__stdcall*)(uint32_t);
ColourLockedFn g_colourLockedOriginal = nullptr;

uint8_t __stdcall ColourLocked(uint32_t code)
{
	if (IsCarrier(code)) {
		if (const PartPackage* pkg = CarrierWorn(g_lastCarrierCat, (code >> 16) & 0xff)) {
			return pkg->colours.size() < 2;
		}
	}
	if (((code & 0xff) & 0xdf) == 0x0e) {
		for (const PartPackage& pkg : g_packages) {
			if (pkg.item == (int)((code >> 16) & 0xff)) {
				return pkg.colours.size() < 2;
			}
		}
	}
	return g_colourLockedOriginal(code);
}

// Descriptor id lookup FUN_00f5c860 cdecl(set, colour, variant, cat): DLC entries exist for colour
// A only, so a DLC code with another colour finds nothing (0x100) and the loadout falls back to
// id 0 (hd2f0000, a part that doesn't exist: fatal "Failed open file"). Our parts keep their
// colour in the code byte; the id is the slot's.
using DescriptorIdFn = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t);
DescriptorIdFn g_descriptorIdOriginal = nullptr;

uint32_t __cdecl DescriptorId(uint32_t set, uint32_t colour, uint32_t variant, uint32_t cat)
{
	if (colour != 0 && (set & 0xff) == 0x0e) {
		for (const PartPackage& pkg : g_packages) {
			if (pkg.item == (int)(variant & 0xff)) {
				colour = 0;
				break;
			}
		}
	}
	uint32_t id = g_descriptorIdOriginal(set, colour, variant, cat);
	if (id >= 0x100 && (set & 0xdf) == 0x0e) {
		static int logged = 0;
		if (logged++ < 8) {
			XLS_LOG_WARN("mods: parts: no descriptor for DLC set 0x%02x colour %u variant %u (cat %u), asked by 0x%p.", set & 0xff, colour, variant & 0xff, cat,
			             __builtin_return_address(0));
		}
	}
	return id;
}

// --- Chosen colours. A DLC part has one id for all colours (stock parts have one per colour), so
// the save can't hold a custom part's colour: it is kept here per set and category
// (ModCache\parts\colours.txt, "<set id> <category> <letter>" lines) and put back into the code
// whenever the game turns an id into a code.
std::mutex g_coloursMutex;
std::map<std::string, int> g_chosen;
bool g_chosenLoaded = false;

std::wstring ColoursFile()
{
	return Cfg().modCacheDir + L"parts\\colours.txt";
}

void LoadChosenLocked()
{
	if (g_chosenLoaded) {
		return;
	}
	g_chosenLoaded = true;
	Bytes data;
	if (!ReadWholeFile(ColoursFile(), &data)) {
		return;
	}
	std::string text(data.begin(), data.end()), line;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		pos = end == std::string::npos ? text.size() : end + 1;
		char id[64], cat[16], letter = 0;
		int item = -1;
		int n = sscanf(line.c_str(), "%63s %15s %c %d", id, cat, &letter, &item);
		if (n >= 3 && letter >= 'A' && letter <= 'D') {
			g_chosen[std::string(id) + ":" + cat + (n == 4 ? ":" + std::to_string(item) : std::string())] = letter - 'A';
		}
	}
}

// Keys: "<set id>:<category>" (the set's last colour, worn by a loadout with no choice yet) and,
// for a carrier code, "<set id>:<category>:<slot>" (that loadout's: its alias pair's first slot).
std::string ColourKey(const PartPackage& pkg, uint32_t code)
{
	std::string key = pkg.setId + ":" + kSlotNames[pkg.slot];
	if (IsCarrier(code)) {
		key += ":" + std::to_string(ChoiceItem((code >> 16) & 0xff));
	}
	return key;
}

int ChosenColour(const PartPackage& pkg, uint32_t code)
{
	std::lock_guard<std::mutex> lock(g_coloursMutex);
	LoadChosenLocked();
	auto it = g_chosen.find(ColourKey(pkg, code));
	if (it == g_chosen.end()) {
		it = g_chosen.find(pkg.setId + ":" + kSlotNames[pkg.slot]);
	}
	return it == g_chosen.end() ? -1 : it->second;
}

void SetChosenColour(const PartPackage& pkg, uint32_t code, int colour)
{
	std::lock_guard<std::mutex> lock(g_coloursMutex);
	LoadChosenLocked();
	g_chosen[pkg.setId + ":" + kSlotNames[pkg.slot]] = colour;
	g_chosen[ColourKey(pkg, code)] = colour;
	std::string text;
	for (const auto& c : g_chosen) {
		size_t a = c.first.find(':'), b = c.first.find(':', a + 1);
		text += c.first.substr(0, a) + " " + c.first.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1) + " " + (char)('A' + c.second);
		if (b != std::string::npos) {
			text += " " + c.first.substr(b + 1);
		}
		text += "\n";
	}
	WriteWholeFile(ColoursFile(), Bytes(text.begin(), text.end()));
}

// Apply part FUN_00eed720 thiscall(menu, cat, code): wearing a part resets its colour to 0. For our
// parts with a chosen colour, step to it through the colour step's tail (preview, swatch, codes).
using ApplyFn = void(__attribute__((thiscall)) *)(uint8_t*, uint32_t, uint32_t);
ApplyFn g_applyOriginal = nullptr;

void __attribute__((thiscall)) ApplyPart(uint8_t* menu, uint32_t cat, uint32_t code)
{
	g_applyOriginal(menu, cat, code);
	const PartPackage* pkg = FindPackage(cat, code);
	if (!pkg || pkg->colours.size() < 2 || !g_colourStepTail) {
		return;
	}
	int chosen = ChosenColour(*pkg, code);
	if (chosen < 0) {
		return;
	}
	int worn = WornColour(*pkg, (uint8_t)chosen);
	menu[0x54 + cat] = (uint8_t)(worn - 1); // the tail adds 1
	g_colourStepTail(menu, cat, MenuCode(menu, cat), 1);
}

// DLC part name FUN_008654a0 thiscall(names, cat, variant, language) (DX9 855df0): the 44-byte
// sname.bin name of a DLC slot. The carrier's name is the worn set's.
using DlcNameFn = char*(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t, uint32_t);
DlcNameFn g_dlcNameOriginal = nullptr;

char* __attribute__((thiscall)) DlcName(void* names, uint32_t cat, uint32_t variant, uint32_t language)
{
	if ((cat & 0xff) < kPartSlotCount && IsCarrierItem(variant & 0xff)) {
		if (const PartPackage* pkg = CarrierWorn(cat & 0xff, variant & 0xff)) {
			static char buf[kPartSlotCount][0x2c];
			char* b = buf[cat & 0xff];
			snprintf(b, 0x2c, "%s", pkg->name.c_str());
			return b;
		}
	}
	return g_dlcNameOriginal(names, cat, variant, language);
}

// Part step FUN_00ee4ac0 thiscall(menu, &out, cat, dir) (DX9 ed4ca0): Left/Right on a part row
// steps the list (unlocks, faction filter) and returns the new code in out (the current one when
// nothing else is wearable); the menu applies it when it differs from menu+0xa8+cat*4. On the
// carrier, Left/Right step through the sets first (the menu's faction, sets for every faction
// everywhere); past either end the game's step moves on. Coming onto the carrier starts at the
// near end.
using PartCodeFn = uint32_t(__cdecl*)(uint32_t, uint32_t);
PartCodeFn g_partCodeOriginal = nullptr;

// Backup for a set's first load in a session: if the preview hasn't got it after 1.5 s (state 3 at
// preview + g_previewState + cat*4 = loaded; the preview is menu+0x344), it is applied again.
uint32_t g_previewState = 0x20d4;
std::atomic<uint32_t> g_redrawCode[kPartSlotCount];
std::atomic<int> g_redrawPass[kPartSlotCount];    // redraws done for g_redrawCode (two: when loaded, ~1 s later)
std::atomic<DWORD> g_redrawAt[kPartSlotCount];    // tick of the first
std::set<int> g_loadedOnce[kPartSlotCount]; // menu thread only

void NoteCarrierWorn(uint32_t cat, int set, uint32_t code)
{
	if (g_loadedOnce[cat].insert(set).second) {
		g_redrawPass[cat] = 0;
		g_redrawAt[cat] = GetTickCount();
		g_redrawCode[cat] = code;
	}
}

void __attribute__((thiscall)) ApplyPart(uint8_t* menu, uint32_t cat, uint32_t code);


// Menu update FUN_00ef0c20 (DX9 ee0e00), every frame while the customise menu runs.
extern "C" __attribute__((used)) void __stdcall MenuTick(uint8_t* menu)
{
	uint8_t* preview = *(uint8_t**)(menu + 0x344);
	for (uint32_t cat = 0; cat < kPartSlotCount; cat++) {
		uint32_t code = g_redrawCode[cat];
		if (!code) {
			continue;
		}
		if (MenuCode(menu, cat) != code) {
			g_redrawCode[cat] = 0; // moved on before it loaded
			continue;
		}
		if (!preview) {
			continue;
		}
		if (*(int32_t*)(preview + g_previewState + cat * 4) != 3) {
			// never ready: the game keeps retrying a model it didn't find; load it again
			if (GetTickCount() - g_redrawAt[cat] > 1500 && g_redrawPass[cat] < 4) {
				int pass = ++g_redrawPass[cat];
				g_redrawAt[cat] = GetTickCount();
				XLS_LOG_INFO("mods: parts: %s carrier not loaded after 1.5 s, reload %d.", kSlotNames[cat], pass);
				ApplyPart(menu, cat, code);
			}
			continue;
		}
		g_redrawCode[cat] = 0; // loaded
	}
}
extern "C" void* g_menuUpdateTramp;
__attribute__((used)) void* g_menuUpdateTramp = nullptr;
extern "C" void MenuUpdateStub();
asm(".text\n"
    "_MenuUpdateStub:\n"
    "  pushal\n"
    "  push %ecx\n"
    "  call _MenuTick@4\n"
    "  popal\n"
    "  jmp *_g_menuUpdateTramp\n");

using PartStepFn = uint32_t*(__attribute__((thiscall)) *)(uint8_t*, uint32_t*, uint32_t, int);
PartStepFn g_partStepOriginal = nullptr;

uint32_t* __attribute__((thiscall)) PartStep(uint8_t* menu, uint32_t* out, uint32_t cat, int dir)
{
	uint32_t cur = MenuCode(menu, cat);
	if (Cfg().partsAnyFaction) {
		menu[0x72f] = 1; // the menu's own ignore-faction flag: the stock list skips its faction filter too
	}
	if (dir == 0 || cat >= kPartSlotCount || g_carrierSets[cat].empty() || !g_partCodeOriginal) {
		return g_partStepOriginal(menu, out, cat, dir);
	}
	uint32_t* wornSlot = (uint32_t*)(menu + 0xa8 + (cat + *(int32_t*)(menu + 0xa4) * 8) * 4);
	int faction = *(int32_t*)(menu + 0x124);
	bool anyFaction = menu[0x72f] != 0;
	std::vector<int> list;
	for (size_t i = 0; i < g_carrierSets[cat].size(); i++) {
		int f = g_carrierSets[cat][i].faction;
		if (anyFaction || f < 0 || f == faction) {
			list.push_back((int)i);
		}
	}
	// this loadout's carrier slots (its own, whatever slot the code had); a switch wears the other one
	int loadout = *(int32_t*)(menu + 0xa4);
	int curItem = (cur >> 16) & 0xff;
	int alias = AliasItem(faction, loadout, 0);
	int other = curItem == alias ? AliasItem(faction, loadout, 1) : alias;
	uint32_t aliasCode = g_partCodeOriginal(180 + alias, cat);
	uint32_t otherCode = g_partCodeOriginal(180 + other, cat);
	int step = dir < 0 ? -1 : 1;
	if (IsCarrier(cur) && !list.empty()) {
		int at = (int)(std::find(list.begin(), list.end(), CarrierWornIndex((int)cat, (cur >> 16) & 0xff)) - list.begin());
		if (at == (int)list.size()) {
			at = step > 0 ? -1 : (int)list.size(); // worn set not in this faction's list
		}
		int next = at + step;
		if (next >= 0 && next < (int)list.size()) {
			SetCarrierWorn((int)cat, alias, list[next]);
			g_lastCarrierCat = (int)cat;
			XLS_LOG_INFO("mods: parts: %s carrier -> %s (%d/%zu).", kSlotNames[cat], g_carrierSets[cat][list[next]].setId.c_str(), next + 1, list.size());
			*out = otherCode;
			NoteCarrierWorn(cat, list[next], otherCode);
			if (otherCode == *(uint32_t*)(menu + 0xa8 + cat * 4)) {
				ApplyPart(menu, cat, otherCode); // the menu won't (same code): reload the preview, name, colour
			}
			return out;
		}
	}
	t_inPartStep = true;
	uint32_t* r;
	if (IsCarrier(cur)) {
		// leaving the carrier: the list holds every carrier slot, step over them all. The game's step
		// stops on the worn code's own entry, so the worn code is hidden meanwhile (no entry matches).
		uint32_t* worn = wornSlot;
		const uint32_t kNone = 0xfefefefe;
		*worn = kNone;
		r = g_partStepOriginal(menu, out, cat, dir);
		for (int guard = 0; guard < kAliasCount + 2 && IsCarrier(*out); guard++) {
			r = g_partStepOriginal(menu, out, cat, dir);
		}
		*worn = cur;
		if (*out == kNone || IsCarrier(*out)) {
			*out = cur; // nothing else to wear
		}
	}
	else {
		r = g_partStepOriginal(menu, out, cat, dir);
	}
	t_inPartStep = false;
	if (IsCarrier(*out) && !IsCarrier(cur)) {
		*out = aliasCode; // coming onto the carrier: this loadout's slot, at the near end
		if (!list.empty()) {
			SetCarrierWorn((int)cat, alias, step > 0 ? list.front() : list.back());
			NoteCarrierWorn(cat, step > 0 ? list.front() : list.back(), aliasCode);
		}
		g_lastCarrierCat = (int)cat;
	}
	return r;
}

// Archive request FUN_007d3fd0 thiscall(loader, character, cat, code) (DX9 7c5120): logs which
// character slot asks for which part (sync groundwork: what `character` is for the menu preview,
// the local player, remote players and bots). Each (character, cat, code) once, up to 300 lines.
using PartRequestFn = void(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t, uint32_t);
PartRequestFn g_partRequestOriginal = nullptr;

void __attribute__((thiscall)) PartRequest(void* loader, uint32_t character, uint32_t cat, uint32_t code)
{
	static std::mutex mutex;
	static std::set<uint64_t> seen;
	{
		std::lock_guard<std::mutex> lock(mutex);
		uint64_t key = (uint64_t)character << 40 | (uint64_t)(cat & 0xff) << 32 | code;
		if (seen.size() < 300 && seen.insert(key).second) {
			XLS_LOG_INFO("mods: parts: request character %u %s code 0x%08x%s (from 0x%p).", character, cat < kPartSlotCount ? kSlotNames[cat] : "?", code,
			             IsCarrier(code) ? " carrier" : "", __builtin_return_address(0));
		}
	}
	// in the stage loop: whose part this is (the loop asks for 4 categories per present player)
	const PartPackage* own = nullptr;
	if (t_stagePlayerCount > 0 && IsCarrier(code)) {
		int player = t_stagePlayers[(t_stageRequest / 4) % t_stagePlayerCount];
		own = PlayerSet(player, cat, (code >> 16) & 0xff);
		if (own) {
			XLS_LOG_INFO("mods: parts: stage request player %d %s -> %s.", player, kSlotNames[cat], own->setId.c_str());
		}
	}
	if (t_stagePlayerCount > 0) {
		t_stageRequest++;
	}
	t_requestSet = own;
	g_partRequestOriginal(loader, character, cat, code);
	t_requestSet = nullptr;
}

// The request's archive names (sprintf "CustomParts\%s%02x%02x[%02x]" from prefix, set, item[,
// colour]): for a player wearing a set of their own, "CustomParts\<token><set>[<colour>]".
extern "C" int __cdecl RequestNameSprintf(char* out, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	int n = vsprintf(out, format, args);
	va_end(args);
	if (const PartPackage* own = t_requestSet; own && own->carrierToken.size() == 6) {
		va_start(args, format);
		va_arg(args, const char*);
		unsigned set = va_arg(args, unsigned);
		va_arg(args, unsigned);
		std::string name = "CustomParts\\" + own->carrierToken + Hex2((int)(set & 0xff));
		if (strstr(format, "%02x%02x%02x")) {
			name += Hex2((int)(va_arg(args, unsigned) & 0xff));
		}
		va_end(args);
		n = sprintf(out, "%s", name.c_str());
	}
	return n;
}

// Stage loop DX11 FUN_007d4260 thiscall(loader, character, 0, 2): requests 4 categories for each
// present player (game+0x28[p] != 0): offline players 0-3 twice (f8c6a0 codes, then the infos'),
// online 0-15 once.
using StageRequestFn = void(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t, uint32_t);
StageRequestFn g_stageRequestOriginal = nullptr;

void __attribute__((thiscall)) StageRequest(void* loader, uint32_t character, uint32_t a, uint32_t b)
{
	uint8_t* game = g_gameObject ? *g_gameObject : nullptr;
	t_stagePlayerCount = 0;
	t_stageRequest = 0;
	g_stageSeen = true;
	if (game) {
		FindInfoXuid(game);
		int limit = game[0x256] == 0 ? 4 : kMaxPlayers;
		for (int p = 0; p < limit; p++) {
			if (*(uint32_t*)(game + 0x28 + p * 4) != 0) {
				t_stagePlayers[t_stagePlayerCount++] = p;
			}
		}
	}
	static std::atomic<int> logged{0};
	if (logged++ < 16) {
		XLS_LOG_INFO("mods: parts: stage parts request (character %u, %d player(s), %s).", character, t_stagePlayerCount, game && game[0x256] ? "online" : "offline");
	}
	g_stageRequestOriginal(loader, character, a, b);
	t_stagePlayerCount = -1;
}

// Part id -> code FUN_00f5cae0 cdecl(id, cat). Id 0 of every category is a placeholder
// (0x0500000f: set 0x0f, faction 5) whose colour archive doesn't exist, so loading it is fatal. A
// loadout holding id 0 (a DLC part saved with a colour other than A) gets the default part (id 1)
// instead. Also logs which caller produced it (diagnostics for the colour/save route).

uint32_t __cdecl PartCode(uint32_t id, uint32_t cat)
{
	uint32_t code = g_partCodeOriginal(id, cat);
	if (id == 0 && cat < kPartSlotCount && (code & 0xff) == 0x0f) {
		static int logged = 0;
		if (logged++ < 8) {
			XLS_LOG_WARN("mods: parts: %s id 0 (placeholder) asked for by 0x%p; using the default part.", kSlotNames[cat], __builtin_return_address(0));
		}
		return g_partCodeOriginal(1, cat);
	}
	// a save from before the carrier wears a custom part's own slot (20-68): wear the carrier instead
	// (or the default part when this category has none)
	if (!t_inPartStep && cat < kPartSlotCount && ((code & 0xff) & 0xdf) == 0x0e && id >= 180 + kAliasFirst + kAliasCount && id < 180 + kCarrierItem) {
		static int logged = 0;
		if (logged++ < 8) {
			XLS_LOG_WARN("mods: parts: %s DLC slot %u is no longer used; wearing the %s.", kSlotNames[cat], id - 180, g_carrierSets[cat].empty() ? "default part" : "carrier");
		}
		code = g_partCodeOriginal(g_carrierSets[cat].empty() ? 1 : 180 + kCarrierItem, cat);
	}
	if (const PartPackage* pkg = FindPackage(cat, code)) {
		int chosen = ChosenColour(*pkg, code);
		if (chosen >= 0) {
			code = (code & ~0xff00u) | ((uint32_t)WornColour(*pkg, (uint8_t)chosen) << 8);
		}
	}
	return code;
}

// Part code -> id FUN_00f5c9a0 cdecl(code, cat): logs failed lookups of DLC codes (diagnostics).
using PartIdFn = uint32_t(__cdecl*)(uint32_t, uint32_t);
PartIdFn g_partIdOriginal = nullptr;

uint32_t __cdecl PartId(uint32_t code, uint32_t cat)
{
	uint32_t id = g_partIdOriginal(code, cat);
	if (id >= 0x100 && ((code & 0xff) & 0xdf) == 0x0e) {
		static int logged = 0;
		if (logged++ < 8) {
			XLS_LOG_WARN("mods: parts: no id for DLC code 0x%08x (cat %u), asked by 0x%p.", code, cat, __builtin_return_address(0));
		}
	}
	return id;
}

void* Detour(uint8_t* fn, const uint8_t* expect, size_t length, void* detour)
{
	if (memcmp(fn, expect, length) != 0) {
		return nullptr;
	}
	auto* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!tramp) {
		return nullptr;
	}
	memcpy(tramp, fn, length);
	tramp[length] = 0xe9;
	int32_t back = (int32_t)((fn + length) - (tramp + length + 5));
	memcpy(tramp + length + 1, &back, 4);
	FlushInstructionCache(GetCurrentProcess(), tramp, 32);
	uint8_t patch[16];
	memset(patch, 0x90, sizeof(patch));
	patch[0] = 0xe9;
	int32_t rel = (int32_t)((uint8_t*)detour - (fn + 5));
	memcpy(patch + 1, &rel, 4);
	DWORD old = 0;
	if (!VirtualProtect(fn, length, PAGE_EXECUTE_READWRITE, &old)) {
		return nullptr;
	}
	memcpy(fn, patch, length);
	VirtualProtect(fn, length, old, &old);
	FlushInstructionCache(GetCurrentProcess(), fn, length);
	return tramp;
}

const uint8_t kColourStepPrologue[] = {0x8a, 0x44, 0x24, 0x08, 0x56, 0x8b, 0xf1};      // mov al,[esp+8]; push esi; mov esi,ecx
const uint8_t kColourStepBody[] = {0x8a, 0x4c, 0x24, 0x10, 0x57, 0x8b, 0x7c, 0x24, 0x0c}; // at +0x34: mov cl,[esp+0x10]; push edi; mov edi,[esp+0xc]
const uint8_t kColourLockedPrologue[] = {0x8b, 0x44, 0x24, 0x04, 0x50};               // mov eax,[esp+4]; push eax
const uint8_t kDescriptorIdPrologue[] = {0x0f, 0xb6, 0x4c, 0x24, 0x0c, 0x0f, 0xb6, 0x54, 0x24, 0x08}; // movzx ecx,[esp+0xc]; movzx edx,[esp+8]
const uint8_t kPartCodePrologue[] = {0x8b, 0x44, 0x24, 0x04, 0x3d, 0xb4, 0x00, 0x00, 0x00}; // mov eax,[esp+4]; cmp eax,0xb4
const uint8_t kPartIdPrologue[] = {0x51, 0x8a, 0x4c, 0x24, 0x08, 0x80, 0xe1, 0xdf};       // push ecx; mov cl,[esp+8]; and cl,0xdf
const uint8_t kApplyPrologue[] = {0x56, 0x8b, 0xf1, 0x57, 0x8b, 0x7c, 0x24, 0x0c};       // push esi; mov esi,ecx; push edi; mov edi,[esp+0xc]
const uint8_t kPartStepPrologue[] = {0x53, 0x8b, 0x5c, 0x24, 0x0c, 0x55, 0x8b, 0xe9};   // push ebx; mov ebx,[esp+0xc]; push ebp; mov ebp,ecx
const uint8_t kDlcNamePrologue[] = {0x53, 0x8a, 0x5c, 0x24, 0x08, 0x56, 0x8b, 0xf1};    // push ebx; mov bl,[esp+8]; push esi; mov esi,ecx
const uint8_t kMenuUpdatePrologue[] = {0x83, 0xec, 0x08, 0x53, 0x56, 0x8b, 0xf1};      // sub esp,8; push ebx; push esi; mov esi,ecx
const uint8_t kPartRequestPrologue[] = {0x81, 0xec, 0x10, 0x01, 0x00, 0x00};       // sub esp,0x110
const uint8_t kStageRequestPrologue[] = {0x51, 0x80, 0xb9, 0x20, 0x95, 0x03, 0x00, 0x00}; // push ecx; cmp byte [ecx+0x39520],0
const uint8_t kSwatchPrologue[] = {0x83, 0xec, 0x24, 0x8b, 0x54, 0x24, 0x2c};          // sub esp,0x24; mov edx,[esp+0x2c]

// VAs per exe (LP2DX11.exe, LP2DX9.exe); every prologue is checked before patching.
struct MenuVas {
	uint32_t step, swatch, locked, apply, descriptorId, partCode, partId, partStep, dlcName, menuUpdate, previewState, partRequest;
	uint32_t stageRequest, nameSprintf[2], sprintfFn, game, unitParts, slotMap;
};
const MenuVas kMenuVas[] = {
	{0xeeb9a0, 0xee5060, 0xee4c40, 0xeed720, 0xf5c860, 0xf5cae0, 0xf5c9a0, 0xee4ac0, 0x8654a0, 0xef0c20, 0x20d4, 0x7d3fd0,
	 0x7d4260, {0x7d4097, 0x7d4118}, 0x1224239, 0x18520c0, 0x6f70, 0x7e5d10}, // DX11
	{0xedbb80, 0xed5240, 0xed4e20, 0xedd900, 0xf4cb40, 0xf4cdc0, 0xf4cc80, 0xed4ca0, 0x855df0, 0xee0e00, 0x20c4, 0x7c5120,
	 0x7c53b0, {0x7c51e7, 0x7c5268}, 0x1213e59, 0x1842020, 0x6f60, 0x7d6e50}, // DX9
};

void InstallMenuColours(uint8_t* base, const MenuVas& va)
{
	auto at = [&](uint32_t v) { return base + (v - 0x400000); };
	uint8_t* step = at(va.step);
	uint8_t* body = step + 0x34;
	if (memcmp(body, kColourStepBody, sizeof(kColourStepBody)) != 0 || memcmp(step, kColourStepPrologue, sizeof(kColourStepPrologue)) != 0 ||
	    memcmp(at(va.swatch), kSwatchPrologue, sizeof(kSwatchPrologue)) != 0 || memcmp(at(va.apply), kApplyPrologue, sizeof(kApplyPrologue)) != 0) {
		XLS_LOG_WARN("mods: parts: customise menu not as expected; custom parts keep one colour.");
		return;
	}
	// the tail entry: push esi; mov esi,ecx; jmp body (the stack as the body expects it)
	auto* tail = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!tail) {
		return;
	}
	tail[0] = 0x56;
	tail[1] = 0x8b;
	tail[2] = 0xf1;
	tail[3] = 0xe9;
	int32_t rel = (int32_t)(body - (tail + 8));
	memcpy(tail + 4, &rel, 4);
	FlushInstructionCache(GetCurrentProcess(), tail, 16);
	g_colourStepTail = (ColourStepFn)tail;
	g_partCodeOriginal = (PartCodeFn)Detour(at(va.partCode), kPartCodePrologue, sizeof(kPartCodePrologue), (void*)&PartCode);
	g_partIdOriginal = (PartIdFn)Detour(at(va.partId), kPartIdPrologue, sizeof(kPartIdPrologue), (void*)&PartId);
	g_descriptorIdOriginal = (DescriptorIdFn)Detour(at(va.descriptorId), kDescriptorIdPrologue, sizeof(kDescriptorIdPrologue), (void*)&DescriptorId);
	g_colourStepOriginal = (ColourStepFn)Detour(step, kColourStepPrologue, sizeof(kColourStepPrologue), (void*)&ColourStep);
	g_swatchOriginal = (SwatchFn)Detour(at(va.swatch), kSwatchPrologue, sizeof(kSwatchPrologue), (void*)&Swatch);
	g_colourLockedOriginal = (ColourLockedFn)Detour(at(va.locked), kColourLockedPrologue, sizeof(kColourLockedPrologue), (void*)&ColourLocked);
	g_applyOriginal = (ApplyFn)Detour(at(va.apply), kApplyPrologue, sizeof(kApplyPrologue), (void*)&ApplyPart);
	if (g_applyOriginal) {
		g_partStepOriginal = (PartStepFn)Detour(at(va.partStep), kPartStepPrologue, sizeof(kPartStepPrologue), (void*)&PartStep);
	}
	g_previewState = va.previewState;
	g_partRequestOriginal = (PartRequestFn)Detour(at(va.partRequest), kPartRequestPrologue, sizeof(kPartRequestPrologue), (void*)&PartRequest);
	int names = 0;
	if (g_partRequestOriginal) {
		// the request's two archive name sprintf calls -> RequestNameSprintf (call rel32 checked)
		for (uint32_t site : va.nameSprintf) {
			uint8_t* call = at(site);
			if (call[0] == 0xe8 && call + 5 + *(int32_t*)(call + 1) == at(va.sprintfFn)) {
				int32_t rel = (int32_t)((uint8_t*)&RequestNameSprintf - (call + 5));
				DWORD old = 0;
				if (VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
					memcpy(call + 1, &rel, 4);
					VirtualProtect(call + 1, 4, old, &old);
					FlushInstructionCache(GetCurrentProcess(), call, 5);
					names++;
				}
			}
		}
		if (names == 2) {
			g_gameObject = (uint8_t**)at(va.game);
			g_unitParts = va.unitParts;
			if (memcmp(at(va.slotMap), "\x81\xec\xd8\x00\x00\x00\x53", 7) == 0) {
				g_slotMap = (SlotMapFn)at(va.slotMap); // sub esp,0xd8; push ebx
			}
			g_stageRequestOriginal = (StageRequestFn)Detour(at(va.stageRequest), kStageRequestPrologue, sizeof(kStageRequestPrologue), (void*)&StageRequest);
		}
	}
	XLS_LOG_INFO("mods: parts: per-player sets: names %d, stage %d.", names, !!g_stageRequestOriginal);
	g_menuUpdateTramp = Detour(at(va.menuUpdate), kMenuUpdatePrologue, sizeof(kMenuUpdatePrologue), (void*)&MenuUpdateStub);
	g_dlcNameOriginal = (DlcNameFn)Detour(at(va.dlcName), kDlcNamePrologue, sizeof(kDlcNamePrologue), (void*)&DlcName);
	XLS_LOG_INFO("mods: parts: customise menu colours hooked (code %d, id %d, descriptor %d, step %d, swatch %d, arrows %d, apply %d, part step %d, name %d, redraw %d, request log %d).", !!g_partCodeOriginal,
	             !!g_partIdOriginal, !!g_descriptorIdOriginal, !!g_colourStepOriginal, !!g_swatchOriginal, !!g_colourLockedOriginal, !!g_applyOriginal, !!g_partStepOriginal, !!g_dlcNameOriginal, !!g_menuUpdateTramp, !!g_partRequestOriginal);
}

const uint8_t kPathPrologue[] = {0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x53, 0x55, 0x8b, 0xac, 0x24, 0xe4, 0x00, 0x00, 0x00, 0x56, 0x8b, 0xf1, 0xb8};
const uint32_t kPathRvas[] = {0xb664b0 /* DX11 */, 0xb56650 /* DX9 */};

}

void PartsIndex(std::unordered_map<std::wstring, std::wstring>* index)
{
	for (const PartPackage& pkg : PartPackages()) {
		std::wstring dir = pkg.root + L"Content\\nativePC\\arcXmlTmpPC\\CustomParts\\";
		for (int dset : {0x0e, 0x2e}) {
			for (int c = 1; c < 4; c++) {
				std::string name = std::string(kPrefixes[pkg.slot]) + Hex2(dset) + Hex2(pkg.item) + Hex2(c) + ".arc";
				std::wstring wname = Utf8ToWide(name.c_str());
				(*index)[L"nativepc\\arcxmltmppc\\customparts\\" + wname] = dir + wname;
			}
		}
	}
}

bool PartsCarrierRedirect(const std::wstring& path, std::wstring* out)
{
	// a carrier slot's archive <pfx><0e|2e><slot>[<colour>].arc in its package
	std::wstring lower = LowerW(path);
	if (lower.find(L"\\modcache\\parts\\pkg\\carrier_") == std::wstring::npos) {
		return false;
	}
	std::wstring name = lower.substr(lower.rfind(L'\\') + 1);
	// a player's own set (RequestNameSprintf): <token><set>[<colour>].arc -> that set's build
	if ((name.size() == 12 || name.size() == 14) && name[2] == L'z' && name.compare(name.size() - 4, 4, L".arc") == 0) {
		std::lock_guard<std::mutex> lock(g_remoteMutex);
		for (int s = 0; s < kPartSlotCount; s++) {
			std::vector<const PartPackage*> all;
			for (const PartPackage& pkg : g_carrierSets[s]) {
				all.push_back(&pkg);
			}
			for (const PartPackage& pkg : g_remoteSets[s]) {
				all.push_back(&pkg);
			}
			for (const PartPackage* found : all) {
				const PartPackage& pkg = *found;
				if (pkg.carrierToken.size() == 6 && name.compare(0, 6, Utf8ToWide(pkg.carrierToken.c_str())) == 0) {
					*out = pkg.root + L"Content\\nativePC\\arcXmlTmpPC\\CustomParts\\" + Utf8ToWide(kPrefixes[s]) + name.substr(6, 2) +
					       Utf8ToWide(Hex2(kCarrierItem).c_str()) + name.substr(8);
					static std::atomic<int> logged{0};
					if (logged++ < 64) {
						XLS_LOG_INFO("mods: parts: player set %s %s -> %s", pkg.setId.c_str(), kSlotNames[s], WideToUtf8(out->c_str()).c_str());
					}
					return true;
				}
			}
		}
	}
	if ((name.size() != 10 && name.size() != 12) || name.compare(name.size() - 4, 4, L".arc") != 0 || (name.compare(2, 2, L"0e") != 0 && name.compare(2, 2, L"2e") != 0)) {
		return false;
	}
	int cat = -1;
	for (int s = 0; s < kPartSlotCount; s++) {
		if (name[0] == (wchar_t)kPrefixes[s][0] && name[1] == (wchar_t)kPrefixes[s][1]) {
			cat = s;
		}
	}
	int item = (int)wcstol(name.substr(4, 2).c_str(), nullptr, 16);
	const PartPackage* worn = cat >= 0 && IsCarrierItem(item) ? CarrierWorn(cat, item) : nullptr;
	if (!worn) {
		return false;
	}
	*out = worn->root + L"Content\\nativePC\\arcXmlTmpPC\\CustomParts\\" + name.substr(0, 4) + Utf8ToWide(Hex2(kCarrierItem).c_str()) + name.substr(6);
	static std::atomic<int> logged{0};
	if (logged++ < 64) {
		XLS_LOG_INFO("mods: parts: carrier slot %d %s -> %s", item, kSlotNames[cat], WideToUtf8(out->c_str()).c_str());
	}
	return true;
}

bool PartsTryInstallPathHook(unsigned char* base)
{
	uint8_t* fn = nullptr;
	for (uint32_t rva : kPathRvas) {
		if (memcmp(base + rva, kPathPrologue, sizeof(kPathPrologue)) == 0) {
			fn = base + rva;
		}
	}
	if (!fn) {
		return false;
	}
	const size_t len = 8; // sub esp,0xd0; push ebx; push ebp
	auto* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!tramp) {
		return false;
	}
	memcpy(tramp, fn, len);
	tramp[len] = 0xe9;
	int32_t back = (int32_t)((fn + len) - (tramp + len + 5));
	memcpy(tramp + len + 1, &back, 4);
	FlushInstructionCache(GetCurrentProcess(), tramp, 32);
	g_pathOriginal = (PathFn)tramp;
	uint8_t patch[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
	int32_t rel = (int32_t)((uint8_t*)&PartPath - (fn + 5));
	memcpy(patch + 1, &rel, 4);
	DWORD old = 0;
	if (!VirtualProtect(fn, len, PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	memcpy(fn, patch, len);
	VirtualProtect(fn, len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), fn, len);
	XLS_LOG_INFO("mods: parts: path builder hooked at 0x%p.", fn);
	InstallMenuColours(base, kMenuVas[fn == base + kPathRvas[0] ? 0 : 1]);
	return true;
}

}
