#include "mods/lp2.h"
#include "mods/packages.h"

#include "core/log.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

// Map packages' water volumes (map.json "water_volumes"): water inside boxes, its surface at the box's top.
//
// The game asks one place for the water at a point: sGameManager (DX11 0x7e1790, DX9 0x7d28d0), thiscall(manager,
// pos, &height) -> bool: none unless the map's water exists (+0x261, the Efc scheduler's mWaterExist), then the
// area's water boundary (DX11 FUN_0086a0f0) or the scene box zone's water (FUN_0086df90, contents 0x40, height +0x44).
// Its sibling (DX11 0x7e1810, DX9 0x7d2950) also gives the water's current (vec4, speed). Underwater is
// pos.y < height (DX11 FUN_007fcde0, 290 callers: characters (lighter with the stage's 0x04000000 flag, gravity.cpp),
// weapons, effects, sounds). We redirect their 7 + 2 calls: on our maps a point inside a water volume (x / z within,
// y at or above its floor) has still water at the volume's top; elsewhere the map's own water (if any) as before.
// Still the game's: what reads the map's level directly (sGameManager +0x2ac: water fog, the camera's underwater look).

namespace xls::mods {

namespace {

struct Build {
	uint32_t at, flow;          // the two lookups
	uint32_t atCalls[7], flowCalls[2];
};
const Build kBuilds[kBuildCount] = {
	{0x3e1790, 0x3e1810, {0x3e18df, 0x3f1c5c, 0x3f1cd7, 0x3fcdeb, 0x3fce57, 0x3fffa3, 0x3fffce}, {0x4000b3, 0x4000e7}}, // LP2DX11.exe
	{0x3d28d0, 0x3d2950, {0x3d2a1f, 0x3e2d9c, 0x3e2e17, 0x3edf2b, 0x3edf97, 0x3f10e3, 0x3f110e}, {0x3f11f3, 0x3f1227}}, // LP2DX9.exe
	{0x3e18b0, 0x3e1930, {0x3e19ff, 0x3f1d7c, 0x3f1df7, 0x3fcf0b, 0x3fcf77, 0x4000c3, 0x4000ee}, {0x4001d3, 0x400207}}, // LP2DX11.exe 1.1
	{0x3d2a30, 0x3d2ab0, {0x3d2b7f, 0x3e2efc, 0x3e2f77, 0x3ee08b, 0x3ee0f7, 0x3f1243, 0x3f126e}, {0x3f1353, 0x3f1387}}, // LP2DX9.exe 1.1
};

typedef bool(__fastcall* AtFn)(uint8_t*, void*, const float*, float*);
typedef bool(__fastcall* FlowFn)(uint8_t*, void*, const float*, float*, float*, float*);
AtFn g_at = nullptr;
FlowFn g_flow = nullptr;

// The map's volumes, set at area load and read by every lookup (other threads too): a fixed table, the count
// published last.
const int kMax = 64;
WaterVolume g_water[kMax];
volatile LONG g_count = 0;
volatile LONG g_entered[kMax]; // logged once each

const WaterVolume* Find(const float* p)
{
	LONG n = g_count;
	for (LONG i = 0; i < n; i++) {
		const WaterVolume& v = g_water[i];
		if (ScriptVolumeOn(v.sid) && InBox(v.min, v.max, v.turn, p, false)) {   // above the top too: there it's above the water
			if (!g_entered[i] && InterlockedExchange(&g_entered[i], 1) == 0) {
				XLS_LOG_INFO("mods: water: something reached water volume %ld (surface %.0f) at %.0f %.0f %.0f.", i, v.max[1], p[0], p[1], p[2]);
			}
			return &v;
		}
	}
	return nullptr;
}

bool __fastcall WaterAt(uint8_t* self, void* edx, const float* pos, float* height)
{
	if (const WaterVolume* v = Find(pos)) {
		*height = v->max[1];
		return true;
	}
	return g_at(self, edx, pos, height);
}

bool __fastcall WaterFlow(uint8_t* self, void* edx, const float* pos, float* height, float* vec, float* speed)
{
	if (const WaterVolume* v = Find(pos)) {
		*height = v->max[1];
		vec[0] = vec[1] = vec[2] = vec[3] = 0.0f;
		*speed = 0.0f;
		return true;
	}
	return g_flow(self, edx, pos, height, vec, speed);
}

bool IsCall(uint8_t* base, uint32_t rva, uint32_t fn)
{
	uint8_t* at = base + rva;
	return at[0] == 0xe8 && at + 5 + *(int32_t*)(at + 1) == base + fn;
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

}

void WaterInstall(unsigned char* base)
{
	for (const Build& b : kBuilds) {
		if (GameBuild() >= 0 && &b - kBuilds != GameBuild()) {
			continue;
		}
		bool ok = true;
		for (uint32_t c : b.atCalls) {
			ok = ok && IsCall(base, c, b.at);
		}
		for (uint32_t c : b.flowCalls) {
			ok = ok && IsCall(base, c, b.flow);
		}
		if (!ok) {
			continue;
		}
		g_at = (AtFn)(base + b.at);
		g_flow = (FlowFn)(base + b.flow);
		for (uint32_t c : b.atCalls) {
			WriteCall(base + c, (void*)&WaterAt);
		}
		for (uint32_t c : b.flowCalls) {
			WriteCall(base + c, (void*)&WaterFlow);
		}
		XLS_LOG_INFO("mods: water: installed (%s).", BuildName((int)(&b - kBuilds)));
		return;
	}
	XLS_LOG_WARN("mods: water: calls not recognised, water volumes ignored.");
}

void WaterSetMap(const MapPhysics* physics)
{
	g_count = 0;
	MemoryBarrier();
	LONG n = 0;
	if (physics) {
		for (const WaterVolume& v : physics->water) {
			if (n == kMax) {
				XLS_LOG_WARN("mods: water: more than %d water volumes, the rest ignored.", kMax);
				break;
			}
			g_entered[n] = 0;
			g_water[n++] = v;
		}
	}
	MemoryBarrier();
	g_count = n;
	if (n) {
		XLS_LOG_INFO("mods: water: %ld water volume(s).", n);
	}
}

}
