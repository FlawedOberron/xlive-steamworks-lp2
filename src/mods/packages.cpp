#include "mods/packages.h"

#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/utils.h"
#include "mods/package.h"
#include "mods/parts.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <mutex>

// Map packages: Maps\<name>.zip with
//   map.json   {"name": "Test Arena", "size": "medium", "base": 0, "number": 0}
//   nativePC\...  the map's files, named and built for online map <number>
// Each package gets a free online slot (13, then 14). Its files are unpacked into
// ModCache\maps\<name>\ with every map-number reference renumbered to the slot (archive entry
// names and contents: Area_ONLINE_<n>, rader_map_7_<n>), and the overlay serves them from there.
// The map's name goes into MsgOnline 126+slot of every ID_FCORE_<lang>_PC.arc, generated into
// ModCache\generated\ the first time the game opens each one.

namespace xls::mods {

namespace {

const int kFreeSlots[] = {13, 14};
const int kMsgMapName = 126;
// Bumped whenever the unpacking changes, so stale caches are rebuilt.
const char* const kCacheVersion = "lp2-maps 1";

std::once_flag g_once;
std::vector<MapPackage> g_maps;
std::unordered_map<std::wstring, std::wstring> g_files; // lower game-relative path -> cache file
std::mutex g_generatedMutex;
std::map<std::wstring, std::wstring> g_generated; // lower game-relative path -> generated file

std::wstring LowerPath(std::wstring s)
{
	for (wchar_t& c : s) {
		c = c == L'/' ? L'\\' : (wchar_t)towlower(c);
	}
	return s;
}

std::vector<std::wstring> ListFiles(const std::wstring& dir, const wchar_t* pattern)
{
	std::vector<std::wstring> out;
	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW((dir + pattern).c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) {
		return out;
	}
	do {
		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			out.push_back(data.cFileName);
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
	std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
	return out;
}

void IndexTree(const std::wstring& root, const std::wstring& rel)
{
	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW((root + rel + L"*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		std::wstring name = data.cFileName;
		if (name == L"." || name == L"..") {
			continue;
		}
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			IndexTree(root, rel + name + L"\\");
		}
		else {
			g_files[LowerPath(rel + name)] = root + rel + name;
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
}

std::string Stamp(const std::wstring& zip, int slot)
{
	WIN32_FILE_ATTRIBUTE_DATA a{};
	GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &a);
	char buf[160];
	snprintf(buf, sizeof(buf), "%s size %lu:%lu time %lu:%lu slot %d", kCacheVersion, a.nFileSizeHigh, a.nFileSizeLow,
	         a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime, slot);
	return buf;
}

bool Unpack(const Zip& zip, const MapPackage& map, const std::wstring& dir)
{
	int files = 0, refs = 0;
	for (const ZipEntry& e : zip.Entries()) {
		if (_strnicmp(e.name.c_str(), "nativePC/", 9) != 0) {
			continue;
		}
		Bytes data;
		if (!zip.Read(e, &data)) {
			XLS_LOG_WARN("mods: map %ls: cannot read %s from the package.", map.package.c_str(), e.name.c_str());
			return false;
		}
		std::string rel = RenumberName(e.name, map.number, map.slot);
		size_t dot = rel.find_last_of('.');
		if (dot != std::string::npos && _stricmp(rel.c_str() + dot, ".arc") == 0) {
			std::vector<ArcEntry> entries;
			if (!ArcRead(data, &entries)) {
				XLS_LOG_WARN("mods: map %ls: %s is not a readable archive.", map.package.c_str(), e.name.c_str());
				return false;
			}
			for (ArcEntry& entry : entries) {
				entry.name = RenumberName(entry.name, map.number, map.slot);
				refs += Renumber(entry.data.data(), entry.data.size(), map.number, map.slot);
			}
			data = ArcWrite(entries);
		}
		else {
			refs += Renumber(data.data(), data.size(), map.number, map.slot);
		}
		std::wstring out = dir + Utf8ToWide(rel.c_str());
		std::replace(out.begin(), out.end(), L'/', L'\\');
		if (!WriteWholeFile(out, data)) {
			XLS_LOG_WARN("mods: map %ls: cannot write %ls.", map.package.c_str(), out.c_str());
			return false;
		}
		files++;
	}
	XLS_LOG_INFO("mods: map %ls: unpacked %d files for slot %d (%d map-number references renumbered).", map.package.c_str(), files, map.slot, refs);
	return files > 0;
}

void LoadMaps()
{
	std::vector<std::wstring> zips = ListFiles(Cfg().mapsDir, L"*.zip");
	size_t nextSlot = 0;
	for (const std::wstring& file : zips) {
		std::wstring zipPath = Cfg().mapsDir + file;
		MapPackage map;
		map.package = file.substr(0, file.size() - 4);
		Zip zip;
		Bytes text;
		const ZipEntry* manifest = nullptr;
		if (!zip.Open(zipPath) || !(manifest = zip.Find("map.json")) || !zip.Read(*manifest, &text)) {
			XLS_LOG_WARN("mods: map %ls: not a zip with a map.json, skipped.", file.c_str());
			continue;
		}
		Json root;
		std::string error;
		if (!Json::Parse(std::string(text.begin(), text.end()), root, &error)) {
			XLS_LOG_WARN("mods: map %ls: map.json: %s", file.c_str(), error.c_str());
			continue;
		}
		map.name = root["name"].GetString(WideToUtf8(map.package.c_str()));
		map.base = (int)root["base"].GetInt(0);
		map.number = (int)root["number"].GetInt(map.base);
		std::string size = root["size"].GetString(std::string());
		map.size = size == "small" ? 0 : size == "medium" ? 1 : size == "large" ? 2 : -1;
		if (map.number < 0 || map.number > 99) {
			XLS_LOG_WARN("mods: map %ls: \"number\" must be 0-99.", file.c_str());
			continue;
		}
		if (nextSlot >= sizeof(kFreeSlots) / sizeof(kFreeSlots[0])) {
			XLS_LOG_WARN("mods: map %ls: no free map slot left (the game has two), skipped.", file.c_str());
			continue;
		}
		map.slot = kFreeSlots[nextSlot];
		std::wstring dir = Cfg().modCacheDir + L"maps\\" + map.package + L"\\";
		std::string stamp = Stamp(zipPath, map.slot);
		Bytes old;
		bool fresh = ReadWholeFile(dir + L"stamp.txt", &old) && std::string(old.begin(), old.end()) == stamp;
		if (!fresh) {
			if (!Unpack(zip, map, dir)) {
				continue;
			}
			WriteWholeFile(dir + L"stamp.txt", Bytes(stamp.begin(), stamp.end()));
		}
		else {
			XLS_LOG_INFO("mods: map %ls: slot %d, cached.", map.package.c_str(), map.slot);
		}
		IndexTree(dir, L"nativePC\\"); // keys are nativepc\..., like the overlay's
		nextSlot++;
		g_maps.push_back(map);
		XLS_LOG_INFO("mods: map \"%s\" (%ls) in online slot %d.", map.name.c_str(), file.c_str(), map.slot);
	}
}

void Load()
{
	ConfigEnsureLoaded();
	LoadMaps();
	PartsLoad();
}

// MsgOnline with the mod maps' names, for one ID_FCORE_<lang>_PC.arc.
bool GenerateNames(const std::wstring& source, const std::wstring& out)
{
	Bytes file;
	std::vector<ArcEntry> entries;
	if (!ReadWholeFile(source, &file) || !ArcRead(file, &entries)) {
		return false;
	}
	bool changed = false;
	for (ArcEntry& e : entries) {
		if (_stricmp(e.name.c_str(), "message\\msgPC\\MsgOnline") != 0) {
			continue;
		}
		for (const MapPackage& m : g_maps) {
			changed |= MessageSet(&e.data, kMsgMapName + m.slot, m.name);
		}
	}
	return changed && WriteWholeFile(out, ArcWrite(entries));
}

}

void PackagesLoad()
{
	std::call_once(g_once, Load);
}

const std::vector<MapPackage>& PackageMaps()
{
	PackagesLoad();
	return g_maps;
}

void PackagesIndex(std::unordered_map<std::wstring, std::wstring>* index)
{
	PackagesLoad();
	for (const auto& f : g_files) {
		(*index)[f.first] = f.second;
	}
}

const std::wstring* PackagesGenerated(const std::wstring& rel, const std::wstring& source)
{
	PackagesLoad();
	if (g_maps.empty()) {
		return nullptr;
	}
	// nativepc\arclp2xml\id_fcore_<lang>_pc.arc
	static const std::wstring prefix = L"nativepc\\arclp2xml\\id_fcore_";
	if (rel.compare(0, prefix.size(), prefix) != 0 || rel.size() < 7 || rel.compare(rel.size() - 7, 7, L"_pc.arc") != 0) {
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(g_generatedMutex);
	auto it = g_generated.find(rel);
	if (it != g_generated.end()) {
		return it->second.empty() ? nullptr : &it->second;
	}
	std::wstring out = Cfg().modCacheDir + L"generated\\" + rel.substr(rel.find_last_of(L'\\') + 1);
	if (GenerateNames(source, out)) {
		XLS_LOG_INFO("mods: maps: names added to %ls.", rel.c_str());
		return &(g_generated[rel] = out);
	}
	g_generated[rel] = L"";
	return nullptr;
}

}
