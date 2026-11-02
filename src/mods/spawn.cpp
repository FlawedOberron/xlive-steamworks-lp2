#include "mods/lp2.h"

#include "core/log.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

// Scripts' Spawn node (scripts.cpp): a weapon pickup (grenades and VS weapons are pickups too) or an empty VS at a
// point, made the way the akrid generator uAk31 makes its units (DX9 FUN_00a76540, DX11 0xa85d80): a copy of the
// exe's own template object-set record (rObjSet's format, lp2-tools objset.py: +0x04 tier, +0x18 u8 1, u8 kind,
// u16 item / VS type at +0x1a, +0x40 pos cm, +0x50 rot degrees, +0x74 user data: 0 = not buried) handed to the set's
// spawner for its group: weapons DX9 FUN_00f72d40 cdecl(record, &unit, 0), VS FUN_00f73f20 cdecl(record, pilot
// record (null: empty), &unit, 0). The spawners keep nothing of the record (the generator's is on its stack too).
// The VS one honours the lobby's VS setting through the record's tier (+0x04: 0 = Few, Many and All).
// Called on the game thread (a character's update), on every machine: the host sends what to spawn.

namespace xls::mods {

namespace {

struct Build {
	uint32_t weaponCall, weapon, vsCall, vs; // the generator's calls (checked), the spawners (RVAs)
	uint32_t weaponTemplate, vsTemplate;     // the template records (RVAs: 0x120 / 0x140 bytes)
};
const Build kBuilds[kBuildCount] = {
	{0x68607c, 0xb82bd0, 0x685fd1, 0xb83db0, 0x13c1c10, 0x13c19b0}, // LP2DX11.exe
	{0x67683c, 0xb72d40, 0x676791, 0xb73f20, 0x13b1bb0, 0x13b1950}, // LP2DX9.exe
	{0x6862bc, 0xb83060, 0x686211, 0xb84240, 0x13c2c10, 0x13c29b0}, // LP2DX11.exe 1.1
	{0x67646c, 0xb73080, 0x6763c1, 0xb74260, 0x13b1bb0, 0x13b1950}, // LP2DX9.exe 1.1
};
const int kWeaponSize = 0x120, kVsSize = 0x140;

typedef int(__cdecl* WeaponFn)(uint8_t*, uint8_t**, int);
typedef int(__cdecl* VsFn)(uint8_t*, uint8_t*, uint8_t**, int);
uint8_t* g_base = nullptr;
const Build* g_build = nullptr;

bool IsCall(uint8_t* base, uint32_t rva, uint32_t fn)
{
	uint8_t* at = base + rva;
	return at[0] == 0xe8 && at + 5 + *(int32_t*)(at + 1) == base + fn;
}

void Place(uint8_t* rec, int item, const float pos[3], float yaw)
{
	*(uint16_t*)(rec + 0x1a) = (uint16_t)item;
	*(uint32_t*)(rec + 0x04) = 0;
	memcpy(rec + 0x40, pos, 12);
	*(float*)(rec + 0x4c) = 0.0f;
	float rot[4] = {0.0f, yaw, 0.0f, 0.0f};
	memcpy(rec + 0x50, rot, 16);
	memset(rec + 0x74, 0, 16 * 4);
}

}

void SpawnInstall(unsigned char* base)
{
	for (const Build& b : kBuilds) {
		if (GameBuild() >= 0 && &b - kBuilds != GameBuild()) {
			continue;
		}
		if (!IsCall(base, b.weaponCall, b.weapon) || !IsCall(base, b.vsCall, b.vs)) {
			continue;
		}
		const uint8_t* w = base + b.weaponTemplate;
		const uint8_t* v = base + b.vsTemplate;
		if (w[0x18] != 1 || w[0x19] != 4 || v[0x18] != 1 || v[0x19] != 2) {
			XLS_LOG_WARN("mods: spawn: template records not recognised, scripts cannot spawn.");
			return;
		}
		g_base = base;
		g_build = &b;
		XLS_LOG_INFO("mods: spawn: ready (%s).", BuildName((int)(&b - kBuilds)));
		return;
	}
	XLS_LOG_WARN("mods: spawn: calls not recognised, scripts cannot spawn.");
}

bool SpawnObject(int kind, int item, const float pos[3], float yaw)
{
	if (!g_build) {
		return false;
	}
	uint8_t* unit = nullptr;
	int ok = 0;
	if (kind == kSpawnVs) {
		alignas(16) uint8_t rec[kVsSize];
		memcpy(rec, g_base + g_build->vsTemplate, kVsSize);
		Place(rec, item, pos, yaw);
		ok = ((VsFn)(g_base + g_build->vs))(rec, nullptr, &unit, 0);
	}
	else {
		alignas(16) uint8_t rec[kWeaponSize];
		memcpy(rec, g_base + g_build->weaponTemplate, kWeaponSize);
		Place(rec, item, pos, yaw);
		ok = ((WeaponFn)(g_base + g_build->weapon))(rec, &unit, 0);
	}
	XLS_LOG_INFO("mods: spawn: %s %02x at %.0f %.0f %.0f: %s.", kind == kSpawnVs ? "VS" : "weapon", item, pos[0], pos[1], pos[2],
		ok ? "done" : "refused (the lobby's VS setting, or not loaded?)");
	return ok != 0;
}

}
