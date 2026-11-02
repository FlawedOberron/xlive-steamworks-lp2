#include "mods/lp2.h"
#include "mods/packages.h"

#include "core/log.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <unordered_map>
#include <vector>

// Map packages' one-way volumes (map.json "oneway_volumes"): characters and shells may cross the box along its arrow
// only. Every character's and shell's update starts with uCharacter::vf55 (DX9 0x998d50, thiscall, no arguments: it
// saves +0x30 / +0xd0 into mOldPos / mOldWorldPos), called by uCharacterBase::vf55 (players, VS, akrid) and
// uShellBase::vf55 (bullets, rockets, grenades: uShell01 and its subclasses); nothing else calls it. We wrap both
// calls: after it, +0x30 is where last frame's move left the unit, and we remember where it was the frame before.
// A move through a box against its arrow: characters lose the move's part along the arrow (they can still slide
// sideways), shells are removed (unit state 3: sUnit deletes them, DX9 0x4a0360).
// Runs on every machine: each moves its own copies (remote players are put back by their own machine anyway).

namespace xls::mods {

namespace {

struct Build {
	uint32_t charCall, shellCall, update; // the two calls and uCharacter::vf55
	uint32_t grenade;                    // uShell01's DTI
	uint32_t oldPos, oldWorldPos;        // uCharacter mOldPos / mOldWorldPos
};
const Build kBuilds[kBuildCount] = {
	{0x5bdda4, 0xa4a6e3, 0x5a8530, 0x14a4920, 0xc20, 0xc70}, // LP2DX11.exe
	{0x5ae434, 0xa3a903, 0x598d50, 0x1494880, 0xc10, 0xc60}, // LP2DX9.exe
	{0x5bdd04, 0xa4a7b3, 0x5a8830, 0x14a5920, 0xc20, 0xc70}, // LP2DX11.exe 1.1
	{0x5ae1b4, 0xa3a6d3, 0x598f10, 0x1494880, 0xc10, 0xc60}, // LP2DX9.exe 1.1
};
const DWORD kStale = 250; // ms without an update: a new unit (or a deleted one's address reused)

struct Last {
	float pos[3];
	DWORD at;
};

uint8_t* g_base = nullptr;
const Build* g_build = nullptr;
void(__fastcall* g_update)(uint8_t*) = nullptr;
std::vector<OnewayVolume> g_volumes; // the map's (game thread)
std::unordered_map<uint8_t*, Last> g_last;
std::unordered_map<void*, bool> g_isGrenade; // by vtable
std::vector<bool> g_logged;

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

// A shell's class derives from uShell01 (MT DTI: getDTI is vtable slot 4, +0x10 the parent).
bool IsGrenade(uint8_t* shell)
{
	void** vtable = *(void***)shell;
	auto it = g_isGrenade.find(vtable);
	if (it != g_isGrenade.end()) {
		return it->second;
	}
	const uint8_t* dti = ((const uint8_t*(__fastcall*)(uint8_t*))vtable[4])(shell);
	bool yes = false;
	for (int i = 0; dti && i < 16 && !yes; i++, dti = *(const uint8_t**)(dti + 0x10)) {
		yes = dti == g_base + g_build->grenade;
	}
	g_isGrenade[vtable] = yes;
	return yes;
}

// The move a -> b in the volume's frame (centre, z along its arrow's back): whether it goes through the box while
// going against the arrow; dz: how far against it.
bool Against(const OnewayVolume& v, const float* a, const float* b, float* dz)
{
	float cx = (v.min[0] + v.max[0]) * 0.5f, cz = (v.min[2] + v.max[2]) * 0.5f;
	float c = v.turn[0], s = v.turn[1];
	float p0[3] = {(a[0] - cx) * c - (a[2] - cz) * s, a[1], (a[0] - cx) * s + (a[2] - cz) * c};
	float p1[3] = {(b[0] - cx) * c - (b[2] - cz) * s, b[1], (b[0] - cx) * s + (b[2] - cz) * c};
	*dz = p1[2] - p0[2];
	if (*dz <= 0.01f) {
		return false;
	}
	float lo[3] = {v.min[0] - cx, v.min[1], v.min[2] - cz};
	float hi[3] = {v.max[0] - cx, v.max[1], v.max[2] - cz};
	float t0 = 0.0f, t1 = 1.0f;
	for (int i = 0; i < 3; i++) {
		float d = p1[i] - p0[i];
		if (fabsf(d) < 1e-6f) {
			if (p0[i] < lo[i] || p0[i] > hi[i]) {
				return false;
			}
			continue;
		}
		float ta = (lo[i] - p0[i]) / d, tb = (hi[i] - p0[i]) / d;
		if (ta > tb) {
			float t = ta;
			ta = tb;
			tb = t;
		}
		t0 = ta > t0 ? ta : t0;
		t1 = tb < t1 ? tb : t1;
		if (t0 > t1) {
			return false;
		}
	}
	return true;
}

void Check(uint8_t* unit, bool shell)
{
	float* pos = (float*)(unit + 0x30);
	DWORD now = GetTickCount();
	if (g_last.size() > 4096) {
		for (auto it = g_last.begin(); it != g_last.end();) {
			it = now - it->second.at > kStale ? g_last.erase(it) : std::next(it);
		}
	}
	Last& last = g_last[unit];
	bool fresh = last.at && now - last.at <= kStale;
	float from[3] = {last.pos[0], last.pos[1], last.pos[2]};
	memcpy(last.pos, pos, 12);
	last.at = now;
	if (!fresh) {
		return;
	}
	int what = 0;
	if (shell) {
		what = IsGrenade(unit) ? kOnewayGrenades : kOnewayBullets;
	}
	else {
		int type = unit[0x1f];
		what = type == 1 ? kOnewayPlayers : type == 2 ? kOnewayVs : type == 3 ? kOnewayAkrid : 0;
	}
	for (size_t i = 0; i < g_volumes.size(); i++) {
		const OnewayVolume& v = g_volumes[i];
		float dz = 0.0f;
		if (!(v.flags & what) || !ScriptVolumeOn(v.sid) || !Against(v, from, pos, &dz)) {
			continue;
		}
		if (!g_logged[i]) {
			g_logged[i] = true;
			XLS_LOG_INFO("mods: oneway: volume %u stopped a %s at %.0f %.0f %.0f.", (unsigned)i,
				what == kOnewayBullets ? "bullet" : what == kOnewayGrenades ? "grenade" : what == kOnewayVs ? "VS" :
				what == kOnewayAkrid ? "akrid" : "player", pos[0], pos[1], pos[2]);
		}
		if (shell) {
			uint32_t* state = (uint32_t*)(unit + 4);
			*state = (*state & ~7u) | 3;
			return;
		}
		// back along the box's z by the move's part against the arrow
		float back[3] = {pos[0] - dz * v.turn[1], pos[1], pos[2] - dz * v.turn[0]};
		for (uint32_t at : {0x30u, 0xd0u, g_build->oldPos, g_build->oldWorldPos}) {
			float* p = (float*)(unit + at);
			p[0] = back[0];
			p[2] = back[2];
		}
		memcpy(last.pos, back, 12);
		return;
	}
}

void __fastcall CharUpdate(uint8_t* unit)
{
	g_update(unit);
	if (!g_volumes.empty()) {
		Check(unit, false);
	}
}

void __fastcall ShellUpdate(uint8_t* unit)
{
	g_update(unit);
	if (!g_volumes.empty()) {
		Check(unit, true);
	}
}

}

void OnewayInstall(unsigned char* base)
{
	for (const Build& b : kBuilds) {
		if (GameBuild() >= 0 && &b - kBuilds != GameBuild()) {
			continue;
		}
		if (!IsCall(base, b.charCall, b.update) || !IsCall(base, b.shellCall, b.update)) {
			continue;
		}
		g_base = base;
		g_build = &b;
		g_update = (void(__fastcall*)(uint8_t*))(base + b.update);
		if (WriteCall(base + b.charCall, (void*)&CharUpdate) && WriteCall(base + b.shellCall, (void*)&ShellUpdate)) {
			XLS_LOG_INFO("mods: oneway: installed (%s).", BuildName((int)(&b - kBuilds)));
		}
		return;
	}
	XLS_LOG_WARN("mods: oneway: calls not recognised, one-way volumes ignored.");
}

void OnewaySetMap(const MapPhysics* physics)
{
	g_volumes = physics ? physics->oneway : std::vector<OnewayVolume>();
	g_logged.assign(g_volumes.size(), false);
	g_last.clear();
	if (!g_volumes.empty()) {
		XLS_LOG_INFO("mods: oneway: %u volume(s).", (unsigned)g_volumes.size());
	}
}

}
