#include <new>
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
#include "core/net.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>

// Map packages: Maps\<name>.zip with
//   map.json   {"name": "Test Arena", "size": "medium", "base": 0, "number": 0, "preload": ["uVs50"]}
//              preload: archives in arcLp2Xml\autoArc the map needs beyond its base map's (VS, weapons it places)
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

// A slot's package: light (picked in the menu: its name, picture, base, size; files listed, not unpacked) or full
// (unpacked for a match: in a lobby, or when the game opens one of its files).
struct Binding {
	int package = -1;                                   // index into g_all, -1 = empty
	bool full = false;
	std::unordered_map<std::wstring, std::wstring> files; // lower game-relative path -> cache file ("" while light)
	std::wstring thumbnail;
};

std::once_flag g_once;
std::recursive_mutex g_mutex;
std::vector<MapPackage> g_all;  // every package: Maps\*.zip, then downloaded ones (slot unused here)
Binding g_bind[2];              // per kSlots entry
int g_default[2] = {-1, -1};    // what each slot holds outside other players' lobbies
std::atomic<bool> g_bound[2];
std::atomic<int> g_picked{-1};  // picked in create match, bound by the sync thread (unpacking takes a while)
std::atomic<DWORD> g_pickedAt{0}; // when: a pick is bound once left alone a moment, not every map scrolled past
std::map<uint64_t, std::string> g_memberMaps; // member -> their "maps" member data
std::map<uint64_t, std::string> g_memberNames; // member -> their "mapnames" member data
std::atomic<uint64_t> g_lobbyHost{0};          // another player's lobby we're in (0: none / our own)
std::set<std::string> g_downloading;          // hashes being fetched
bool g_unbound = false;                        // the host's maps aren't all bound here yet
std::map<std::string, DWORD> g_failed;         // hash -> when its download failed (retried after 30 s)
std::mutex g_generatedMutex;
std::map<std::wstring, std::wstring> g_generated; // lower game-relative path + "|" + version -> generated file
std::atomic<int> g_generation{0};                  // bumped on every rebind (names / thumbnails change)
std::atomic<bool> g_picDirty[2];                   // a rebind since the slot's picture was last written (mappics.cpp)
std::mutex g_stockPicsMutex;
Bytes g_stockPics[14];                              // the stock create-match pictures (ID221_SC00 .. SC13), once seen
std::mutex g_unpackMutex;                           // one unpack at a time (the sync thread, the game's loader); never take g_mutex under it
std::atomic<bool> g_unpacking{false};               // our lobby's map being unpacked: match starts wait

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

// HashFile, remembered by file name, size and time (ModCache\maps\hashes.txt): the zips aren't read through on
// every start.
std::string CachedHash(const std::wstring& path, const std::wstring& name, std::map<std::wstring, std::string>* cache, bool* changed)
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) {
		return std::string();
	}
	std::wstring key = FormatW(L"%lu.%lu.%lu.%lu|", a.nFileSizeHigh, a.nFileSizeLow, a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime) + LowerPath(name);
	auto it = cache->find(key);
	if (it != cache->end()) {
		return it->second;
	}
	std::string hash = HashFile(path);
	if (!hash.empty()) {
		(*cache)[key] = hash;
		*changed = true;
	}
	return hash;
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
			Bytes().swap(data); // the packed archive, before the rewritten one is written (address space)
			std::wstring out = dir + Utf8ToWide(rel.c_str());
			std::replace(out.begin(), out.end(), L'/', L'\\');
			if (!ArcWriteFile(out, entries)) {
				XLS_LOG_WARN("mods: map %ls: cannot write %ls.", map.package.c_str(), out.c_str());
				return false;
			}
			files++;
			continue;
		}
		refs += Renumber(data.data(), data.size(), map.number, slot);
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
	map->preload.clear();
	for (const Json& e : root["preload"].Elements()) {
		std::string arc = e.GetString(std::string());
		bool ok = arc.size() >= 3 && arc.size() <= 32 && arc.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos;
		if (ok) {
			map->preload.push_back(arc);
		}
		else {
			XLS_LOG_WARN("mods: map %ls: preload entry \"%s\" skipped (an archive name in arcLp2Xml\\autoArc).", label.c_str(), arc.c_str());
		}
	}
	map->physics = MapPhysics();
	bool lowGravity = root["low_gravity"].GetBool(false);
	map->physics.jumpPacks = root["jump_packs"].GetBool(false) || lowGravity;
	map->physics.underwater = root["underwater"].GetBool(false);
	map->physics.gravity = (float)root["gravity"].GetNumber(1.0) * (lowGravity ? 0.3f : 1.0f);
	for (const Json& e : root["gravity_volumes"].Elements()) {
		GravityVolume v;
		bool ok = e["min"].Elements().size() == 3 && e["max"].Elements().size() == 3;
		for (int i = 0; ok && i < 3; i++) {
			v.min[i] = (float)e["min"].Elements()[i].GetNumber(0.0);
			v.max[i] = (float)e["max"].Elements()[i].GetNumber(0.0);
		}
		v.gravity = (float)e["gravity"].GetNumber(1.0);
		SetTurn(v.turn, e["yaw"].GetNumber(0.0));
		v.sid = (int)e["sid"].GetInt(-1);
		if (ok) {
			map->physics.volumes.push_back(v);
		}
	}
	for (const Json& e : root["water_volumes"].Elements()) {
		WaterVolume v;
		bool ok = e["min"].Elements().size() == 3 && e["max"].Elements().size() == 3;
		for (int i = 0; ok && i < 3; i++) {
			v.min[i] = (float)e["min"].Elements()[i].GetNumber(0.0);
			v.max[i] = (float)e["max"].Elements()[i].GetNumber(0.0);
		}
		SetTurn(v.turn, e["yaw"].GetNumber(0.0));
		v.sid = (int)e["sid"].GetInt(-1);
		if (ok) {
			map->physics.water.push_back(v);
		}
	}
	for (const Json& e : root["damage_volumes"].Elements()) {
		DamageVolume v;
		bool ok = e["min"].Elements().size() == 3 && e["max"].Elements().size() == 3;
		for (int i = 0; ok && i < 3; i++) {
			v.min[i] = (float)e["min"].Elements()[i].GetNumber(0.0);
			v.max[i] = (float)e["max"].Elements()[i].GetNumber(0.0);
		}
		v.player = (float)e["player"].GetNumber(0.0);
		v.vs = (float)e["vs"].GetNumber(0.0);
		v.playerKill = e["player_kill"].GetBool(false);
		v.vsKill = e["vs_kill"].GetBool(false);
		SetTurn(v.turn, e["yaw"].GetNumber(0.0));
		v.sid = (int)e["sid"].GetInt(-1);
		if (ok) {
			map->physics.damage.push_back(v);
		}
	}
	for (const Json& e : root["oneway_volumes"].Elements()) {
		OnewayVolume v;
		bool ok = e["min"].Elements().size() == 3 && e["max"].Elements().size() == 3;
		for (int i = 0; ok && i < 3; i++) {
			v.min[i] = (float)e["min"].Elements()[i].GetNumber(0.0);
			v.max[i] = (float)e["max"].Elements()[i].GetNumber(0.0);
		}
		const char* names[] = {"players", "vs", "bullets", "grenades", "akrid"};
		for (int i = 0; i < 5; i++) {
			v.flags |= e[names[i]].GetBool(false) ? 1 << i : 0;
		}
		SetTurn(v.turn, e["yaw"].GetNumber(0.0));
		v.sid = (int)e["sid"].GetInt(-1);
		if (ok && v.flags) {
			map->physics.oneway.push_back(v);
		}
	}
	for (const Json& e : root["force_volumes"].Elements()) {
		ForceVolume v;
		bool ok = e["min"].Elements().size() == 3 && e["max"].Elements().size() == 3 && e["force"].Elements().size() == 3;
		for (int i = 0; ok && i < 3; i++) {
			v.min[i] = (float)e["min"].Elements()[i].GetNumber(0.0);
			v.max[i] = (float)e["max"].Elements()[i].GetNumber(0.0);
			v.force[i] = (float)e["force"].Elements()[i].GetNumber(0.0);
		}
		const char* names[] = {"players", "vs", "bullets", "grenades", "akrid"};
		for (int i = 0; i < 5; i++) {
			v.flags |= e[names[i]].GetBool(false) ? 1 << i : 0;
		}
		const std::string mode = e["mode"].GetString("continuous");
		v.mode = mode == "impulse" ? kForceImpulse : mode == "velocity" ? kForceVelocity : kForceContinuous;
		SetTurn(v.turn, e["yaw"].GetNumber(0.0));
		v.sid = (int)e["sid"].GetInt(-1);
		if (ok && v.flags) {
			map->physics.force.push_back(v);
		}
	}
	for (const Json& e : root["post_volumes"].Elements()) {
		PostVolume v;
		bool ok = e["min"].Elements().size() == 3 && e["max"].Elements().size() == 3;
		for (int i = 0; ok && i < 3; i++) {
			v.min[i] = (float)e["min"].Elements()[i].GetNumber(0.0);
			v.max[i] = (float)e["max"].Elements()[i].GetNumber(0.0);
			v.tint[i] = i < (int)e["tint"].Elements().size() ? (float)e["tint"].Elements()[i].GetNumber(1.0) : 1.0f;
		}
		v.brightness = (float)e["brightness"].GetNumber(1.0);
		v.saturation = (float)e["saturation"].GetNumber(1.0);
		v.contrast = (float)e["contrast"].GetNumber(1.0);
		v.fade = (float)e["fade"].GetNumber(0.5);   // packages from before "fade"
		SetTurn(v.turn, e["yaw"].GetNumber(0.0));
		v.sid = (int)e["sid"].GetInt(-1);
		if (ok) {
			map->physics.post.push_back(v);
		}
	}
	ScriptGraph& sc = map->physics.script;
	const Json& js = root["script"];
	sc.volumes = (int)js["volumes"].GetInt(0);
	sc.vars = (int)js["vars"].GetInt(0);
	for (const Json& e : js["start_off"].Elements()) {
		sc.startOff.push_back((int)e.GetInt(-1));
	}
	for (const Json& e : js["var_names"].Elements()) {
		sc.varNames.push_back(e.GetString(std::string()));
	}
	for (const Json& e : js["var_kinds"].Elements()) {
		std::string k = e.GetString(std::string());
		sc.varKinds.push_back(k == "player" ? kVarPlayer : k == "list" ? kVarList : k == "marker" ? kVarMarker
			: k == "markers" ? kVarMarkers : k == "vector" ? kVarVector : kVarNumber);
	}
	auto operand = [](const Json& j) {
		ScriptOperand o;
		if (j.IsObject()) {
			o.var = (int)j["var"].GetInt(-1);
		}
		else {
			o.num = (float)j.GetNumber(0.0);
		}
		return o;
	};
	std::vector<std::vector<std::pair<int, std::string>>> ins; // per node, per output port: (target, its input name)
	for (const Json& e : js["nodes"].Elements()) {
		ScriptNode n;
		std::string type = e["type"].GetString(std::string());
		n.type = -1;
		for (int t = 0; t < kScriptTypes; t++) {
			if (type == kScriptTypeInfo[t].name) {
				n.type = t;
			}
		}
		if (n.type < 0) {
			XLS_LOG_WARN("mods: map %ls: script node type \"%s\" unknown (a newer editor?): it does nothing.", label.c_str(), type.c_str());
			n.type = kScriptLog;
		}
		n.sid = (int)e["sid"].GetInt(-1);
		std::string who = e["who"].GetString("any");
		n.who = who == "player" ? 1 : who == "vs" ? 2 : 0;
		n.var = (int)e["var"].GetInt(-1);
		n.toggle = e["toggle"].GetBool(false);
		if (n.type == kScriptSet && !e.Has("a") && !n.toggle) {   // older maps: "value" on / off / toggle
			std::string value = e["value"].GetString("toggle");
			n.toggle = value == "toggle";
			n.a.num = value == "on" ? 1.0f : 0.0f;
		}
		n.a = e.Has("a") ? operand(e["a"]) : n.a;
		n.b = operand(e["b"]);
		std::string op = e["op"].GetString("+");
		for (int k = 0; k < kOps; k++) {
			if (op == kScriptOps[k]) {
				n.op = k;
			}
		}
		std::string setting = e["setting"].GetString(std::string());
		for (int k = 0; k < kSettings; k++) {
			if (setting == kScriptSettings[k]) {
				n.setting = k;
			}
		}
		n.integer = e["integer"].GetBool(false);
		n.player = (int)e["player"].GetInt(-1);
		n.list = (int)e["list"].GetInt(-1);
		n.random = e["random"].GetBool(false);
		n.spawnKind = e["kind"].GetString("weapon") == "vs" ? 2 : 4;
		n.marker = (int)e["marker"].GetInt(-1);
		n.face = e["face"].GetBool(true);
		for (const Json& c : e["cases"].Elements()) {
			n.cases.push_back((float)c.GetNumber(0.0));
		}
		for (const Json& k : e["items"].Elements()) {
			n.items.push_back((int)k.GetInt(-1));
		}
		auto pick = [](const std::string& v, const char* const* names, int count, int fallback) {
			for (int k = 0; k < count; k++) {
				if (v == names[k]) {
					return k;
				}
			}
			return fallback;
		};
		n.check = pick(e["check"].GetString(std::string()), kScriptChecks, kChecks, kCheckGone);
		n.stat = pick(e["stat"].GetString(std::string()), kScriptStats, kStats, kStatHealth);
		n.action = pick(e["action"].GetString(std::string()), kScriptActions, kActions, kActDamage);
		n.vec = (int)e["vec"].GetInt(-1);
		n.fromMarker = e["from"].GetString("player") == "marker";
		n.vecOp = pick(e["vecop"].GetString(std::string()), kScriptVecOps, kVecOps, kVecAdd);
		n.target = (int)e["target"].GetInt(1);
		n.seconds = (float)e["seconds"].GetNumber(n.type == kScriptSetValue ? 0.0 : 1.0);
		n.maxSeconds = (float)e["max_seconds"].GetNumber(0.0);
		n.repeat = e["repeat"].GetBool(false);
		n.autostart = e["autostart"].GetBool(false);
		n.text = e["text"].GetString(std::string());
		ins.emplace_back();
		for (const auto& port : e["out"].Items()) {
			for (int k = 0; k < kScriptOuts; k++) {
				const char* name = kScriptTypeInfo[n.type].outs[k];
				if (!name || port.first != name) {
					continue;
				}
				for (const Json& l : port.second.Elements()) {
					n.out[k].push_back({(int)l.At(0).GetInt(-1), -1});
					ins.back().emplace_back(k, l.At(1).GetString(std::string()));
				}
			}
		}
		sc.nodes.push_back(n);
	}
	// links' input ports by name, from their target's type
	for (size_t i = 0; i < sc.nodes.size(); i++) {
		ScriptNode& n = sc.nodes[i];
		std::vector<ScriptLink> out[kScriptOuts];
		size_t at[kScriptOuts] = {};
		for (const auto& in : ins[i]) {
			ScriptLink link = n.out[in.first][at[in.first]++];
			if (link.node < 0 || link.node >= (int)sc.nodes.size()) {
				continue;
			}
			const ScriptTypeInfo& t = kScriptTypeInfo[sc.nodes[link.node].type];
			for (int k = 0; k < kScriptIns; k++) {
				if (t.ins[k] && in.second == t.ins[k]) {
					link.in = k;
				}
			}
			if (link.in >= 0) {
				out[in.first].push_back(link);
			}
		}
		for (int k = 0; k < kScriptOuts; k++) {
			n.out[k] = out[k];
		}
	}
	for (const Json& e : root["script_volumes"].Elements()) {
		ScriptBox b;
		bool ok = e["min"].Elements().size() == 3 && e["max"].Elements().size() == 3;
		for (int i = 0; ok && i < 3; i++) {
			b.min[i] = (float)e["min"].Elements()[i].GetNumber(0.0);
			b.max[i] = (float)e["max"].Elements()[i].GetNumber(0.0);
		}
		SetTurn(b.turn, e["yaw"].GetNumber(0.0));
		b.sid = (int)e["sid"].GetInt(-1);
		if (ok && b.sid >= 0) {
			sc.boxes.push_back(b);
		}
	}
	for (const Json& e : root["script_markers"].Elements()) {
		ScriptMarker m;
		m.name = e["name"].GetString(std::string());
		for (char& c : m.name) {
			c = (char)tolower((unsigned char)c);
		}
		for (int i = 0; i < 3; i++) {
			m.pos[i] = (float)e["pos"].At(i).GetNumber(0.0);
			m.size[i] = (float)e["size"].At(i).GetNumber(100.0);
		}
		m.yaw = (float)e["yaw"].GetNumber(0.0);
		sc.markers.push_back(m);
	}
	std::string size = root["size"].GetString(std::string());
	map->size = size == "small" ? 0 : size == "medium" ? 1 : size == "large" ? 2 : -1;
	if (map->number < 0 || map->number > 99 || map->base < 0 || map->base > 15) {
		XLS_LOG_WARN("mods: map %ls: \"number\" must be 0-99 and \"base\" 0-15.", label.c_str());
		return false;
	}
	return true;
}

std::wstring UnpackDir(const MapPackage& map, int slot)
{
	return Cfg().modCacheDir + L"maps\\" + map.package + FormatW(L"\\s%d\\", slot);
}

// Unpacks the package for slot unless that's done already.
bool Unpacked(const MapPackage& map, int slot)
{
	std::lock_guard<std::mutex> unpacking(g_unpackMutex);
	std::wstring dir = UnpackDir(map, slot);
	std::string stamp = Stamp(map, slot);
	Bytes old;
	if (!(ReadWholeFile(dir + L"stamp.txt", &old) && std::string(old.begin(), old.end()) == stamp)) {
		Zip zip;
		bool ok = false;
		try {
			ok = zip.Open(map.zip) && Unpack(zip, map, slot, dir);
		}
		catch (const std::bad_alloc&) {
			// a 32-bit game on Windows has 2 GB: a big map can find no room while another is loaded
			XLS_LOG_ERROR("mods: map %ls: out of memory unpacking it for slot %d (restarting the game frees it up).", map.package.c_str(), slot);
		}
		if (!ok) {
			return false;
		}
		WriteWholeFile(dir + L"stamp.txt", Bytes(stamp.begin(), stamp.end()));
	}
	return true;
}

// Unpacks (if stale) and indexes package i for slot.
bool Prepare(int i, int slot, Binding* out)
{
	const MapPackage& map = g_all[i];
	std::wstring dir = UnpackDir(map, slot);
	if (!Unpacked(map, slot)) {
		return false;
	}
	out->package = i;
	out->full = true;
	out->files.clear();
	IndexTree(dir, L"nativePC\\", &out->files); // keys are nativepc\..., like the overlay's
	out->thumbnail = GetFileAttributesW((dir + L"thumbnail.rTexture").c_str()) != INVALID_FILE_ATTRIBUTES ? dir + L"thumbnail.rTexture" : std::wstring();
	return true;
}

// Package i's names and thumbnail for slot, without unpacking: the thumbnail alone into the package's cache folder.
bool Light(int i, int slot, Binding* out)
{
	const MapPackage& map = g_all[i];
	Zip zip;
	if (!zip.Open(map.zip)) {
		XLS_LOG_WARN("mods: map %ls: cannot read the package.", map.package.c_str());
		return false;
	}
	out->package = i;
	out->full = false;
	out->files.clear();
	out->thumbnail.clear();
	for (const ZipEntry& e : zip.Entries()) {
		if (_strnicmp(e.name.c_str(), "nativePC/", 9) == 0 && SafeEntryName(e.name)) {
			out->files[LowerPath(Utf8ToWide(RenumberName(e.name, map.number, slot).c_str()))] = std::wstring();
		}
	}
	if (const ZipEntry* e = zip.Find("thumbnail.rTexture")) {
		std::wstring thumb = Cfg().modCacheDir + L"maps\\" + map.package + L"\\thumb-" + Utf8ToWide(map.hash.c_str()) + L".rTexture";
		Bytes data;
		if (GetFileAttributesW(thumb.c_str()) != INVALID_FILE_ATTRIBUTES || (zip.Read(*e, &data) && WriteWholeFile(thumb, data))) {
			out->thumbnail = thumb;
		}
	}
	return !out->files.empty();
}

void PublishMaps();

// Binds package i (-1 = none) to slot k, light or full (a light binding of the same package is made full). Only between matches: an area being played keeps the
// files it has open, but loads after this see the new package.
bool Bind(int k, int i, const char* why, bool full)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (g_bind[k].package == i && (g_bind[k].full || !full || i < 0)) {
		return true;
	}
	Binding b;
	if (i >= 0 && !(full ? Prepare(i, kSlots[k], &b) : Light(i, kSlots[k], &b))) {
		return false;
	}
	if (g_bind[k].package == i) {
		g_bind[k] = std::move(b);
		XLS_LOG_INFO("mods: map \"%s\" unpacked for online slot %d (%s).", g_all[i].name.c_str(), kSlots[k], why);
		return true;
	}
	g_bind[k] = std::move(b);
	g_bound[k] = i >= 0;
	g_generation++;
	g_picDirty[k] = true;
	if (i >= 0) {
		MapsRebind(kSlots[k], g_all[i].base, g_all[i].size, g_all[i].preload);
		XLS_LOG_INFO("mods: map \"%s\" (%ls, %s) in online slot %d (%s%s).", g_all[i].name.c_str(), g_all[i].package.c_str(), g_all[i].hash.c_str(), kSlots[k], why,
		             g_bind[k].full ? ", unpacked" : "");
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
	std::map<std::wstring, std::string> hashes;
	bool hashesChanged = false;
	std::wstring hashesFile = Cfg().modCacheDir + L"maps\\hashes.txt";
	Bytes saved;
	if (ReadWholeFile(hashesFile, &saved)) {
		std::string text(saved.begin(), saved.end());
		for (size_t at = 0; at < text.size();) {
			size_t end = text.find('\n', at);
			std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
			size_t space = line.find(' ');
			if (space == 16) {
				hashes[Utf8ToWide(line.substr(17).c_str())] = line.substr(0, 16);
			}
			at = end == std::string::npos ? text.size() : end + 1;
		}
	}
	for (const std::wstring& file : ListFiles(Cfg().mapsDir, L"*.zip")) {
		MapPackage map;
		map.package = file.substr(0, file.size() - 4);
		map.zip = Cfg().mapsDir + file;
		Zip zip;
		if (!zip.Open(map.zip) || !ReadManifest(zip, file, &map)) {
			continue;
		}
		map.hash = CachedHash(map.zip, file, &hashes, &hashesChanged);
		if (map.hash.empty()) {
			continue;
		}
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
		map.hash = CacheNameHash(file.substr(0, file.size() - 4)); // Map_<name>_<hash>.zip (older builds: <hash>.zip)
		map.package = L"remote-" + Utf8ToWide(map.hash.c_str());
		map.zip = remote + file;
		map.remote = true;
		Zip zip;
		if (FindHash(map.hash) < 0 && zip.Open(map.zip) && ReadManifest(zip, file, &map)) {
			g_all.push_back(map);
		}
	}
	if (hashesChanged) {
		std::string text;
		for (const auto& h : hashes) {
			text += h.second + " " + WideToUtf8(h.first.c_str()) + "\n";
		}
		WriteWholeFile(hashesFile, Bytes(text.begin(), text.end()));
	}
	// slot 13 holds the last package picked in create match (or the first one); 14 only the hosts'
	std::string last;
	if (ReadWholeFile(Cfg().modCacheDir + L"maps\\last.txt", &saved)) {
		last.assign(saved.begin(), saved.end());
		last = last.substr(0, last.find_first_of("\r\n "));
	}
	int pick = FindHash(last);
	if (pick < 0 || g_all[pick].remote) {
		pick = -1;
		for (size_t i = 0; i < g_all.size() && pick < 0; i++) {
			pick = g_all[i].remote ? -1 : (int)i;
		}
	}
	if (pick >= 0 && Bind(0, pick, "default", false)) {
		g_default[0] = pick;
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

// "13=<name>\n14=<name>": the bound maps' names, for players who don't have them (member data
// "mapnames" in lobbies, our QoS answers for the session browser).
std::string NamesValue()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::string v;
	for (int k = 0; k < 2; k++) {
		if (g_bind[k].package >= 0) {
			std::string name = g_all[g_bind[k].package].name;
			std::replace(name.begin(), name.end(), '\n', ' ');
			v += FormatA("%s%d=%s", v.empty() ? "" : "\n", kSlots[k], name.substr(0, 100).c_str());
		}
	}
	return v;
}

void PublishMaps()
{
	xls::MemberDataSet("maps", MapsValue());
	xls::MemberDataSet("mapnames", NamesValue());
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
	else {
		// kept as Map_<name>_<hash>.zip, so the cache folder says what each one is
		std::wstring named = Cfg().modCacheDir + L"maps\\remote\\" + CacheName("Map", map.name, d->hash) + L".zip";
		if (MoveFileExW(path.c_str(), named.c_str(), MOVEFILE_REPLACE_EXISTING)) {
			map.zip = named;
		}
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

// Once a second (a pick: soon after): bind the lobby host's maps (or our own outside lobbies).
void Reconcile()
{
	CSteamID lobby = SessionPresenceLobby();
	uint64_t host = 0;
	if (lobby.IsValid() && SteamReady() && SteamMatchmaking()) {
		host = LobbyOwner(SteamMatchmaking(), lobby).ConvertToUint64();
	}
	bool inGame = SessionPresenceInGame(); // before our lock: the session code calls our listener under its own
	g_lobbyHost = host == SteamLocalId().ConvertToUint64() ? 0 : host;
	if (inGame) {
		return; // never swap files under a running match
	}
	// our own lobby: the map unpacked for the match; in the menu only its name and picture (light)
	bool full = lobby.IsValid();
	if (!host || host == SteamLocalId().ConvertToUint64()) {
		int picked = g_picked;
		if (picked >= 0 && GetTickCount() - g_pickedAt < 300) {
			return; // still scrolling
		}
		// unzip before taking the lock: the menu asks for names (under it) every frame
		MapPackage want;
		bool unzip = false;
		{
			std::lock_guard<std::recursive_mutex> lock(g_mutex);
			if (full && g_default[0] >= 0 && !(g_bind[0].package == g_default[0] && g_bind[0].full)) {
				want = g_all[g_default[0]];
				unzip = true;
			}
		}
		if (unzip) {
			g_unpacking = true;
			Unpacked(want, kSlots[0]);
		}
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_unbound = false;
	if (!host || host == SteamLocalId().ConvertToUint64()) {
		int picked = g_picked;
		bool ok = true;
		for (int k = 0; k < 2; k++) {
			ok &= Bind(k, g_default[k], picked >= 0 && k == 0 ? "picked" : "own maps", full) || k;
		}
		g_unpacking = false;
		if (picked >= 0 && (g_bind[0].package == picked || !ok)) { // a pick that fails to unpack doesn't hold match starts
			const std::string& hash = g_all[picked].hash;
			WriteWholeFile(Cfg().modCacheDir + L"maps\\last.txt", Bytes(hash.begin(), hash.end()));
			g_picked.compare_exchange_strong(picked, -1);
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
			Bind(k, i, "the host's", true);
			continue;
		}
		auto failed = g_failed.find(want);
		if (failed != g_failed.end() && GetTickCount() - failed->second < 30000) {
			// not holding match starts for a map we can't get, unless the slot has nothing at all (the game would
			// fail to open its files): then wait for the retry
			if (g_bind[k].package < 0) {
				g_unbound = true;
			}
			continue;
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

// A rebound slot's picture into the loaded one (it stays loaded once shown): its thumbnail, else its base map's.
// Outside g_mutex: the game's resource lock is taken in there.
void WritePictures()
{
	for (int k = 0; k < 2; k++) {
		if (!g_picDirty[k].exchange(false)) {
			continue;
		}
		std::wstring thumb;
		int base = -1;
		{
			std::lock_guard<std::recursive_mutex> lock(g_mutex);
			if (g_bind[k].package < 0) {
				continue;
			}
			thumb = g_bind[k].thumbnail;
			int b = g_all[g_bind[k].package].base;
			base = b <= 12 ? b : b == 15 ? 13 : 0;
		}
		Bytes pic;
		{
			std::lock_guard<std::mutex> lock(g_stockPicsMutex);
			if (g_stockPics[13].empty()) {
				XLS_LOG_INFO("mods: maps: picture of slot %d: the menu pictures aren't loaded yet.", kSlots[k]);
				continue; // they'll load the slot's own
			}
			if (thumb.empty() || !ReadWholeFile(thumb, &pic) || pic.size() != g_stockPics[13].size()) {
				pic = g_stockPics[base];
			}
		}
		MapPicsUpdate(k, pic);
	}
}

DWORD WINAPI SyncThread(LPVOID)
{
	for (;;) {
		Sleep(g_picked >= 0 ? 250 : 1000); // a pick waiting: soon after scrolling stops
		Reconcile();
		WritePictures();
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

// The transfer test (ModCache\maps\loopback.txt): our bound maps come again from a loopback
// "host" over the transfer protocol, and those downloaded copies play instead (packages
// "loopback-<hash>", unpacked from the downloaded zip). Match starts wait for it like for a host's.
const uint64_t kLoopbackHost = 0x01100001fffffff1ull;

DWORD WINAPI LoopbackThread(LPVOID)
{
	std::vector<std::pair<int, std::string>> wanted; // (slot index, hash)
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		for (int k = 0; k < 2; k++) {
			if (g_default[k] >= 0 && g_downloading.insert(g_all[g_default[k]].hash).second) {
				wanted.emplace_back(k, g_all[g_default[k]].hash);
			}
		}
	}
	for (const auto& w : wanted) {
		const std::string& hash = w.second;
		std::wstring path = Cfg().modCacheDir + L"maps\\loopback\\" + Utf8ToWide(hash.c_str()) + L".zip";
		MapPackage map;
		map.hash = hash;
		map.package = L"loopback-" + Utf8ToWide(hash.c_str());
		map.zip = path;
		map.remote = true;
		Zip zip;
		bool ok = xls::TransferFetch(kLoopbackHost, "map", hash, kMaxMapDownload, path);
		if (ok && HashFile(path) != hash) {
			XLS_LOG_WARN("mods: loopback test: map %s arrived different from what was sent.", hash.c_str());
			ok = false;
		}
		ok = ok && zip.Open(path) && ReadManifest(zip, map.package, &map);
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_downloading.erase(hash);
		if (!ok) {
			XLS_LOG_WARN("mods: loopback test: map %s failed; the original stays in slot %d.", hash.c_str(), kSlots[w.first]);
			continue;
		}
		g_all.push_back(map);
		g_default[w.first] = (int)g_all.size() - 1; // bound by the next Reconcile (outside matches)
		XLS_LOG_INFO("mods: loopback test: map \"%s\" (%s) came through intact; slot %d plays the downloaded copy.", map.name.c_str(), hash.c_str(), kSlots[w.first]);
	}
	return 0;
}

void Load()
{
	ConfigEnsureLoaded();
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		LoadMaps();
	}
	xls::TransferRegister("map", ServeMap);
	Bytes loopback;
	if (ReadWholeFile(Cfg().modCacheDir + L"maps\\loopback.txt", &loopback)) {
		// "rate <KB/s>" throttles it (a slow host)
		std::string text(loopback.begin(), loopback.end());
		size_t at = text.find("rate ");
		uint32_t rate = at == std::string::npos ? 0 : (uint32_t)atoi(text.c_str() + at + 5);
		xls::TransferAddLoopback(kLoopbackHost, rate * 1024);
		XLS_LOG_INFO("mods: loopback test on (maps\\loopback.txt): fetching our maps from ourselves.");
		HANDLE t = CreateThread(nullptr, 0, LoopbackThread, nullptr, 0, nullptr);
		if (t) {
			CloseHandle(t);
		}
	}
	xls::ContentAddPending([] {
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		// unbound: about to start one; picked: our pick isn't bound yet
		return (int)g_downloading.size() + (g_unbound && g_downloading.empty()) + (g_picked >= 0) + (int)g_unpacking;
	});
	xls::MemberDataListen("maps", [](uint64_t steamId, XUID, const std::string& value) {
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_memberMaps[steamId] = value;
	});
	xls::MemberDataListen("mapnames", [](uint64_t steamId, XUID, const std::string& value) {
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_memberNames[steamId] = value;
	});
	xls::NetSetQosExtra([] { return NamesValue(); });
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
		if (_stricmp(e.name.c_str(), "message\\msgPC\\MsgOption") == 0) {
			changed |= NetMenuPatchMessages(&e.data); // the Options Network entry's texts
			continue;
		}
		if (_stricmp(e.name.c_str(), "message\\msgPC\\MsgOnline") != 0) {
			continue;
		}
		for (const MapPackage& m : PackageMaps()) {
			changed |= MessageSet(&e.data, kMsgMapName + m.slot, m.name);
		}
	}
	return changed && WriteWholeFile(out, ArcWrite(entries));
}

// An archive's own list (rArchive 'ARCS': u16 8, u16 count, count x (name hash, class hash)): the name hash is the
// lowercased name's crc32 register (zlib's crc32 ^ ~0, all 32 bits, unlike ClassHash).
uint32_t NameHash(const std::string& name)
{
	uint32_t crc = 0xffffffff;
	for (char c : name) {
		crc ^= (uint8_t)tolower((unsigned char)c);
		for (int k = 0; k < 8; k++) {
			crc = crc & 1 ? (crc >> 1) ^ 0xedb88320 : crc >> 1;
		}
	}
	return crc;
}

// The mod slots' create-match pictures (the Flash's frames 15 / 16 show ID221_SC14_ID / SC15_ID, thumbflash.cpp):
// the bound map's thumbnail.rTexture, else its base map's stock picture, beside the stock ones (in archives that
// have them), and in the archive's list.
void AddMapPictures(std::vector<ArcEntry>* entries)
{
	const ArcEntry* stock[14] = {};
	std::string dir;
	for (const ArcEntry& e : *entries) {
		size_t at = e.name.find_last_of('\\');
		int n = -1;
		if (at != std::string::npos && sscanf(e.name.c_str() + at + 1, "ID221_SC%d_ID", &n) == 1 && n >= 0 && n < 14) {
			stock[n] = &e;
			dir = e.name.substr(0, at + 1);
		}
	}
	if (!stock[13]) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_stockPicsMutex);
		for (int n = 0; n < 14; n++) {
			if (stock[n] && g_stockPics[n].empty()) {
				g_stockPics[n] = stock[n]->data;
			}
		}
	}
	std::vector<MapPackage> maps = PackageMaps();
	std::vector<ArcEntry> add;
	for (int k = 0; k < 2; k++) {
		int slot = 13 + k;
		ArcEntry e = *stock[13];
		e.name = dir + (k ? "ID221_SC15_ID" : "ID221_SC14_ID");
		for (const MapPackage& m : maps) {
			if (m.slot != slot) {
				continue;
			}
			Bytes thumb;
			int base = m.base <= 12 ? m.base : m.base == 15 ? 13 : 0;   // the stock picture of frame base + 2
			if (!m.thumbnail.empty() && ReadWholeFile(m.thumbnail, &thumb) && thumb.size() == stock[13]->data.size()) {
				e.data = thumb;
			} else if (stock[base]) {
				e.data = stock[base]->data;
			}
		}
		add.push_back(e);
	}
	for (ArcEntry& e : *entries) {
		if (e.data.size() >= 8 && memcmp(e.data.data(), "ARCS", 4) == 0 && e.name.size() >= 5
				&& _stricmp(e.name.c_str() + e.name.size() - 5, "id221") == 0) {
			uint16_t count = (uint16_t)(e.data[6] | e.data[7] << 8);
			for (const ArcEntry& a : add) {
				uint32_t h[2] = {NameHash(a.name), a.classHash};
				const uint8_t* p = (const uint8_t*)h;
				e.data.insert(e.data.end(), p, p + 8);
				count++;
			}
			e.data[6] = (uint8_t)count;
			e.data[7] = (uint8_t)(count >> 8);
		}
	}
	entries->insert(entries->end(), add.begin(), add.end());
}

// The menus with our rows added to their Flash: create match (flash\playermatch\id221, rule rows),
// training (flash\training\id300, AI Enemy Battle rows) and Options (flash\option\id500_PC, the
// Network entry), in whichever archive carries them.
bool GenerateMenus(const std::wstring& source, const std::wstring& out)
{
	Bytes file;
	std::vector<ArcEntry> entries;
	if (!ReadWholeFile(source, &file) || !ArcRead(file, &entries)) {
		return false;
	}
	bool changed = false, pictures = false;
	for (ArcEntry& e : entries) {
		size_t at = e.name.find_last_of('\\');
		const char* name = e.name.c_str() + (at == std::string::npos ? 0 : at + 1);
		if (e.data.size() <= 8 || memcmp(e.data.data(), "GFX", 3) != 0) {
			continue;
		}
		if (_stricmp(name, "id221") == 0) {
			changed |= RulesPatchMenuFlash(&e.data);
			int r = ThumbsPatchMenuFlash(&e.data);
			if (r < 0) {
				XLS_LOG_WARN("mods: maps: the create-match Flash isn't laid out as expected, no mod map pictures.");
			}
			pictures |= r > 0;
			changed |= r > 0;
		} else if (_stricmp(name, "id300") == 0) {
			changed |= RulesPatchTrainingFlash(&e.data);
		} else if (_stricmp(name, "id200") == 0) {
			changed |= BotMatchPatchFlash(&e.data);
		} else if (_stricmp(name, "id500_PC") == 0) {
			changed |= NetMenuPatchFlash(&e.data);
		} else if (_stricmp(name, "id010_360") == 0) {
			changed |= TitlePatchFlash(&e.data);
		}
	}
	if (pictures) {
		AddMapPictures(&entries);
	}
	return changed && WriteWholeFile(out, ArcWrite(entries));
}


}

void PackagesLoad()
{
	std::call_once(g_once, Load);
}

bool PackagesSlotPhysics(int slot, MapPhysics* out)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	for (int k = 0; k < 2; k++) {
		if (kSlots[k] == slot && g_bind[k].package >= 0) {
			*out = g_all[g_bind[k].package].physics;
			return true;
		}
	}
	return false;
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

bool PackagesPick(int step, bool enter)
{
	PackagesLoad();
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::vector<int> own;
	int at = -1;
	for (size_t i = 0; i < g_all.size(); i++) {
		if (!g_all[i].remote) {
			at = (int)i == g_default[0] ? (int)own.size() : at;
			own.push_back((int)i);
		}
	}
	int next = enter ? (step > 0 ? 0 : (int)own.size() - 1) : at < 0 ? -1 : at + step;
	if (next < 0 || next >= (int)own.size()) {
		return false;
	}
	g_default[0] = own[next];
	g_pickedAt = GetTickCount();
	g_picked = own[next];
	return true;
}

std::string PackagesNameIn(const std::string& value, int slot)
{
	std::string key = FormatA("%d=", slot);
	for (size_t at = 0; at < value.size();) {
		size_t end = value.find('\n', at);
		std::string entry = value.substr(at, end == std::string::npos ? std::string::npos : end - at);
		if (entry.compare(0, key.size(), key) == 0) {
			return entry.substr(key.size());
		}
		at = end == std::string::npos ? value.size() : end + 1;
	}
	return std::string();
}

const char* PackagesSlotName(int slot)
{
	static char names[2][128];
	static char hostNames[2][128];
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	// in another player's lobby: the host's map, by the name it published (we may not have it yet)
	uint64_t host = g_lobbyHost;
	auto it = host ? g_memberNames.find(host) : g_memberNames.end();
	if (it != g_memberNames.end()) {
		std::string name = PackagesNameIn(it->second, slot);
		if (!name.empty()) {
			char* out = hostNames[slot == kSlots[0] ? 0 : 1];
			snprintf(out, 128, "%s", name.c_str());
			return out;
		}
	}
	for (int k = 0; k < 2; k++) {
		int i = k == 0 && g_picked >= 0 ? (int)g_picked : g_bind[k].package;
		if (kSlots[k] == slot && i >= 0) {
			snprintf(names[k], sizeof(names[k]), "%s", g_all[i].name.c_str());
			return names[k];
		}
	}
	return nullptr;
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
	for (int attempt = 0; attempt < 2; attempt++) {
		int light = -1;
		MapPackage map;
		{
			std::lock_guard<std::recursive_mutex> lock(g_mutex);
			for (int k = 0; k < 2; k++) {
				auto it = g_bind[k].files.find(rel);
				if (it == g_bind[k].files.end()) {
					continue;
				}
				if (!it->second.empty()) {
					*out = it->second;
					return true;
				}
				light = k;
				map = g_all[g_bind[k].package];
			}
		}
		if (light < 0 || attempt > 0) {
			return false;
		}
		// a light binding's file (a load outside our lobby: training, or before the sync thread got to it): unpacked now
		XLS_LOG_INFO("mods: map \"%s\": the game opens %ls; unpacking it now.", map.name.c_str(), rel.c_str());
		bool ok = Unpacked(map, kSlots[light]);
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		int i = g_bind[light].package;
		if (i < 0 || g_all[i].hash != map.hash) {
			continue; // rebound meanwhile: look again
		}
		if (!(ok && Bind(light, i, "the game loads it", true))) {
			XLS_LOG_WARN("mods: map \"%s\" cannot be unpacked; the base map's files are used.", map.name.c_str());
			g_bind[light].files.clear(); // not again for every file
			return false;
		}
	}
	return false;
}

// The Improved bot brains (bots.cpp BotsBuildBrains), from the player's own uHm6d.arc. An overlay that already has them wins.
bool GenerateBrains(const std::wstring& source, const std::wstring& out)
{
	Bytes file;
	std::vector<ArcEntry> entries;
	return ReadWholeFile(source, &file) && ArcRead(file, &entries) && BotsBuildBrains(&entries) && WriteWholeFile(out, ArcWrite(entries));
}

const std::wstring* PackagesGenerated(const std::wstring& rel, const std::wstring& source)
{
	PackagesLoad();
	// nativepc\arclp2xml\id_fcore_<lang>_pc.arc (names, the Network entry texts), the create-match Flash's archives (rule rows)
	static const std::wstring prefix = L"nativepc\\arclp2xml\\id_fcore_";
	// the menu Flashes (id221 create match, id300 training, id500_PC options) are also in the title archives, which load first
	static const std::wstring creates[] = {L"nativepc\\arcxmltmppc\\abattleplayercreate.arc", L"nativepc\\arcxmltmppc\\atrainingtop.arc",
	                                       L"nativepc\\arclp2xml\\id_title.arc", L"nativepc\\arclp2xml\\id_title_multi.arc",
	                                       L"nativepc\\arclp2xml\\id_title_pc.arc", L"nativepc\\arcxmltmppc\\aoption.arc"};
	bool names = rel.compare(0, prefix.size(), prefix) == 0 && rel.size() >= 7 && rel.compare(rel.size() - 7, 7, L"_pc.arc") == 0;
	bool brains = rel == L"nativepc\\arclp2xml\\autoarc\\uhm6d.arc"; // the bots' stock brain: ours go next to it
	if (!names && !brains && std::find(std::begin(creates), std::end(creates), rel) == std::end(creates)) {
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
	if (names ? GenerateNames(source, out) : brains ? GenerateBrains(source, out) : GenerateMenus(source, out)) {
		XLS_LOG_INFO("mods: maps: %s added to %ls.", names ? "names" : brains ? "bot brains" : "menu rows", rel.c_str());
		return &(g_generated[key] = out);
	}
	g_generated[key] = L"";
	return nullptr;
}

}
