#include "mods/parts.h"

#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/utils.h"
#include "mods/package.h"

#include <windows.h>

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

}
