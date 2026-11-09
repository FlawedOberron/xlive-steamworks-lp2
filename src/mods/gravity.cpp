#include "mods/lp2.h"
#include "mods/packages.h"

#include "core/log.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <vector>

// Map packages' gravity (map.json "low_gravity", "underwater", "gravity", "gravity_volumes").
//
// The game decides a map's gravity in a hard-coded per-area table (DX9 FUN_00869940, cdecl(area
// name), called once per area load): it clears stage flags [stage]+0x14598 0x02000000 / 0x04000000
// and sets them by episode ([main]+0x257) and area ([main]+0x25a): 0x02000000 = low gravity (NEOS,
// online 7: characters x0.3, the air boost and the AI's low-gravity rules), 0x04000000 = lighter
// characters below the water level (online 2, 3, 5, 6, 8, 11). We wrap its one call: after it, on
// a mod slot (online 13 / 14), the bound package's flags go in ("jump_packs" sets NEOS's 0x02000000), and the
// physics world's gravity ([main]+0x511d0, read when the Havok world is built: thrown grenades, debris,
// ragdolls) is scaled by "gravity".
//
// Characters: their gravity (cJump mGravity) is set every frame from mGravityOrg and those flags
// (DX9 FUN_009acef0, fastcall(character), one call). We wrap that call too: with jump packs the game's x0.3 is
// undone (the map's "gravity" decides), then the result is scaled by "gravity" and by the gravity volume the
// character stands in (its position, +0x30, in a box).
//
// Distance fog (not gravity, but the same per-frame hook): the scene box's fog managers (sSceneBox2 + 0x990 and + 0x1030,
// flags + 0x694, DX9 FUN_00f58680) draw the distance fog with a uColorFog, from the zone's colour, strength, start / end
// and curves, as in the campaign. On online maps the map's scheduler turns on their atmosphere mode (0x200, from scene
// box flag + 0x16d0 0x2000, DX11 FUN_0086a540): the haze then comes from the atmosphere (mpAreaLScattering) and the
// zone's fog reaches the uColorFog only in some places (0x40), so none with our clear sky. On our maps it's kept off,
// every frame: the fog the editor shows.

namespace xls::mods {

namespace {

struct Build {
	uint32_t areaCall, areaFn;     // the area flag table's call and function
	uint32_t charCall, charFn;     // the character gravity update's call and function
	uint32_t main, stage;          // game globals (RVAs of the pointers)
	uint32_t gravityField;         // cJump mGravity in the character (mGravityOrg follows it)
	uint32_t sceneBox;             // sSceneBox2 pointer
};
const Build kBuilds[kBuildCount] = {
	{0x47b7be, 0x478ff0, 0x5bde2d, 0x5bc860, 0x14520c0, 0x14d08ac, 0x17cc, 0x1452d38}, // LP2DX11.exe
	{0x46c10e, 0x469940, 0x5ae4bd, 0x5acef0, 0x1442020, 0x14c080c, 0x17bc, 0x1442c98}, // LP2DX9.exe
	{0x47bb4e, 0x479380, 0x5bdd8d, 0x5bc7b0, 0x14530c0, 0x14d18ac, 0x17cc, 0x1453d38}, // LP2DX11.exe 1.1
	{0x46c37e, 0x469bb0, 0x5ae23d, 0x5acc70, 0x1442020, 0x14c080c, 0x17bc, 0x1442c98}, // LP2DX9.exe 1.1
};
const uint32_t kLowGravity = 0x02000000;
const uint32_t kUnderwater = 0x04000000;
const float kEarth = -9.8f;
const float kLowFactor = 0.3f; // the game's low-gravity factor for characters (DX9 _DAT_0135b19c)

uint8_t* g_base = nullptr;
const Build* g_build = nullptr;
void(__cdecl* g_areaFn)(const char*) = nullptr;
void(__fastcall* g_charFn)(uint8_t*) = nullptr;

// The map being played (set on the game thread at area load, read there every frame).
bool g_active = false;
MapPhysics g_physics;
std::vector<bool> g_entered; // gravity volumes someone has been in (logged once each)

uint8_t* Global(uint32_t rva)
{
	return *(uint8_t**)(g_base + rva);
}

void __cdecl AreaFlags(const char* area)
{
	g_areaFn(area);
	uint8_t* main = Global(g_build->main);
	uint8_t* stage = Global(g_build->stage);
	if (!main || !stage) {
		return;
	}
	int episode = main[0x257];
	int map = main[0x25a];
	g_active = episode == 7 && (map == 13 || map == 14) && PackagesSlotPhysics(map, &g_physics);
	float* gravity = (float*)(main + 0x511d0);
	gravity[0] = 0.0f;
	// thrown grenades, debris and ragdolls are Havok bodies: on our maps low gravity reaches them too (the game's
	// own low gravity, NEOS, is characters only)
	gravity[1] = kEarth * (g_active ? g_physics.gravity : 1.0f);
	gravity[2] = 0.0f;
	g_entered.assign(g_active ? g_physics.volumes.size() : 0, false);
	PostSetMap(g_active ? &g_physics : nullptr);
	WaterSetMap(g_active ? &g_physics : nullptr);
	DamageSetMap(g_active ? &g_physics : nullptr);
	OnewaySetMap(g_active ? &g_physics : nullptr);
	if (!g_active && episode == 7) {
		// a stock online map (System Link or online): its embedded bot zones, as a package's
		MapPhysics stock;
		if (StockNavZones(map, &stock.nav)) {
			BotsSetMap(&stock);
			ZonesSetMap(&stock);
			XLS_LOG_INFO("mods: stock nav: map %d has %u bot zones, %u links.", map, (unsigned)stock.nav.zones.size(),
				(unsigned)stock.nav.links.size());
		}
	}
	MemoryLogNow("at area load");
	ScriptSetMap(g_active ? &g_physics : nullptr);
	JeepSetMap(episode == 7 ? map : -1);
	if (!g_active) {
		return;
	}
	uint32_t* flags = (uint32_t*)(stage + 0x14598);
	// water volumes: characters in them are lighter, as below a stock map's water level
	bool underwater = g_physics.underwater || !g_physics.water.empty();
	*flags = (*flags & ~(kLowGravity | kUnderwater)) | (g_physics.jumpPacks ? kLowGravity : 0) | (underwater ? kUnderwater : 0);
	XLS_LOG_INFO("mods: gravity: map %d: jump packs %d, underwater %d, gravity x%.2f, %u gravity / %u post volume(s).", map,
		(int)g_physics.jumpPacks, (int)underwater, g_physics.gravity, (unsigned)g_physics.volumes.size(),
		(unsigned)g_physics.post.size());
}

void ColourFog()
{
	if (uint8_t* box = Global(g_build->sceneBox)) {
		*(uint32_t*)(box + 0x1024) &= ~0x200u;   // the fog managers (+0x990 / +0x1030, flags +0x694): atmosphere mode off
		*(uint32_t*)(box + 0x16c4) &= ~0x200u;
	}
}

void __fastcall CharGravity(uint8_t* character)
{
	g_charFn(character);
	if (!g_active) {
		return;
	}
	ColourFog();
	ScriptCharacter(character);
	float* gravity = (float*)(character + g_build->gravityField);
	float org = gravity[1];
	if (g_physics.jumpPacks && fabsf(*gravity - org * kLowFactor) <= fabsf(org) * 1e-4f) {
		*gravity = org;   // the game's low gravity off, its jump packs kept
	}
	float scale = g_physics.gravity;
	const float* pos = (const float*)(character + 0x30);
	for (size_t i = 0; i < g_physics.volumes.size(); i++) {
		const GravityVolume& v = g_physics.volumes[i];
		if (ScriptVolumeOn(v.sid) && InBox(v.min, v.max, v.turn, pos)) {
			scale *= ScriptValue(v.sid, kSetGravity, v.gravity);
			if (!g_entered[i]) {
				g_entered[i] = true;
				XLS_LOG_INFO("mods: gravity: a character entered gravity volume %u (x%.2f) at %.0f %.0f %.0f.", (unsigned)i, v.gravity, pos[0], pos[1], pos[2]);
			}
			break;
		}
	}
	if (scale != 1.0f) {
		*gravity *= scale;
	}
	DamageCharacter(character);
}

bool WriteCall(uint8_t* at, void* to)
{
	int32_t rel = (int32_t)((uint8_t*)to - (at + 5));
	DWORD old = 0;
	if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	memcpy(at + 1, &rel, 4);
	VirtualProtect(at + 1, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), at, 5);
	return true;
}

// A call instruction at rva that calls fn.
bool IsCall(uint8_t* base, uint32_t rva, uint32_t fn)
{
	uint8_t* at = base + rva;
	return at[0] == 0xe8 && at + 5 + *(int32_t*)(at + 1) == base + fn;
}

}

void GravityInstall(unsigned char* base)
{
	for (const Build& b : kBuilds) {
		if (GameBuild() >= 0 && &b - kBuilds != GameBuild()) {
			continue;
		}
		if (!IsCall(base, b.areaCall, b.areaFn) || !IsCall(base, b.charCall, b.charFn)) {
			continue;
		}
		g_base = base;
		g_build = &b;
		g_areaFn = (void(__cdecl*)(const char*))(base + b.areaFn);
		g_charFn = (void(__fastcall*)(uint8_t*))(base + b.charFn);
		if (WriteCall(base + b.areaCall, (void*)&AreaFlags) && WriteCall(base + b.charCall, (void*)&CharGravity)) {
			XLS_LOG_INFO("mods: gravity: installed (%s).", BuildName((int)(&b - kBuilds)));
		}
		return;
	}
	XLS_LOG_WARN("mods: gravity: calls not recognised, map gravity settings ignored.");
}

}
