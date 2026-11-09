#include "mods/lp2.h"
#include "mods/packages.h"
#include "mods/parts.h"

#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/net.h"
#include "core/utils.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

#include <mutex>
#include <set>
#include <string>
#include <vector>

// Extra online map slots for map packages (packages.cpp assigns the slots and serves the files):
// makes each slot selectable everywhere, gives it its base map's preload list (the vehicle, weapon
// and object archives the game loads before the area) and its own size class.
//
// The game identifies online maps by these numbers (map picker values 0-19, session data). 0-12
// and 15 are real maps and 16-19 are the Random <Small/Medium/Large/All> choices, so only 13 and
// 14 are free. Both are always set up (a player without map packages may join a lobby playing
// the host's, downloaded on the spot); packages.cpp binds packages to them at runtime and calls
// MapsRebind, which re-points the slot's preload list and size. A slot is selectable while bound.

namespace xls::mods {

namespace {

struct ModMap {
	int slot;
	int base;
	int size; // 0 small, 1 medium, 2 large; -1 = the base map's
	std::string name;
	std::vector<std::string> preload; // the bound package's extra archives
};

std::vector<ModMap> g_maps; // always slots 13 and 14
uint32_t* g_remapTable = nullptr;         // {map, size} x 16

// Online map availability (DX11 FUN_0080e470, thiscall on the game object, (u8 map, u8 flag)):
// 13 and 14 are hard-disabled, 4/8/11/15 depend on DLC flag bits at +0x50d20, 2 and 9 are off when
// flag is set. The original starts with
//   mov edx,ecx; movzx ecx,byte [esp+4]; lea eax,[ecx-2]; cmp eax,0xd; ja ...
// and ends with test [edx+0x50d20],eax, which pins the field offset.
const uint8_t kAvailBytes[] = {0x8b, 0xd1, 0x0f, 0xb6, 0x4c, 0x24, 0x04, 0x8d, 0x41, 0xfe, 0x83, 0xf8, 0x0d, 0x77};
const uint8_t kAvailField[] = {0x85, 0x82, 0x20, 0x0d, 0x05, 0x00};
const size_t kAvailFieldAt = 0x30;
const uint32_t kAvailRvas[kBuildCount] = {0x40e470, 0x3ff680, 0x40e670, 0x3ff720}; // LP2DX11.exe, LP2DX9.exe, 1.1 ones

// Per-map preload list {const char** arcs, u8 areaType, u8 id}, null terminated (DX11 0x1730fe0).
// Its two readers load the address as
//   mov ecx,LIST; cmp [LIST],eax      (lookup, DX11 FUN_007d12d0)
//   mov ebp,LIST; cmp [LIST],ebx      (group selector, DX11 FUN_007d1320)
struct DepEntry {
	const char** arcs;
	uint8_t areaType;
	uint8_t id;
	uint16_t pad;
};
// A map's arc list: {const char* name, u32 flags} pairs, null terminated, split into groups by "dummy"
// entries: the first group loads for every match on the map, then one group per mode and layout
// (DX11 FUN_007d1320 picks it). Map packages' extra archives (map.json "preload") go into the first.
struct ArcEntry {
	const char* name;
	uint32_t flags;
};
// Lists built for map packages: never freed (a list may still be read by a load in progress).
std::vector<std::vector<ArcEntry>*> g_builtLists;
std::vector<std::string*> g_builtNames;
DepEntry* g_preloadCopy = nullptr;        // the moved preload list (mod entries at the end)
std::vector<DepEntry> g_preloadOriginal;  // the game's entries
const uint32_t kDepRvas[kBuildCount][2] = {{0x3d12d2, 0x3d1329}, {0x3c2462, 0x3c24b9}, {0x3d13b2, 0x3d1409}, {0x3c2592, 0x3c25e9}}; // DX11, DX9, 1.1 ones
const uint8_t kAreaOnline = 7;

bool __attribute__((thiscall)) MapAvailable(uint8_t* game, uint32_t mapArg, uint32_t flagArg)
{
	uint32_t map = mapArg & 0xff;
	bool flag = (flagArg & 0xff) == 1;
	if ((map == 13 || map == 14) && PackagesSlotBound((int)map)) {
		return true;
	}
	switch (map) {
	case 2:
	case 9:
		return !flag;
	case 4:
		if (flag) {
			return false;
		}
		[[fallthrough]];
	case 8:
	case 11:
	case 15:
		return (*(uint32_t*)(game + 0x50d20) >> map) & 1;
	case 13:
	case 14:
		return false;
	default:
		return true;
	}
}

bool WriteCode(void* at, const void* bytes, size_t size)
{
	DWORD old = 0;
	if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	memcpy(at, bytes, size);
	VirtualProtect(at, size, old, &old);
	FlushInstructionCache(GetCurrentProcess(), at, size);
	return true;
}

bool InstallAvailability(uint8_t* base, int build)
{
	uint8_t* fn = base + kAvailRvas[build];
	if (memcmp(fn, kAvailBytes, sizeof(kAvailBytes)) != 0 || memcmp(fn + kAvailFieldAt, kAvailField, sizeof(kAvailField)) != 0) {
		return false;
	}
	uint8_t jmp[5] = {0xe9};
	int32_t rel = (int32_t)((uint8_t*)&MapAvailable - (fn + 5));
	memcpy(jmp + 1, &rel, 4);
	return WriteCode(fn, jmp, sizeof(jmp));
}

// base's arc list with extra archives added to its common group (those it doesn't already have).
const char** WithExtras(const char** arcs, const std::vector<std::string>& extra, int slot)
{
	if (!arcs || extra.empty()) {
		return arcs;
	}
	auto* src = (const ArcEntry*)arcs;
	auto* list = new std::vector<ArcEntry>();
	size_t n = 0;
	while (src[n].name) {
		n++;
	}
	size_t common = 1;
	while (common < n && _stricmp(src[common].name, src[0].name) != 0) {
		common++;
	}
	list->assign(src, src + common);
	int added = 0;
	for (const std::string& e : extra) {
		bool have = false;
		for (size_t i = 1; i < common && !have; i++) {
			have = _stricmp(src[i].name, e.c_str()) == 0;
		}
		if (!have) {
			auto* name = new std::string(e);
			g_builtNames.push_back(name);
			list->push_back({name->c_str(), 0});
			added++;
		}
	}
	list->insert(list->end(), src + common, src + n + 1); // the rest and the terminator
	g_builtLists.push_back(list);
	XLS_LOG_INFO("mods: maps: slot %d preloads %d extra archive(s) for its package.", slot, added);
	return (const char**)list->data();
}

bool InstallPreload(uint8_t* base, int build)
{
	uint8_t* sites[2] = {base + kDepRvas[build][0], base + kDepRvas[build][1]};
	const uint8_t movOp[2] = {0xb9, 0xbd};
	const uint8_t cmpOp[2] = {0x05, 0x1d};
	uint32_t list = 0;
	for (int i = 0; i < 2; i++) {
		uint8_t* s = sites[i];
		uint32_t a = *(uint32_t*)(s + 1);
		if (s[0] != movOp[i] || s[5] != 0x39 || s[6] != cmpOp[i] || *(uint32_t*)(s + 7) != a || (list && a != list)) {
			return false;
		}
		list = a;
	}
	std::vector<DepEntry> entries;
	for (auto* e = (const DepEntry*)(uintptr_t)list; e->arcs; e++) {
		entries.push_back(*e);
	}
	size_t original = entries.size();
	for (const ModMap& m : g_maps) {
		const char** arcs = nullptr;
		for (size_t i = 0; i < original; i++) {
			if (entries[i].areaType == kAreaOnline && entries[i].id == m.base) {
				arcs = entries[i].arcs;
			}
		}
		if (!arcs) {
			XLS_LOG_WARN("mods: map \"%s\": base map %d has no preload list, slot %d uses map 0's.", m.name.c_str(), m.base, m.slot);
			for (size_t i = 0; i < original && !arcs; i++) {
				arcs = entries[i].areaType == kAreaOnline && entries[i].id == 0 ? entries[i].arcs : nullptr;
			}
		}
		entries.push_back({WithExtras(arcs, m.preload, m.slot), kAreaOnline, (uint8_t)m.slot, 0});
	}
	entries.push_back({});
	// Never freed: the game reads the list for the rest of the session.
	auto* copy = (DepEntry*)VirtualAlloc(nullptr, entries.size() * sizeof(DepEntry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!copy) {
		return false;
	}
	memcpy(copy, entries.data(), entries.size() * sizeof(DepEntry));
	g_preloadCopy = copy;
	g_preloadOriginal.assign(entries.begin(), entries.begin() + original);
	uint32_t addr = (uint32_t)(uintptr_t)copy;
	for (uint8_t* s : sites) {
		WriteCode(s + 1, &addr, 4);
		WriteCode(s + 7, &addr, 4);
	}
	XLS_LOG_INFO("mods: maps: preload list moved to 0x%p (%u game entries + %u mod).", copy, (unsigned)original, (unsigned)(entries.size() - 1 - original));
	return true;
}

// Map availability is also inlined as switch (map - 2) with a 14-entry byte case table:
//   cmp reg,0xd; ja default; movzx reg,byte [reg+TABLE]; jmp [reg*4+JUMPS]
// Three table layouts send maps 13/14 to "skip" / "alert 175 (HARDUMMY)" / "DLC check": the map
// pickers (DX11 0xe8c9c0, 0xe9c2c8), lobby entry (0xe9a297) and others such as saved matches.
// Every referenced copy gets entries 11/12 (maps 13/14) set to the entry of map 3, which is always
// available. Other 14-case switches on the map id hold per-map properties and are left alone.
const uint8_t kCaseTables[][14] = {
	{0, 4, 1, 4, 4, 4, 2, 0, 4, 2, 4, 3, 3, 2},
	{0, 3, 1, 3, 3, 3, 1, 0, 3, 1, 3, 2, 2, 1},
	{0, 3, 3, 1, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2},
};

// When a match starts (DX11 FUN_00828f90) the chosen map goes through a {u32 map, u32 size} table
// for maps 0-15 in .data (DX11 0x1746038, DX9 0x1735fe8). 13 and 14 are placeholders that turn
// into 1 and 12, so a mod map in slot 13 used to load Turbulent Jungle. Each mod slot gets
// {slot, size}. Size 0/1/2 = small/medium/large, used by the lobby and the Random <size> choices.
bool PatchRemapTable(uint8_t* base)
{
	static const uint32_t kPrefix[] = {0, 1, 1, 0, 2, 0, 3, 1, 4, 0, 5, 2, 6, 1, 7, 2, 8, 1, 9, 0, 10, 0, 11, 1, 12, 0};
	auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
		// ".data" in the shipped exe (Steamless renames it ".dataa" in unpacked copies).
		if (memcmp(sec->Name, ".data", 5) != 0) {
			continue;
		}
		uint8_t* start = base + sec->VirtualAddress;
		uint8_t* end = start + sec->Misc.VirtualSize - 16 * 8;
		for (uint8_t* p = start; p < end; p += 4) {
			if (memcmp(p, kPrefix, sizeof(kPrefix)) != 0) {
				continue;
			}
			auto* table = (uint32_t*)p;
			g_remapTable = table;
			for (const ModMap& m : g_maps) {
				uint32_t size = m.size >= 0 ? (uint32_t)m.size : (m.base < 16 ? table[m.base * 2 + 1] : 1);
				uint32_t entry[2] = {(uint32_t)m.slot, size};
				DWORD old = 0;
				if (VirtualProtect(table + m.slot * 2, 8, PAGE_READWRITE, &old)) {
					memcpy(table + m.slot * 2, entry, 8);
					VirtualProtect(table + m.slot * 2, 8, old, &old);
				}
				XLS_LOG_INFO("mods: maps: slot %d resolves to itself, size %s.", m.slot, size == 0 ? "small" : size == 1 ? "medium" : "large");
			}
			return true;
		}
	}
	return false;
}

int PatchInlineTables(uint8_t* base)
{
	auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
	uint8_t* text = nullptr;
	size_t size = 0;
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
		if (memcmp(sec->Name, ".text", 6) == 0) {
			text = base + sec->VirtualAddress;
			size = sec->Misc.VirtualSize;
		}
	}
	int patched = 0;
	// cmp r32,0xd (83 f8+r 0d); ja rel8 (77 xx) or ja rel32 (0f 87 xx xx xx xx);
	// movzx r32,byte [r32+imm32] (0f b6 modrm imm32, mod=10)
	for (uint8_t* c = text; text && c + 17 <= text + size; c++) {
		if (c[0] != 0x83 || (c[1] & 0xf8) != 0xf8 || c[2] != 0x0d) {
			continue;
		}
		uint8_t* m = c[3] == 0x77 ? c + 5 : (c[3] == 0x0f && c[4] == 0x87 ? c + 9 : nullptr);
		if (!m || m[0] != 0x0f || m[1] != 0xb6 || (m[2] & 0xc0) != 0x80) {
			continue;
		}
		uint8_t* t = (uint8_t*)(uintptr_t)*(uint32_t*)(m + 3);
		if (t < text || t + 14 > text + size) {
			continue;
		}
		for (const uint8_t* layout : kCaseTables) {
			if (memcmp(t, layout, 11) != 0 || t[13] != layout[13] || (t[11] != layout[11] && t[11] != t[1])) {
				continue;
			}
			uint8_t entries[2] = {t[1], t[1]}; // both slots always (selectable only while bound: MapAvailable)
			if (WriteCode(t + 11, entries, 2)) {
				patched++;
			}
			break;
		}
	}
	return patched;
}

bool g_loaded = false;

}

ModMap* SlotEntry(int slot)
{
	for (ModMap& m : g_maps) {
		if (m.slot == slot) {
			return &m;
		}
	}
	g_maps.push_back({slot, 0, -1, std::string()});
	return &g_maps.back();
}

bool MapsWanted()
{
	if (!g_loaded) {
		g_loaded = true;
		SlotEntry(13);
		SlotEntry(14);
		for (const MapPackage& p : PackageMaps()) {
			ModMap* m = SlotEntry(p.slot);
			*m = {p.slot, p.base, p.size, p.name, p.preload};
		}
	}
	return true;
}

void MapsRebind(int slot, int base, int size, const std::vector<std::string>& preload)
{
	if (slot != 13 && slot != 14) {
		return;
	}
	ModMap* m = SlotEntry(slot);
	m->base = base;
	m->size = size;
	m->preload = preload;
	if (g_preloadCopy) {
		const char** arcs = nullptr;
		for (const DepEntry& e : g_preloadOriginal) {
			arcs = !arcs && e.areaType == kAreaOnline && e.id == base ? e.arcs : arcs;
		}
		arcs = WithExtras(arcs, preload, slot);
		for (DepEntry* e = g_preloadCopy + g_preloadOriginal.size(); arcs && e->arcs; e++) {
			if (e->id == slot) {
				e->arcs = arcs;
			}
		}
	}
	if (g_remapTable) {
		uint32_t entry[2] = {(uint32_t)slot, size >= 0 ? (uint32_t)size : (base < 13 || base == 15 ? g_remapTable[base * 2 + 1] : 1)};
		DWORD old = 0;
		if (VirtualProtect(g_remapTable + slot * 2, 8, PAGE_READWRITE, &old)) {
			memcpy(g_remapTable + slot * 2, entry, 8);
			VirtualProtect(g_remapTable + slot * 2, 8, old, &old);
		}
	}
}

// The create-match rule stepper (DX11 FUN_00e8c910, thiscall(screen, key, u8* value, max, min,
// rule, wrap) -> changed; key 10 right / 11 left, rule 0x1b = map): for the map we step ourselves in
// kMapOrder (the map packages after Frozen Wasteland, before the Random choices), with the
// stepper's own availability rule (MapAvailable; its "VS mode" flag is the current rule page's
// bytes +0x16 == 1 and +0x17 == 3, the page at screen+0x3d0 + [screen+0x418] * 4). On slot 13
// left / right step through the map packages first, so any number of them can be picked; slot 14
// only while bound (a host's map being downloaded). The map name (MsgOnline 126 + slot) comes from the message lookup (DX11 FUN_0077b280,
// thiscall(messages, file, id, lang) -> text), which answers with the bound package's name.
struct PickerVas {
	uint32_t step, message, sound, soundOwner, soundIds, game;
};
const PickerVas kPickerVas[kBuildCount] = {
	{0xe8c910, 0x77b280, 0x832f00, 0x185289c, 0x18e0860, 0x18520c0}, // DX11
	{0xe7c880, 0x76c0c0, 0x824110, 0x18427fc, 0x18ce774, 0x1842020}, // DX9
	{0xe8cdb0, 0x77b3f0, 0x833130, 0x185389c, 0x18e1860, 0x18530c0}, // DX11 1.1
	{0xe7cdf0, 0x76c130, 0x8241c0, 0x18427fc, 0x18ce774, 0x1842020}, // DX9 1.1
};
const uint8_t kMapOrder[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 13, 14, 16, 17, 18, 19};
const uint8_t kStepPrologue[] = {0x83, 0xec, 0x08, 0x8b, 0x44, 0x24, 0x10, 0x53}; // sub esp,8; mov eax,[esp+0x10]; push ebx
const uint8_t kMessagePrologue[] = {0x8b, 0x44, 0x24, 0x04, 0x56, 0x50, 0x8b, 0xf1}; // mov eax,[esp+4]; push esi; push eax; mov esi,ecx
const int kRuleMap = 0x1b;
const uint32_t kMsgMapName = 126;
int g_build = -1;

using StepFn = int(__attribute__((thiscall)) *)(void*, int, uint8_t*, int, int, int, char);
using MessageFn = const char*(__attribute__((thiscall)) *)(void*, const char*, uint32_t, uint32_t);
using SoundFn = void(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t, void*, uint32_t);
StepFn g_stepOriginal = nullptr;
MessageFn g_messageOriginal = nullptr;

void MenuSoundAt(void* screen, uint32_t id)
{
	const PickerVas& va = kPickerVas[g_build];
	auto sound = (SoundFn)(uintptr_t)va.sound;
	sound(*(void**)(uintptr_t)va.soundOwner, *(uint32_t*)(*(uint8_t**)(uintptr_t)va.soundIds + 0x78), id, screen, 0);
}

void StepSound(void* screen)
{
	MenuSoundAt(screen, 7);
}

void* g_onlineMessages = nullptr; // from the game's own MsgOnline lookups: for ours
uint32_t g_onlineLang = 0;

int __attribute__((thiscall)) RuleStep(void* screen, int key, uint8_t* value, int max, int min, int rule, char wrap)
{
	int ours = RulesStep(rule, key, value, [&] { return g_stepOriginal(screen, key, value, max, min, rule, wrap); });
	if (ours >= 0) {
		if (ours == 1) {
			StepSound(screen);
		}
		return ours ? 1 : 0;
	}
	if (rule != kRuleMap || (key != 10 && key != 11)) {
		return g_stepOriginal(screen, key, value, max, min, rule, wrap);
	}
	int step = key == 10 ? 1 : -1;
	uint8_t was = *value;
	{
		// which screens step the map through here, and with what range (the post-game lobby's Options: custom maps?)
		static std::mutex mutex;
		static std::set<uintptr_t> seen;
		std::lock_guard<std::mutex> lock(mutex);
		uintptr_t from = (uintptr_t)__builtin_return_address(0);
		if (seen.size() < 16 && seen.insert(from).second) {
			XLS_LOG_INFO("mods: maps: map row stepped from 0x%p (screen 0x%p, map %u, range %d-%d, wrap %d).", (void*)from, screen, was, min, max, wrap);
		}
	}
	*((uint8_t*)screen + 0x3ce) = 1; // as the stepper does first thing
	if (was == 13 && PackagesPick(step, false)) {
		StepSound(screen);
		return 1;
	}
	const PickerVas& va = kPickerVas[g_build];
	uint8_t* game = *(uint8_t**)(uintptr_t)va.game;
	uint8_t* page = *(uint8_t**)((uint8_t*)screen + 0x3d0 + *(int*)((uint8_t*)screen + 0x418) * 4);
	uint32_t flag = page && page[0x16] == 1 && page[0x17] == 3 ? 1 : 0;
	const int n = (int)sizeof(kMapOrder);
	int at = 0;
	while (at < n && kMapOrder[at] != was) {
		at++;
	}
	if (at == n) {
		return g_stepOriginal(screen, key, value, max, min, rule, wrap);
	}
	for (int tries = 1; tries < n; tries++) {
		at += step;
		if (at < 0 || at >= n) {
			if (!wrap) {
				return 0;
			}
			at = (at + n) % n;
		}
		uint8_t v = kMapOrder[at];
		if (v < min || v >= max) {
			continue;
		}
		bool ok = v == 13 ? PackagesPick(step, true) : v == 14 ? PackagesSlotBound(14) : game && MapAvailable(game, v, flag);
		if (ok) {
			*value = v;
			StepSound(screen);
			return 1;
		}
	}
	return 0;
}

// The lobby's Game Options (uIdBattlePlayerMatch, shown after a match; DX11 FUN_00e9c250) steps the map with its own
// thiscall(screen, key, value, max, min, check) -> the new value (keys 10 / 9 up, 11 / 8 down; with check, maps the
// player can't use are skipped, 13 / 14 among them before the case table patch). Like RuleStep: on slot 13 the steps go
// through our packages first, a step onto 13 starts at the first (or last) of them, and 13 / 14 with nothing to show
// are stepped over. The pick rebinds slot 13 and republishes the lobby's maps, so players without it download it and
// the match start waits for them (Reconcile, the content hold).
using MatchStepFn = uint32_t(__attribute__((thiscall)) *)(void*, int, uint32_t, int, uint32_t, char);
MatchStepFn g_matchStepOriginal = nullptr;
const uint32_t kMatchStep[kBuildCount] = {0xe9c250, 0xe8c180, 0xe9c500, 0xe8c660};
const uint8_t kMatchStepPrologue[] = {0x51, 0x53, 0x8b, 0x5c, 0x24, 0x10, 0x55}; // push ecx; push ebx; mov ebx,[esp+0x10]; push ebp

uint32_t __attribute__((thiscall)) MatchStep(void* screen, int key, uint32_t value, int max, uint32_t min, char check)
{
	int step = key == 10 || key == 9 ? 1 : key == 11 || key == 8 ? -1 : 0;
	if (step && (value & 0xff) == 13 && PackagesPick(step, false)) {
		StepSound(screen);
		return 13;
	}
	uint32_t at = value;
	for (int tries = 0; tries < 4; tries++) {
		at = g_matchStepOriginal(screen, key, at, max, min, check);
		uint8_t v = at & 0xff;
		bool usable = v == 13 ? PackagesPick(step, true) : v == 14 ? PackagesSlotBound(14) : true;
		if (usable || !step || v == (value & 0xff)) {
			return at;
		}
	}
	return at;
}

// The session browser's detail panel (DX9 FUN_00e82360, DX11 0xe92430; thiscall(screen, entry)):
// shows one search result's settings from the host's QoS data (screen+0x5c8 -> +0x288 + entry *
// 0x94; the panel reads its fields from +0x290). The map is MsgOnline 126 + map, so on 13 / 14 every
// session would show our own name for that slot (or the stock DUMMY): while the panel runs, the
// name comes from the text the host sent with that QoS data (its maps' names, core/net.cpp).
struct DetailVa {
	uint32_t fn;
	uint8_t prologue[9];   // sub esp,0x28; push esi; push <empty string>
};
const DetailVa kDetailVas[kBuildCount] = {
	{0xe92430, {0x83, 0xec, 0x28, 0x56, 0x68, 0x6c, 0x84, 0x6e, 0x01}}, // DX11
	{0xe82360, {0x83, 0xec, 0x28, 0x56, 0x68, 0xac, 0x7c, 0x6d, 0x01}}, // DX9
	{0xe92880, {0x83, 0xec, 0x28, 0x56, 0x68, 0x6c, 0x88, 0x6e, 0x01}}, // DX11 1.1
	{0xe82840, {0x83, 0xec, 0x28, 0x56, 0x68, 0x3c, 0x7f, 0x6d, 0x01}}, // DX9 1.1
};
using DetailFn = void(__attribute__((thiscall)) *)(void*, uint32_t);
DetailFn g_detailOriginal = nullptr;
const uint8_t* g_detailEntry = nullptr;   // game thread only

void __attribute__((thiscall)) Detail(void* screen, uint32_t entry)
{
	const uint8_t* list = *(const uint8_t**)((uint8_t*)screen + 0x5c8);
	g_detailEntry = list && entry < 32 ? list + 0x288 + entry * 0x94 : nullptr;
	g_detailOriginal(screen, entry);
	g_detailEntry = nullptr;
}

// The browsed session's own name for its map in slot (13 / 14), from its QoS answer.
const char* BrowsedName(uint32_t slot)
{
	static char name[128];
	std::string extra;
	// the game version (bytes 4..11 of the QoS data) is left out: compatible_versions hosts answer with their own
	// where the entry's copy of the QoS data starts varies (DX11 entries begin 4 bytes into it, at the version)
	bool found = false;
	if (g_detailEntry) {
		static const int kShifts[] = {0, -4, 8, 4, -8};
		static int lastShift = 99;
		for (int shift : kShifts) {
			// bytes before the entry aren't the QoS data's: compare from where the entry starts (and skip the version)
			size_t skipAt = shift < 0 ? 0 : 4, skipSize = shift < 0 ? (size_t)(-shift) + 8 : 8;
			if (xls::NetQosExtraFor(g_detailEntry + shift, (size_t)(0x94 - (shift > 0 ? shift : 0)), &extra, skipAt, skipSize)) {
				found = true;
				if (shift != lastShift) {
					lastShift = shift;
					XLS_LOG_INFO("mods: maps: browsed session's QoS data found at entry %+d.", shift);
				}
				break;
			}
		}
	}
	if (!found) {
		static bool logged = false;
		if (g_detailEntry && !logged) {
			logged = true;
			const uint8_t* e = g_detailEntry;
			XLS_LOG_INFO("mods: maps: no host map names for the browsed session (entry %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x; %s).",
				e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11], e[12], e[13], e[14], e[15],
				xls::NetQosExtrasDiff(e, 0x94).c_str());
		}
		return nullptr;
	}
	std::string n = PackagesNameIn(extra, (int)slot);
	if (n.empty()) {
		return nullptr;
	}
	snprintf(name, sizeof(name), "%s", n.c_str());
	return name;
}

const char* __attribute__((thiscall)) Message(void* messages, const char* file, uint32_t id, uint32_t lang)
{
	if (file && strcmp(file, "MsgOnline") == 0) {
		g_onlineMessages = messages;
		g_onlineLang = lang;
	}
	if (file && strcmp(file, "MsgBattle") == 0) {
		if (const char* text = RulesBattleMessage(id)) {
			return text;
		}
	}
	if (file && strcmp(file, "MsgOnline") == 0 && RulesMessageAlias(id) != id) {
		return g_messageOriginal(messages, file, RulesMessageAlias(id), lang);
	}
	if (const char* text = file && strcmp(file, "MsgOnline") == 0 ? RulesMessage(id) : nullptr) {
		return text;
	}
	if ((id == kMsgMapName + 13 || id == kMsgMapName + 14) && file && strcmp(file, "MsgOnline") == 0) {
		if (const char* name = BrowsedName(id - kMsgMapName)) {
			return name;
		}
		if (const char* name = PackagesSlotName((int)(id - kMsgMapName))) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				XLS_LOG_INFO("mods: maps: map name %u asked by 0x%p.", id, __builtin_return_address(0));
			}
			return name;
		}
	}
	return g_messageOriginal(messages, file, id, lang);
}

void InstallPicker(uint8_t* base)
{
	if (g_build < 0) {
		return;
	}
	const PickerVas& va = kPickerVas[g_build];
	uint8_t* exe = (uint8_t*)0x400000;
	auto at = [&](uint32_t v) { return base + (v - (uintptr_t)exe); };
	g_stepOriginal = (StepFn)Detour(at(va.step), kStepPrologue, sizeof(kStepPrologue), (void*)&RuleStep);
	g_messageOriginal = (MessageFn)Detour(at(va.message), kMessagePrologue, sizeof(kMessagePrologue), (void*)&Message);
	g_matchStepOriginal = (MatchStepFn)Detour(at(kMatchStep[g_build]), kMatchStepPrologue, sizeof(kMatchStepPrologue), (void*)&MatchStep);
	if (!g_matchStepOriginal) {
		XLS_LOG_WARN("mods: maps: the lobby's Game Options map row not hooked (only the create screen picks map packages).");
	}
	const DetailVa& dv = kDetailVas[g_build];
	g_detailOriginal = (DetailFn)Detour(at(dv.fn), dv.prologue, sizeof(dv.prologue), (void*)&Detail);
	XLS_LOG_INFO("mods: maps: session browser map names %s.", g_detailOriginal ? "hooked" : "NOT hooked (unknown code)");
	XLS_LOG_INFO("mods: maps: create-match map picker %s, map names %s.", g_stepOriginal ? "hooked" : "NOT hooked (unknown code)", g_messageOriginal ? "hooked" : "NOT hooked");
}

// Called once the whole of .text is decrypted (SteamStub decrypts it progressively, so the
// pickers can still be encrypted when MapsTryInstall succeeds).
void MapsPatchPickers(unsigned char* base)
{
	if (g_maps.empty()) {
		return;
	}
	InstallPicker(base);
	int tables = PatchInlineTables(base);
	XLS_LOG_INFO("mods: maps: %d map availability check(s) patched.", tables);
	if (!PatchRemapTable(base)) {
		XLS_LOG_WARN("mods: maps: match-start map table not found, mod maps will load another map.");
	}
}

// Called from the worker until it returns true (the code must be decrypted first).
bool MapsTryInstall(unsigned char* base)
{
	for (int build = 0; build < kBuildCount; build++) {
		if (GameBuild() >= 0 && build != GameBuild()) {
			continue;
		}
		if (memcmp(base + kAvailRvas[build], kAvailBytes, sizeof(kAvailBytes)) != 0) {
			continue;
		}
		if (!InstallPreload(base, build)) {
			XLS_LOG_WARN("mods: maps: preload list readers not recognised, mod maps disabled.");
			return true;
		}
		if (!InstallAvailability(base, build)) {
			XLS_LOG_WARN("mods: maps: availability check not recognised, mod map slots stay hidden.");
			return true;
		}
		g_build = build;
		XLS_LOG_INFO("mods: maps: %u mod map slot(s) enabled (%s).", (unsigned)g_maps.size(), BuildName(build));
		return true;
	}
	return false;
}

// The lobby's Match Settings: the maps the host can pick for the rules' mode, in the picker's order: the stock maps
// this player has, then our packages (as slot 13). Slot 14 isn't offered.
std::vector<LobbyMap> MapsLobbyChoices(const uint8_t* rules)
{
	std::vector<LobbyMap> out;
	uint8_t* game = g_build >= 0 ? *(uint8_t**)(uintptr_t)kPickerVas[g_build].game : nullptr;
	bool vs = rules[0x16] == 1 && rules[0x17] == 3;
	for (uint8_t v : kMapOrder) {
		if (v == 13) {
			for (const auto& own : PackagesOwn()) {
				out.push_back({13, own.first, own.second});
			}
			continue;
		}
		if (v == 14 || !game || !MapAvailable(game, v, vs ? 1 : 0)) {
			continue;
		}
		std::string name = MapsOnlineText(126 + v);
		out.push_back({v, -1, name.empty() ? "Map " + std::to_string(v) : name});
	}
	return out;
}

void MenuSound(void* screen, uint32_t id)
{
	MenuSoundAt(screen, id);
}

std::string MapsOnlineText(uint32_t id)
{
	if (!g_messageOriginal || !g_onlineMessages) {
		return "";
	}
	const char* text = g_messageOriginal(g_onlineMessages, "MsgOnline", id, g_onlineLang);
	return text ? text : "";
}

}
