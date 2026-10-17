#include "mods/parts.h"

#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/utils.h"
#include "mods/package.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <algorithm>
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
	std::string colour = root["colour"].GetString("A");
	set->colour = colour.size() == 1 && toupper((unsigned char)colour[0]) >= 'A' && toupper((unsigned char)colour[0]) <= 'D' ? toupper((unsigned char)colour[0]) - 'A' : 0;
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

void LoadFolder(const std::wstring& dir, const std::wstring& label)
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
		count++;
	}
	if (count == 0) {
		XLS_LOG_WARN("mods: parts %ls: no usable parts, skipped.", label.c_str());
		return;
	}
	for (const PartSet& other : g_sets) {
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
	XLS_LOG_INFO("mods: part set \"%s\" (%s, %s): %s.", set.name.c_str(), set.id.c_str(), set.faction.c_str(), slots.c_str());
	g_sets.push_back(set);
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
const int kLastItem = 69;
const char* const kGenVersion = "lp2-partpkg 2";

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
              const std::vector<std::pair<std::string, const Bytes*>>& textures = {})
{
	Bytes file;
	std::vector<ArcEntry> entries;
	if (!ReadWholeFile(src, &file) || !ArcRead(file, &entries)) {
		return false;
	}
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

std::string SnameBin(int slot, int item, const std::string& name)
{
	std::string t = "\xef\xbb\xbf" "01\r\n";
	char line[16];
	snprintf(line, sizeof(line), "%02d\r\n%02d\r\n", kSnameCategory[slot], item);
	t += line;
	for (int i = 0; i < 7; i++) {
		t += name + "\r\n";
	}
	for (int i = 0; i < kSnameFloats[slot]; i++) {
		t += "0\r\n";
	}
	return t;
}

std::string FileStamp(const std::wstring& path)
{
	WIN32_FILE_ATTRIBUTE_DATA a{};
	GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
	char buf[96];
	snprintf(buf, sizeof(buf), "%lu:%lu %lu:%lu", a.nFileSizeHigh, a.nFileSizeLow, a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
	return buf;
}

bool BuildPackage(const PartSet& set, int slot, int item, PartPackage* out)
{
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
	std::wstring srcDir = ExeDirectory() + L"nativePC\\arcXmlTmpPC\\CustomParts\\";
	std::wstring root = Cfg().modCacheDir + L"parts\\pkg\\" + Utf8ToWide(set.id.c_str()) + L"_" + Utf8ToWide(kSlotNames[slot]) + L"\\";
	std::wstring dstDir = root + L"Content\\nativePC\\arcXmlTmpPC\\CustomParts\\";
	char stampBuf[256];
	snprintf(stampBuf, sizeof(stampBuf), "%s item %d colour %d template %s model %s", kGenVersion, item, set.colour, tmpl.c_str(), FileStamp(set.models[slot]).c_str());
	std::string stamp = stampBuf;
	for (const auto& t : set.textures[slot]) {
		stamp += " " + t.first + "=" + FileStamp(t.second);
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
		for (int dset : {0x0e, 0x2e}) {
			std::string d = Hex2(dset), dv = Hex2(item);
			std::vector<std::pair<std::string, std::string>> pairs = {
				{modelName, "model\\hm\\hmf6\\" + pfx + "f6" + dv + "\\" + pfx + "f6" + dv},
				{"hm" + sset + "\\" + tmpl, "hm" + d + "\\" + pfx + d + dv},
				{tmpl, pfx + d + dv},
			};
			std::wstring tag = Utf8ToWide((pfx + d + dv).c_str());
			if (!CloneArc(srcDir + Utf8ToWide(tmpl.c_str()) + L".arc", dstDir + tag + L".arc", pairs, modelName, &model, std::string(), mainTex)) {
				XLS_LOG_WARN("mods: parts %s: %s: cannot clone %s.arc.", set.id.c_str(), kSlotNames[slot], tmpl.c_str());
				return false;
			}
			// colour: the chosen variant's texture archive becomes colour A
			std::wstring colourSrc = srcDir + Utf8ToWide(tmpl.c_str()) + Utf8ToWide(Hex2(set.colour).c_str()) + L".arc";
			std::string from = std::string("_BM-00_") + kColours[set.colour];
			if (!CloneArc(colourSrc, dstDir + tag + L"00.arc", pairs, std::string(), nullptr, set.colour ? from : std::string(), colourTex)) {
				XLS_LOG_WARN("mods: parts %s: %s: no colour %c archive for %s.", set.id.c_str(), kSlotNames[slot], kColours[set.colour], tmpl.c_str());
			}
		}
		std::string sname = SnameBin(slot, item, set.name);
		WriteWholeFile(root + L"Content\\nativePC\\sname.bin", Bytes(sname.begin(), sname.end()));
		WriteWholeFile(root + L"stamp.txt", Bytes(stamp.begin(), stamp.end()));
		XLS_LOG_INFO("mods: parts %s: %s built as DLC slot %d (template %s, colour %c).", set.id.c_str(), kSlotNames[slot], item, tmpl.c_str(), kColours[set.colour]);
	}
	out->setId = set.id;
	out->slot = slot;
	out->item = item;
	out->root = root;
	out->displayName = Utf8ToWide((set.name + " (" + kSlotNames[slot] + ")").c_str());
	std::string key = std::string("lpc:") + set.id + ":" + kSlotNames[slot];
	uint64_t h = Fnv1a64(key.data(), key.size());
	uint32_t h2 = Fnv1a32(key.data(), key.size());
	memcpy(out->contentId, "LPC", 3);
	memcpy(out->contentId + 4, &h, 8);
	memcpy(out->contentId + 12, &h2, 4);
	return true;
}

void BuildPackages()
{
	PartsLoad();
	// Sets in id order, one DLC slot each (all of a set's parts share it); slots stay stable
	// while the folder doesn't change (multiplayer will need them agreed between players).
	std::vector<const PartSet*> order;
	for (const PartSet& s : g_sets) {
		order.push_back(&s);
	}
	std::sort(order.begin(), order.end(), [](const PartSet* a, const PartSet* b) { return _stricmp(a->id.c_str(), b->id.c_str()) < 0; });
	int item = Cfg().partsFirstSlot;
	for (const PartSet* set : order) {
		if (item > kLastItem) {
			XLS_LOG_WARN("mods: parts: more sets than DLC slots %d-%d, \"%s\" and later are left out.", Cfg().partsFirstSlot, kLastItem, set->id.c_str());
			break;
		}
		for (int s = 0; s < kPartSlotCount; s++) {
			PartPackage pkg;
			if (!set->models[s].empty() && BuildPackage(*set, s, item, &pkg)) {
				g_packages.push_back(pkg);
			}
		}
		item++;
	}
	XLS_LOG_INFO("mods: parts: %zu part package(s) ready.", g_packages.size());
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

}
