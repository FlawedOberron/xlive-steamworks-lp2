#include "mods/packages.h"

#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/utils.h"
#include "mods/package.h"
#include "mods/lp2.h"
#include "mods/parts.h"
#include "api/xsession.h"
#include "core/memberdata.h"
#include "core/steam.h"
#include "core/content.h"
#include "core/transfer.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>

// Map packages: Maps\<name>.zip with
//   map.json   {"name": "Test Arena", "size": "medium", "base": 0, "number": 0}
//   nativePC\...  the map's files, named and built for online map <number>
//   thumbnail.rTexture  optional: the create-match picture (512x512 DXT5, circle in the alpha like the
//                  stock ones), put over ID221_SC<slot>_ID in a generated aBattlePlayerCreate.arc
// Any number of packages load (each identified by the FNV-1a 64 of its zip); the game has two free
// online map ids (13, 14), so packages are BOUND to those slots at runtime: the first two at
// start, and in a lobby the host's (by hash; downloaded from the host when missing, see
// core/transfer.cpp), our own again after the lobby. A package is unpacked per slot into
// ModCache\maps\<name>\s<slot>\ with every map-number reference renumbered to the slot (archive
// entry names and contents: Area_ONLINE_<n>, rader_map_7_<n>), and the overlay asks
// PackagesMapFile for the bound package's files. The bound map's name goes into MsgOnline
// 126+slot of every ID_FCORE_<lang>_PC.arc and its thumbnail into aBattlePlayerCreate.arc,
// generated into ModCache\generated\ when the game opens them.

namespace xls::mods {

namespace {

const int kSlots[] = {13, 14};
const int kMsgMapName = 126;
// Bumped whenever the unpacking changes, so stale caches are rebuilt.
const char* const kCacheVersion = "lp2-maps 3";
// Largest map package we download from another player.
const uint64_t kMaxMapDownload = 512ull << 20;

struct Binding {
	int package = -1;                                   // index into g_all, -1 = empty
	std::unordered_map<std::wstring, std::wstring> files; // lower game-relative path -> cache file
	std::wstring thumbnail;
};

std::once_flag g_once;
std::recursive_mutex g_mutex;
std::vector<MapPackage> g_all;  // every package: Maps\*.zip, then downloaded ones (slot unused here)
Binding g_bind[2];              // per kSlots entry
int g_default[2] = {-1, -1};    // what each slot holds outside other players' lobbies
std::atomic<bool> g_bound[2];
std::map<uint64_t, std::string> g_memberMaps; // member -> their "maps" member data
std::set<std::string> g_downloading;          // hashes being fetched
bool g_unbound = false;                        // the host's maps aren't all bound here yet
std::map<std::string, DWORD> g_failed;         // hash -> when its download failed (retried after 30 s)
std::mutex g_generatedMutex;
std::map<std::wstring, std::wstring> g_generated; // lower game-relative path + "|" + version -> generated file
std::atomic<int> g_generation{0};                  // bumped on every rebind (names / thumbnails change)

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

void IndexTree(const std::wstring& root, const std::wstring& rel, std::unordered_map<std::wstring, std::wstring>* into)
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
			IndexTree(root, rel + name + L"\\", into);
		}
		else {
			(*into)[LowerPath(rel + name)] = root + rel + name;
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
}

// FNV-1a 64 of a file, 16 hex (as the part set hashes).
std::string HashFile(const std::wstring& path)
{
	HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return std::string();
	}
	uint64_t h = 14695981039346656037ull;
	std::vector<uint8_t> buffer(1 << 20);
	DWORD got = 0;
	while (ReadFile(f, buffer.data(), (DWORD)buffer.size(), &got, nullptr) && got) {
		for (DWORD i = 0; i < got; i++) {
			h = (h ^ buffer[i]) * 1099511628211ull;
		}
	}
	CloseHandle(f);
	return FormatA("%016llx", (unsigned long long)h);
}

std::string Stamp(const MapPackage& map, int slot)
{
	return FormatA("%s hash %s slot %d", kCacheVersion, map.hash.c_str(), slot);
}

// A zip entry name we unpack: "nativePC/..." without "..", drive letters or empty parts.
bool SafeEntryName(const std::string& name)
{
	if (name.find(':') != std::string::npos || name.find('\\') != std::string::npos) {
		return false;
	}
	size_t start = 0;
	while (start <= name.size()) {
		size_t end = name.find('/', start);
		std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (part.empty() || part == "." || part == "..") {
			return end == std::string::npos && part.empty() && start > 0; // a trailing '/' (folder entry) only
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return true;
}

bool Unpack(const Zip& zip, const MapPackage& map, int slot, const std::wstring& dir)
{
	int files = 0, refs = 0;
	for (const ZipEntry& e : zip.Entries()) {
		if (_stricmp(e.name.c_str(), "thumbnail.rTexture") == 0) {
			Bytes data;
			if (!zip.Read(e, &data) || !WriteWholeFile(dir + L"thumbnail.rTexture", data)) {
				XLS_LOG_WARN("mods: map %ls: cannot unpack thumbnail.rTexture.", map.package.c_str());
			}
			continue;
		}
		if (_strnicmp(e.name.c_str(), "nativePC/", 9) != 0 || e.name.back() == '/') {
			continue;
		}
		if (!SafeEntryName(e.name)) {
			XLS_LOG_WARN("mods: map %ls: unsafe file name \"%s\" in the package, not used.", map.package.c_str(), e.name.c_str());
			return false;
		}
		Bytes data;
		if (!zip.Read(e, &data)) {
			XLS_LOG_WARN("mods: map %ls: cannot read %s from the package.", map.package.c_str(), e.name.c_str());
			return false;
		}
		std::string rel = RenumberName(e.name, map.number, slot);
		size_t dot = rel.find_last_of('.');
		if (dot != std::string::npos && _stricmp(rel.c_str() + dot, ".arc") == 0) {
			std::vector<ArcEntry> entries;
			if (!ArcRead(data, &entries)) {
				XLS_LOG_WARN("mods: map %ls: %s is not a readable archive.", map.package.c_str(), e.name.c_str());
				return false;
			}
			for (ArcEntry& entry : entries) {
				entry.name = RenumberName(entry.name, map.number, slot);
				refs += Renumber(entry.data.data(), entry.data.size(), map.number, slot);
			}
			data = ArcWrite(entries);
		}
		else {
			refs += Renumber(data.data(), data.size(), map.number, slot);
		}
		std::wstring out = dir + Utf8ToWide(rel.c_str());
		std::replace(out.begin(), out.end(), L'/', L'\\');
		if (!WriteWholeFile(out, data)) {
			XLS_LOG_WARN("mods: map %ls: cannot write %ls.", map.package.c_str(), out.c_str());
			return false;
		}
		files++;
	}
	XLS_LOG_INFO("mods: map %ls: unpacked %d files for slot %d (%d map-number references renumbered).", map.package.c_str(), files, slot, refs);
	return files > 0;
}

// map.json of a zip into map (name, base, number, size). false = not a map package.
bool ReadManifest(const Zip& zip, const std::wstring& label, MapPackage* map)
{
	Bytes text;
	const ZipEntry* manifest = zip.Find("map.json");
	if (!manifest || !zip.Read(*manifest, &text)) {
		XLS_LOG_WARN("mods: map %ls: not a zip with a map.json, skipped.", label.c_str());
		return false;
	}
	Json root;
	std::string error;
	if (!Json::Parse(std::string(text.begin(), text.end()), root, &error)) {
		XLS_LOG_WARN("mods: map %ls: map.json: %s", label.c_str(), error.c_str());
		return false;
	}
	map->name = root["name"].GetString(WideToUtf8(label.c_str()));
	if (map->name.size() > 60) {
		map->name.resize(60);
	}
	map->base = (int)root["base"].GetInt(0);
	map->number = (int)root["number"].GetInt(map->base);
	std::string size = root["size"].GetString(std::string());
	map->size = size == "small" ? 0 : size == "medium" ? 1 : size == "large" ? 2 : -1;
	if (map->number < 0 || map->number > 99 || map->base < 0 || map->base > 15) {
		XLS_LOG_WARN("mods: map %ls: \"number\" must be 0-99 and \"base\" 0-15.", label.c_str());
		return false;
	}
	return true;
}

// Unpacks (if stale) and indexes package i for slot.
bool Prepare(int i, int slot, Binding* out)
{
	const MapPackage& map = g_all[i];
	std::wstring dir = Cfg().modCacheDir + L"maps\\" + map.package + FormatW(L"\\s%d\\", slot);
	std::string stamp = Stamp(map, slot);
	Bytes old;
	if (!(ReadWholeFile(dir + L"stamp.txt", &old) && std::string(old.begin(), old.end()) == stamp)) {
		Zip zip;
		if (!zip.Open(map.zip) || !Unpack(zip, map, slot, dir)) {
			return false;
		}
		WriteWholeFile(dir + L"stamp.txt", Bytes(stamp.begin(), stamp.end()));
	}
	out->package = i;
	out->files.clear();
	IndexTree(dir, L"nativePC\\", &out->files); // keys are nativepc\..., like the overlay's
	out->thumbnail = GetFileAttributesW((dir + L"thumbnail.rTexture").c_str()) != INVALID_FILE_ATTRIBUTES ? dir + L"thumbnail.rTexture" : std::wstring();
	return true;
}

void PublishMaps();

// Binds package i (-1 = none) to slot k. Only between matches: an area being played keeps the
// files it has open, but loads after this see the new package.
bool Bind(int k, int i, const char* why)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (g_bind[k].package == i) {
		return true;
	}
	Binding b;
	if (i >= 0 && !Prepare(i, kSlots[k], &b)) {
		return false;
	}
	g_bind[k] = std::move(b);
	g_bound[k] = i >= 0;
	g_generation++;
	if (i >= 0) {
		MapsRebind(kSlots[k], g_all[i].base, g_all[i].size);
		XLS_LOG_INFO("mods: map \"%s\" (%ls, %s) in online slot %d (%s).", g_all[i].name.c_str(), g_all[i].package.c_str(), g_all[i].hash.c_str(), kSlots[k], why);
	}
	else {
		XLS_LOG_INFO("mods: online slot %d emptied (%s).", kSlots[k], why);
	}
	PublishMaps();
	return true;
}

int FindHash(const std::string& hash)
{
	for (size_t i = 0; i < g_all.size(); i++) {
		if (g_all[i].hash == hash) {
			return (int)i;
		}
	}
	return -1;
}

void LoadMaps()
{
	for (const std::wstring& file : ListFiles(Cfg().mapsDir, L"*.zip")) {
		MapPackage map;
		map.package = file.substr(0, file.size() - 4);
		map.zip = Cfg().mapsDir + file;
		Zip zip;
		if (!zip.Open(map.zip) || !ReadManifest(zip, file, &map)) {
			continue;
		}
		map.hash = HashFile(map.zip);
		if (FindHash(map.hash) >= 0) {
			XLS_LOG_WARN("mods: map %ls is a copy of %ls, skipped.", file.c_str(), g_all[FindHash(map.hash)].package.c_str());
			continue;
		}
		g_all.push_back(map);
		XLS_LOG_INFO("mods: map package \"%s\" (%ls, %s).", map.name.c_str(), file.c_str(), map.hash.c_str());
	}
	// downloaded earlier (other players' maps): available for their lobbies, never bound by default
	std::wstring remote = Cfg().modCacheDir + L"maps\\remote\\";
	for (const std::wstring& file : ListFiles(remote, L"*.zip")) {
		MapPackage map;
		map.hash = WideToUtf8(file.substr(0, file.size() - 4).c_str());
		map.package = L"remote-" + file.substr(0, file.size() - 4);
		map.zip = remote + file;
		map.remote = true;
		Zip zip;
		if (FindHash(map.hash) < 0 && zip.Open(map.zip) && ReadManifest(zip, file, &map)) {
			g_all.push_back(map);
		}
	}
	int k = 0;
	for (size_t i = 0; i < g_all.size() && k < 2; i++) {
		if (!g_all[i].remote && Bind(k, (int)i, "default")) {
			g_default[k++] = (int)i;
		}
	}
	size_t local = 0;
	for (const MapPackage& m : g_all) {
		local += !m.remote;
	}
	if (local > 2) {
		XLS_LOG_INFO("mods: %zu map packages: the first two are in slots 13 / 14, the others play when a host picks them.", local);
	}
}

// --- Sync: every player publishes "13=<hash>;14=<hash>" (member data "maps"); in a lobby we
// bind what the host has, downloading it from the host when we don't have that package.
std::string MapsValue()
{
	std::string v;
	for (int k = 0; k < 2; k++) {
		if (g_bind[k].package >= 0) {
			v += FormatA("%s%d=%s", v.empty() ? "" : ";", kSlots[k], g_all[g_bind[k].package].hash.c_str());
		}
	}
	return v;
}

void PublishMaps()
{
	xls::MemberDataSet("maps", MapsValue());
}

struct Download {
	uint64_t host;
	std::string hash;
};

DWORD WINAPI DownloadThread(LPVOID param)
{
	std::unique_ptr<Download> d((Download*)param);
	std::wstring path = Cfg().modCacheDir + L"maps\\remote\\" + Utf8ToWide(d->hash.c_str()) + L".zip";
	MapPackage map;
	map.hash = d->hash;
	map.package = L"remote-" + Utf8ToWide(d->hash.c_str());
	map.zip = path;
	map.remote = true;
	Zip zip;
	bool ok = xls::TransferFetch(d->host, "map", d->hash, kMaxMapDownload, path);
	if (ok && HashFile(path) != d->hash) {
		XLS_LOG_WARN("mods: map %s from the host does not match its hash, dropped.", d->hash.c_str());
		ok = false;
	}
	ok = ok && zip.Open(path) && ReadManifest(zip, map.package, &map);
	if (!ok) {
		DeleteFileW(path.c_str());
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (ok && FindHash(d->hash) < 0) {
		g_all.push_back(map);
		XLS_LOG_INFO("mods: got the host's map \"%s\" (%s).", map.name.c_str(), d->hash.c_str());
	}
	g_downloading.erase(d->hash);
	if (!ok) {
		g_failed[d->hash] = GetTickCount();
	}
	return 0;
}

// Once a second: bind the lobby host's maps (or our own outside lobbies).
void Reconcile()
{
	CSteamID lobby = SessionPresenceLobby();
	uint64_t host = 0;
	if (lobby.IsValid() && SteamReady() && SteamMatchmaking()) {
		host = LobbyOwner(SteamMatchmaking(), lobby).ConvertToUint64();
	}
	bool inGame = SessionPresenceInGame(); // before our lock: the session code calls our listener under its own
	if (inGame) {
		return; // never swap files under a running match
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_unbound = false;
	if (!host || host == SteamLocalId().ConvertToUint64()) {
		for (int k = 0; k < 2; k++) {
			Bind(k, g_default[k], "own maps");
		}
		return;
	}
	auto it = g_memberMaps.find(host);
	if (it == g_memberMaps.end()) {
		return;
	}
	for (int k = 0; k < 2; k++) {
		std::string want;
		std::string key = FormatA("%d=", kSlots[k]);
		for (size_t at = 0; at < it->second.size();) {
			size_t end = it->second.find(';', at);
			std::string entry = it->second.substr(at, end == std::string::npos ? std::string::npos : end - at);
			if (entry.compare(0, key.size(), key) == 0) {
				want = entry.substr(key.size());
			}
			at = end == std::string::npos ? it->second.size() : end + 1;
		}
		if (want.empty()) {
			continue; // the host has nothing there: ours stays (the host can't pick it)
		}
		int i = FindHash(want);
		if (i >= 0) {
			Bind(k, i, "the host's");
			continue;
		}
		auto failed = g_failed.find(want);
		if (failed != g_failed.end() && GetTickCount() - failed->second < 30000) {
			continue; // not holding match starts for a map we can't get
		}
		g_failed.erase(want);
		g_unbound = true;
		if (want.size() == 16 && g_downloading.insert(want).second) {
			XLS_LOG_INFO("mods: the host's map for slot %d (%s) is not here; downloading it from the host.", kSlots[k], want.c_str());
			HANDLE t = CreateThread(nullptr, 0, DownloadThread, new Download{host, want}, 0, nullptr);
			if (t) {
				CloseHandle(t);
			}
		}
	}
}

DWORD WINAPI SyncThread(LPVOID)
{
	for (;;) {
		Sleep(1000);
		Reconcile();
	}
}

// What we send others: one of our zips (local or downloaded), by hash.
std::shared_ptr<xls::TransferSource> ServeMap(const std::string& hash)
{
	std::wstring path;
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		int i = FindHash(hash);
		if (i < 0) {
			return nullptr;
		}
		path = g_all[i].zip;
	}
	HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	LARGE_INTEGER size;
	if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size)) {
		if (f != INVALID_HANDLE_VALUE) {
			CloseHandle(f);
		}
		return nullptr;
	}
	auto handle = std::shared_ptr<void>(f, [](void* h) { CloseHandle(h); });
	auto source = std::make_shared<xls::TransferSource>();
	source->size = (uint64_t)size.QuadPart;
	source->read = [handle](uint64_t offset, uint32_t length, uint8_t* out) {
		OVERLAPPED o = {};
		o.Offset = (DWORD)offset;
		o.OffsetHigh = (DWORD)(offset >> 32);
		DWORD got = 0;
		return ReadFile(handle.get(), out, length, &got, &o) && got == length;
	};
	return source;
}

void Load()
{
	ConfigEnsureLoaded();
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		LoadMaps();
	}
	xls::TransferRegister("map", ServeMap);
	xls::ContentAddPending([] {
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		return g_unbound || !g_downloading.empty();
	});
	xls::MemberDataListen("maps", [](uint64_t steamId, XUID, const std::string& value) {
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_memberMaps[steamId] = value;
	});
	PublishMaps();
	HANDLE t = CreateThread(nullptr, 0, SyncThread, nullptr, 0, nullptr);
	if (t) {
		CloseHandle(t);
	}
	PartsLoad();
}

// MsgOnline with the bound maps' names, for one ID_FCORE_<lang>_PC.arc.
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
		for (const MapPackage& m : PackageMaps()) {
			changed |= MessageSet(&e.data, kMsgMapName + m.slot, m.name);
		}
	}
	return changed && WriteWholeFile(out, ArcWrite(entries));
}

// The create-match pictures (ID221_SC<slot>_ID) of the bound maps that ship a thumbnail, for aBattlePlayerCreate.arc.
bool GenerateThumbnails(const std::wstring& source, const std::wstring& out)
{
	Bytes file;
	std::vector<ArcEntry> entries;
	if (!ReadWholeFile(source, &file) || !ArcRead(file, &entries)) {
		return false;
	}
	bool changed = false;
	for (const MapPackage& m : PackageMaps()) {
		Bytes thumb;
		if (m.thumbnail.empty() || !ReadWholeFile(m.thumbnail, &thumb)) {
			continue;
		}
		char want[32];
		snprintf(want, sizeof(want), "ID221_SC%02d_ID", m.slot);
		for (ArcEntry& e : entries) {
			size_t at = e.name.find_last_of('\\');
			if (_stricmp(e.name.c_str() + (at == std::string::npos ? 0 : at + 1), want) == 0) {
				e.data = thumb;
				changed = true;
			}
		}
	}
	return changed && WriteWholeFile(out, ArcWrite(entries));
}

}

void PackagesLoad()
{
	std::call_once(g_once, Load);
}

std::vector<MapPackage> PackageMaps()
{
	PackagesLoad();
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::vector<MapPackage> out;
	for (int k = 0; k < 2; k++) {
		if (g_bind[k].package >= 0) {
			out.push_back(g_all[g_bind[k].package]);
			out.back().slot = kSlots[k];
			out.back().thumbnail = g_bind[k].thumbnail;
		}
	}
	return out;
}

bool PackagesSlotBound(int slot)
{
	for (int k = 0; k < 2; k++) {
		if (kSlots[k] == slot) {
			return g_bound[k];
		}
	}
	return false;
}

bool PackagesMapFile(const std::wstring& rel, std::wstring* out)
{
	PackagesLoad();
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	for (const Binding& b : g_bind) {
		auto it = b.files.find(rel);
		if (it != b.files.end()) {
			*out = it->second;
			return true;
		}
	}
	return false;
}

const std::wstring* PackagesGenerated(const std::wstring& rel, const std::wstring& source)
{
	PackagesLoad();
	// nativepc\arclp2xml\id_fcore_<lang>_pc.arc (names), nativepc\arcxmltmppc\abattleplayercreate.arc (thumbnails)
	static const std::wstring prefix = L"nativepc\\arclp2xml\\id_fcore_";
	static const std::wstring create = L"nativepc\\arcxmltmppc\\abattleplayercreate.arc";
	bool names = rel.compare(0, prefix.size(), prefix) == 0 && rel.size() >= 7 && rel.compare(rel.size() - 7, 7, L"_pc.arc") == 0;
	if (!names && rel != create) {
		return nullptr;
	}
	int generation = g_generation;
	std::wstring key = rel + FormatW(L"|%d", generation);
	std::lock_guard<std::mutex> lock(g_generatedMutex);
	auto it = g_generated.find(key);
	if (it != g_generated.end()) {
		return it->second.empty() ? nullptr : &it->second; // entries are never erased: pointers stay valid
	}
	std::wstring out = Cfg().modCacheDir + FormatW(L"generated\\g%d_", generation) + rel.substr(rel.find_last_of(L'\\') + 1);
	if (names ? GenerateNames(source, out) : GenerateThumbnails(source, out)) {
		XLS_LOG_INFO("mods: maps: %s added to %ls.", names ? "names" : "thumbnails", rel.c_str());
		return &(g_generated[key] = out);
	}
	g_generated[key] = L"";
	return nullptr;
}

}
