#include "mods/lp2.h"
#include "mods/parts.h"

#include "core/log.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <unordered_map>

// Radar rules, for versus matches.
//
// One check decides nearly everything the radar shows: FUN_00f74980, thiscall(posts, float* pos), "is this position in range of
// a data post held by the viewer's team". Its callers are the map's fade (FUN_00edd730), whether a unit stands near such a post
// (its +0xd80 bit 15) and whether the viewer does (FUN_009bde40, the unit's blip). A blip needs both.
//   Always On: the check says yes. Hidden: FUN_00edd730 is told the radar is off, which is how the game hides it during a cutscene.
//
// Detection: FUN_009bde40 runs once per unit per frame and does nothing but add its blip, so not running it hides the blip. Enemy
// players (+0x1f == 1) and VS (category 2) go through it only while they do what the rule asks. It comes after the data post
// check by construction: it only takes away.
//   Speed comes from the unit's position (+0xd0), which every machine has for every unit.
//   A shot: FUN_00c7ce40, weapon's "spend a round", holder at weapon +0x2194 (DX9 -0x10). Called from each weapon's fire code with
//   no check for whose it is. Whether it runs for other machines' players needs a two-account test: see the log line below.
//   A grappling player reels faster than the run pace, so needs no case of its own; one hanging still counts as still.

namespace xls::mods {

namespace {

enum { kShowAlways, kShowMoving, kShowRunning, kShowShooting };
enum { kRadarAlways, kRadarNear, kRadarHidden };

const float kMovePace = 30.0f; // units a second: anything above noise
const float kRunPace = 300.0f; // faster than a crouching walk. MEASURE: "own speed" in the log
const DWORD kShotHold = 2000, kSample = 100;

const uint32_t kUnitPos = 0xd0, kUnitKind = 0x1f, kUnitSub = 0x1e;
struct Vas {
	uint32_t check, update, blip, shot, game, viewer, team, holder;
	uint8_t shotHead[5];
};
const Vas kVas[kBuildCount] = {
	{0xf74980, 0xedd730, 0x9bde40, 0xc7ce40, 0x18520c0, 0x1852078, 0xce2, 0x2194, {0xa1, 0xac, 0x08, 0x8d, 0x01}},
	{0xf64b20, 0xecd920, 0x9ae4d0, 0xc6d230, 0x1842020, 0x1841fd8, 0xcd2, 0x2184, {0xa1, 0x0c, 0x08, 0x8c, 0x01}},
	{0xf74e10, 0xeddb00, 0x9bdda0, 0xc7ce60, 0x18530c0, 0x1853078, 0xce2, 0x2194, {0xa1, 0xac, 0x18, 0x8d, 0x01}},
	{0xf64e00, 0xecd9c0, 0x9ae250, 0xc6ce20, 0x1842020, 0x1841fd8, 0xcd2, 0x2184, {0xa1, 0x0c, 0x08, 0x8c, 0x01}},
};
const uint8_t kFramePrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0};

using CheckFn = uint32_t(__attribute__((thiscall)) *)(void*, float*);
using UpdateFn = void(__attribute__((thiscall)) *)(void*, char, int, void*);
using UnitFn = void(__attribute__((thiscall)) *)(uint8_t*);
CheckFn g_checkOriginal = nullptr;
UpdateFn g_updateOriginal = nullptr;
UnitFn g_blipOriginal = nullptr, g_shotOriginal = nullptr;
const Vas* g_vas = nullptr;
uint8_t** g_game = nullptr;
uint8_t** g_viewer = nullptr;

std::atomic<int> g_mode{kRadarNear}, g_players{kShowAlways}, g_vs{kShowAlways};

struct Seen {
	float pos[3] = {0, 0, 0};
	DWORD at = 0, shot = 0;
	float speed = 0;
};
std::unordered_map<uint8_t*, Seen> g_seen; // game thread only: unit updates and weapon fire

bool Versus()
{
	return g_game && *g_game && (*g_game)[0x256] != 0;
}

// The viewer's character: holder +0x480 -> +0x40, each a live object (the game's FUN_007d6930).
uint8_t* Viewer()
{
	uint8_t* holder = g_viewer ? *g_viewer : nullptr;
	uint8_t* a = holder ? *(uint8_t**)(holder + 0x480) : nullptr;
	auto live = [](uint8_t* o) {
		uint32_t state = *(uint32_t*)(o + 4) & 7;
		return state == 1 || state == 2;
	};
	if (!a || !live(a)) {
		return nullptr;
	}
	uint8_t* unit = *(uint8_t**)(a + 0x40);
	return unit && live(unit) ? unit : nullptr;
}

int Category(const uint8_t* unit)
{
	if (unit[kUnitKind] != 3) {
		return unit[kUnitKind];
	}
	switch (unit[kUnitSub]) { // the game's FUN_009aad10
	case 0x12: case 0x20: case 0x21: case 0x22: case 0x23: case 0x26: case 0x27: case 0x28: case 0x29: case 0x30: case 0xc6:
		return 2;
	case 0x31:
		return 6;
	default:
		return 3;
	}
}

uint32_t __attribute__((thiscall)) Check(void* posts, float* pos)
{
	if (g_mode == kRadarAlways && Versus()) {
		return 1;
	}
	return g_checkOriginal(posts, pos);
}

void __attribute__((thiscall)) Update(void* cockpit, char on, int a, void* b)
{
	g_updateOriginal(cockpit, g_mode == kRadarHidden && Versus() ? 0 : on, a, b);
}

Seen& Track(uint8_t* unit, DWORD now)
{
	if (g_seen.size() > 512) {
		g_seen.clear();
	}
	Seen& s = g_seen[unit];
	DWORD gap = now - s.at;
	if (gap >= kSample) {
		const float* pos = (const float*)(unit + kUnitPos);
		float dx = pos[0] - s.pos[0], dy = pos[1] - s.pos[1], dz = pos[2] - s.pos[2];
		s.speed = s.at && gap < 2000 ? sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f / gap : 0;
		s.pos[0] = pos[0];
		s.pos[1] = pos[1];
		s.pos[2] = pos[2];
		s.at = now;
	}
	return s;
}

bool Detected(int rule, const Seen& s, DWORD now)
{
	if (s.shot && now - s.shot < kShotHold) {
		return true;
	}
	switch (rule) {
	case kShowMoving:
		return s.speed > kMovePace;
	case kShowRunning:
		return s.speed > kRunPace;
	case kShowShooting:
		return false;
	default:
		return true;
	}
}

void __attribute__((thiscall)) Blip(uint8_t* unit)
{
	int players = g_players, vs = g_vs;
	if ((players == kShowAlways && vs == kShowAlways) || !Versus()) {
		g_blipOriginal(unit);
		return;
	}
	DWORD now = GetTickCount();
	uint8_t* viewer = Viewer();
	Seen& s = Track(unit, now);
	if (unit == viewer) { // the number kRunPace is set from
		static DWORD logged = 0;
		static float peak = 0;
		peak = s.speed > peak ? s.speed : peak;
		if (now - logged > 1000) {
			XLS_LOG_INFO("mods: radar: own speed %.0f (kind %d).", peak, unit[kUnitKind]);
			logged = now;
			peak = 0;
		}
	}
	int category = Category(unit);
	int rule = category == 1 ? players : category == 2 ? vs : kShowAlways;
	bool enemy = viewer && unit != viewer && unit[g_vas->team] != viewer[g_vas->team];
	if (rule != kShowAlways && enemy && !Detected(rule, s, now)) {
		return;
	}
	g_blipOriginal(unit);
}

// Recent gunfire, for bots to hear (bots.cpp).
RadarShotInfo g_shots[32];
unsigned g_shotNext = 0;
std::mutex g_shotLock;

void __attribute__((thiscall)) Shot(uint8_t* weapon)
{
	if (Versus()) {
		uint8_t* who = *(uint8_t**)(weapon + g_vas->holder);
		if (who) {
			std::lock_guard<std::mutex> lock(g_shotLock);
			RadarShotInfo& s = g_shots[g_shotNext++ % 32];
			memcpy(s.pos, who + kUnitPos, sizeof(s.pos));
			s.who = who;
			s.team = who[g_vas->team];
			s.at = GetTickCount();
		}
	}
	if ((g_players != kShowAlways || g_vs != kShowAlways) && Versus()) {
		uint8_t* holder = *(uint8_t**)(weapon + g_vas->holder);
		if (holder) {
			DWORD now = GetTickCount();
			g_seen[holder].shot = now;
			static DWORD logged = 0;
			if (holder != Viewer() && now - logged > 5000) {
				logged = now; // two accounts: seeing this for a remote player says shots do reach here
				XLS_LOG_INFO("mods: radar: a shot from another unit (kind %d).", holder[kUnitKind]);
			}
		}
	}
	g_shotOriginal(weapon);
}

}

int RadarRecentShots(RadarShotInfo* out, int max, DWORD age)
{
	std::lock_guard<std::mutex> lock(g_shotLock);
	DWORD now = GetTickCount();
	int n = 0;
	for (const RadarShotInfo& s : g_shots) {
		if (n < max && s.who && now - s.at < age) {
			out[n++] = s;
		}
	}
	return n;
}

void RadarRulesSet(int mode, int players, int vs)
{
	g_mode = mode;
	g_players = players;
	g_vs = vs;
}

void RadarInstall(uint8_t* base, int build)
{
	const Vas& v = kVas[build];
	g_vas = &v;
	g_game = (uint8_t**)(base + (v.game - 0x400000));
	g_viewer = (uint8_t**)(base + (v.viewer - 0x400000));
	g_checkOriginal = (CheckFn)Detour(base + (v.check - 0x400000), kFramePrologue, sizeof(kFramePrologue), (void*)&Check);
	g_updateOriginal = (UpdateFn)Detour(base + (v.update - 0x400000), kFramePrologue, sizeof(kFramePrologue), (void*)&Update);
	g_blipOriginal = (UnitFn)Detour(base + (v.blip - 0x400000), kFramePrologue, sizeof(kFramePrologue), (void*)&Blip);
	g_shotOriginal = (UnitFn)Detour(base + (v.shot - 0x400000), v.shotHead, sizeof(v.shotHead), (void*)&Shot);
	if (!g_checkOriginal || !g_updateOriginal || !g_blipOriginal || !g_shotOriginal) {
		XLS_LOG_WARN("mods: radar: not all hooked: check %d, update %d, blip %d, shot %d.", g_checkOriginal != nullptr,
		             g_updateOriginal != nullptr, g_blipOriginal != nullptr, g_shotOriginal != nullptr);
	}
}

}
