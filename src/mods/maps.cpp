#include "mods/lp2.h"

#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

// Extra online map slots. A mod folder may hold a mod.json such as
//   {"maps": [{"slot": 13, "base": 0, "name": "Test Arena"}]}
// which makes online map id 13 selectable and gives it map 0's preload list (the vehicle, weapon
// and object archives the game loads before the area). The map's own files come through the file
// overlay as aArea_ONLINE_13*.arc and friends; its name (MsgOnline 126+slot) and picker thumbnail
// (id221 mc_monitor frame slot+2) come from overlay files too.
//
// The game identifies online maps by these numbers (map picker values 0-19, session data). 0-12
// and 15 are real maps and 16-19 are the Random <Small/Medium/Large/All> choices, so only 13 and
// 14 are free. Mapping more mods than slots onto them per session is the next step.

namespace xls::mods {

namespace {

struct ModMap {
	int slot;
	int base;
	std::string name;
};

std::vector<ModMap> g_maps;
bool g_slotUsed[32] = {};

// Online map availability (DX11 FUN_0080e470, thiscall on the game object, (u8 map, u8 flag)):
// 13 and 14 are hard-disabled, 4/8/11/15 depend on DLC flag bits at +0x50d20, 2 and 9 are off when
// flag is set. The original starts with
//   mov edx,ecx; movzx ecx,byte [esp+4]; lea eax,[ecx-2]; cmp eax,0xd; ja ...
// and ends with test [edx+0x50d20],eax, which pins the field offset.
const uint8_t kAvailBytes[] = {0x8b, 0xd1, 0x0f, 0xb6, 0x4c, 0x24, 0x04, 0x8d, 0x41, 0xfe, 0x83, 0xf8, 0x0d, 0x77};
const uint8_t kAvailField[] = {0x85, 0x82, 0x20, 0x0d, 0x05, 0x00};
const size_t kAvailFieldAt = 0x30;
const uint32_t kAvailRvas[] = {0x40e470, 0x3ff680}; // LP2DX11.exe, LP2DX9.exe

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
const uint32_t kDepRvas[2][2] = {{0x3d12d2, 0x3d1329}, {0x3c2462, 0x3c24b9}}; // DX11, DX9
const uint8_t kAreaOnline = 7;

bool __attribute__((thiscall)) MapAvailable(uint8_t* game, uint32_t mapArg, uint32_t flagArg)
{
	uint32_t map = mapArg & 0xff;
	bool flag = (flagArg & 0xff) == 1;
	if (map < 32 && g_slotUsed[map]) {
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

void LoadModJson(const std::wstring& mod)
{
	std::wstring path = Cfg().modDir + mod + L"\\mod.json";
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	std::string text;
	char buf[4096];
	DWORD got = 0;
	while (ReadFile(file, buf, sizeof(buf), &got, nullptr) && got) {
		text.append(buf, got);
	}
	CloseHandle(file);
	Json root;
	std::string error;
	if (!Json::Parse(text, root, &error)) {
		XLS_LOG_WARN("mods: %ls: %s", path.c_str(), error.c_str());
		return;
	}
	for (const Json& m : root["maps"].Elements()) {
		ModMap map{(int)m["slot"].GetInt(-1), (int)m["base"].GetInt(0), m["name"].GetString(std::string())};
		bool freeSlot = map.slot == 13 || map.slot == 14;
		if (!freeSlot || g_slotUsed[map.slot]) {
			XLS_LOG_WARN("mods: %ls: map \"%s\" wants slot %d, which is not a free slot (13 or 14) or is taken.", mod.c_str(), map.name.c_str(), map.slot);
			continue;
		}
		g_slotUsed[map.slot] = true;
		g_maps.push_back(map);
		XLS_LOG_INFO("mods: %ls: map \"%s\" in online slot %d (preloads like map %d).", mod.c_str(), map.name.c_str(), map.slot, map.base);
	}
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
			XLS_LOG_WARN("mods: map \"%s\": base map %d has no preload list, slot %d left disabled.", m.name.c_str(), m.base, m.slot);
			g_slotUsed[m.slot] = false;
			continue;
		}
		entries.push_back({arcs, kAreaOnline, (uint8_t)m.slot, 0});
	}
	entries.push_back({});
	// Never freed: the game reads the list for the rest of the session.
	auto* copy = (DepEntry*)VirtualAlloc(nullptr, entries.size() * sizeof(DepEntry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!copy) {
		return false;
	}
	memcpy(copy, entries.data(), entries.size() * sizeof(DepEntry));
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
			uint8_t entries[2] = {g_slotUsed[13] ? t[1] : layout[11], g_slotUsed[14] ? t[1] : layout[12]};
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

bool MapsWanted()
{
	if (!g_loaded) {
		g_loaded = true;
		for (const std::wstring& mod : Cfg().modsEnabled) {
			LoadModJson(mod);
		}
	}
	return !g_maps.empty();
}

// Called once the whole of .text is decrypted (SteamStub decrypts it progressively, so the
// pickers can still be encrypted when MapsTryInstall succeeds).
void MapsPatchPickers(unsigned char* base)
{
	if (g_maps.empty()) {
		return;
	}
	int tables = PatchInlineTables(base);
	XLS_LOG_INFO("mods: maps: %d map picker check(s) patched.", tables);
}

// Called from the worker until it returns true (the code must be decrypted first).
bool MapsTryInstall(unsigned char* base)
{
	for (int build = 0; build < 2; build++) {
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
		XLS_LOG_INFO("mods: maps: %u mod map slot(s) enabled (%s).", (unsigned)g_maps.size(), build == 0 ? "DX11" : "DX9");
		return true;
	}
	return false;
}

}
