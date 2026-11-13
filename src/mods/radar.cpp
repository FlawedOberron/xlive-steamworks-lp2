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
//
// Team colours: the game draws blips itself (the radar Flash only has the frame). FUN_00ed8aa0 adds one, thiscall(radar, kind,
// a, flags, float* pos, sub, b) (returns a pointer, see Add), into entry buffer +0x1bac (0x32 entries of 0x30 at radar+0x370 + buf*0x960:
// +0 kind, +2 flags, +3 sub, +0x10 pos). FUN_00edd730 reads the other buffer into the draw list (radar+0x1634, 0x1c each, count
// +0x1c0c: +0 code, +4 sub, +0x14/+0x18 the entry's pos x/z), sorted. FUN_00ed8fb0 draws one, thiscall(radar, ctx, code, sub,
// ...): code -> sprite + colour, then FUN_00ecc050 (sprite draw, colour its 7th argument). Players and VS: kinds 1-4 are codes
// 5-7 (green), kinds 5-9 codes 8-0xb (the game's red enemy markers; in a bot match it adds teammates as kind 5 too), so both
// ranges are recoloured. Here: each add notes its unit's side by slot; after the list is built a player entry gets the side in
// its sub (unused for those codes), matched back to its entry by position; the draw passes our colour to the sprite draw.

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
	uint32_t add, draw, sprite, setup;
};
const Vas kVas[kBuildCount] = {
	{0xf74980, 0xedd730, 0x9bde40, 0xc7ce40, 0x18520c0, 0x1852078, 0xce2, 0x2194, {0xa1, 0xac, 0x08, 0x8d, 0x01}, 0xed8aa0, 0xed8fb0, 0xecc050, 0x1852850},
	{0xf64b20, 0xecd920, 0x9ae4d0, 0xc6d230, 0x1842020, 0x1841fd8, 0xcd2, 0x2184, {0xa1, 0x0c, 0x08, 0x8c, 0x01}, 0xec8c90, 0xec91a0, 0xebc250, 0x18427b0},
	{0xf74e10, 0xeddb00, 0x9bdda0, 0xc7ce60, 0x18530c0, 0x1853078, 0xce2, 0x2194, {0xa1, 0xac, 0x18, 0x8d, 0x01}, 0xed8e70, 0xed9380, 0xecc420, 0x1853850},
	{0xf64e00, 0xecd9c0, 0x9ae250, 0xc6ce20, 0x1842020, 0x1841fd8, 0xcd2, 0x2184, {0xa1, 0x0c, 0x08, 0x8c, 0x01}, 0xec8d30, 0xec9240, 0xebc2f0, 0x18427b0},
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

std::atomic<int> g_mode{kRadarNear}, g_players{kShowAlways}, g_vs{kShowAlways}, g_team{1};

enum { kSideNone, kSideOwn, kSideEnemy };
// ABGR, as the radar draw uses them (FUN_00ed8fb0: enemy codes 8-0xd 0xff4040ff red, code 0x21 0xffff8060 blue): blue, red
const uint32_t kColourOwn = 0xffff8060, kColourEnemy = 0xff4040ff;
const uint32_t kSideMark = 0x5a00; // in a draw entry's sub: kSideMark + side
using AddFn = int(__attribute__((thiscall)) *)(uint8_t*, uint32_t, uint32_t, uint32_t, float*, uint32_t, uint32_t);
using DrawFn = void(__attribute__((thiscall)) *)(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
AddFn g_addOriginal = nullptr;
DrawFn g_drawOriginal = nullptr, g_spriteOriginal = nullptr;
uint8_t** g_setup = nullptr;
thread_local int t_side = kSideNone;      // the unit whose blip is being added (Blip)
thread_local uint32_t t_colour = 0;        // the colour for the sprite being drawn (Draw)
uint8_t g_sides[2][0x32];                   // per entry buffer and slot

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
	if ((uintptr_t)a < 0x10000 || !live(a)) {
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

void MarkSides(uint8_t* radar, int read);

uint32_t __attribute__((thiscall)) Check(void* posts, float* pos)
{
	CostScope cost(kCostRadar);
	if (g_mode == kRadarAlways && Versus()) {
		return 1;
	}
	return g_checkOriginal(posts, pos);
}

void __attribute__((thiscall)) Update(void* cockpit, char on, int a, void* b)
{
	int read = 1 - (((uint8_t*)cockpit)[0x1bac] & 1); // the entry buffer it builds the draw list from
	g_updateOriginal(cockpit, g_mode == kRadarHidden && Versus() ? 0 : on, a, b);
	if (g_drawOriginal && g_spriteOriginal && g_team) {
		MarkSides((uint8_t*)cockpit, read);
	}
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

// Own team or enemy, seen from the viewer; in a game type without teams everyone else is an enemy. Whether it's a team
// type comes from the rules code (RulesTeams: the lobby's rule, or the bot match screen's; the match setup's rule number
// is 1 for every bot match, team or not, which drew every blip as an enemy there). Teams by match slot where both
// have one, else the units' team bytes (a VS).
int Side(uint8_t* unit)
{
	uint8_t* viewer = g_team && Versus() ? Viewer() : nullptr;
	if (!viewer || !unit || unit == viewer) {
		return kSideNone;
	}
	bool teams = RulesTeams();
	int side = kSideEnemy, mine = RulesSlotOf(viewer), theirs = RulesSlotOf(unit);
	int myTeam = mine >= 0 ? RulesTeamOf(mine) : -1, theirTeam = theirs >= 0 ? RulesTeamOf(theirs) : -1;
	if (teams) {
		bool same = myTeam >= 0 && theirTeam >= 0 ? myTeam == theirTeam : unit[g_vas->team] == viewer[g_vas->team];
		side = same ? kSideOwn : kSideEnemy;
	}
	static LONG logged = 0;
	if (logged < 12 && InterlockedIncrement(&logged) <= 12) {
		XLS_LOG_INFO("mods: radar: team colours: team game %d; viewer slot %d team %d (byte %u), blip slot %d team %d (byte %u): %s.", (int)teams, mine,
		             myTeam, viewer[g_vas->team], theirs, theirTeam, unit[g_vas->team], side == kSideOwn ? "own team, blue" : "enemy, red");
	}
	return side;
}

void BlipAdd(uint8_t* unit)
{
	t_side = Side(unit);
	g_blipOriginal(unit);
	t_side = kSideNone;
}

void __attribute__((thiscall)) Blip(uint8_t* unit)
{
	CostScope cost(kCostRadar);
	int players = g_players, vs = g_vs;
	if ((players == kShowAlways && vs == kShowAlways) || !Versus()) {
		BlipAdd(unit);
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
	BlipAdd(unit);
}

int __attribute__((thiscall)) Add(uint8_t* radar, uint32_t kind, uint32_t a, uint32_t flags, float* pos, uint32_t sub, uint32_t b)
{
	CostScope cost(kCostRadar);
	// The add returns a pointer on its usual path, not the entry: a new entry goes at the buffer's count (+0x1630 + buf * 2,
	// raised by one); with the buffer full it replaces the farthest one and returns its index (when b is 0).
	int buf = radar[0x1bac] & 1;
	int before = *(int16_t*)(radar + 0x1630 + buf * 2);
	int ret = g_addOriginal(radar, kind, a, flags, pos, sub, b);
	int after = *(int16_t*)(radar + 0x1630 + buf * 2);
	int slot = after == before + 1 ? before : (after == before && (uint8_t)b == 0 ? ret : -1);
	if (slot >= 0 && slot < 0x32) {
		g_sides[buf][slot] = (uint8_t)t_side;
		static bool logged[16];
		uint8_t k = (uint8_t)kind;
		if (t_side != kSideNone && k < 16 && !logged[k]) { // which kinds own team and enemies use
			logged[k] = true;
			XLS_LOG_INFO("mods: radar: blip kind %u for %s.", (unsigned)k, t_side == kSideOwn ? "own team" : "an enemy");
		}
	}
	return ret;
}

// After the draw list is built: a player / VS entry (codes 5-0xb) gets its unit's side, found by position in the buffer it came from.
void MarkSides(uint8_t* radar, int read)
{
	int count = *(int32_t*)(radar + 0x1c0c);
	int entries = *(int16_t*)(radar + 0x1630 + read * 2);
	if (count <= 0 || count > 0x32 || entries <= 0 || entries > 0x32) {
		return;
	}
	for (int i = 0; i < count; i++) {
		uint8_t* d = radar + 0x1634 + i * 0x1c;
		int code = d[0] & 0x7f;
		if (code < 5 || code > 0xb) {
			continue;
		}
		float x = *(float*)(d + 0x14), z = *(float*)(d + 0x18);
		for (int j = 0; j < entries; j++) {
			uint8_t* e = radar + 0x370 + (read * 0x32 + j) * 0x30;
			if (e[0] >= 1 && e[0] <= 9 && *(float*)(e + 0x10) == x && *(float*)(e + 0x18) == z) {
				if (g_sides[read][j] != kSideNone) {
					*(uint32_t*)(d + 4) = kSideMark + g_sides[read][j];
				}
				break;
			}
		}
	}
}

void __attribute__((thiscall)) Draw(uint8_t* radar, uint32_t ctx, uint32_t code, uint32_t sub, uint32_t a, uint32_t x, uint32_t y, uint32_t b, uint32_t scale)
{
	uint32_t c = code & 0x7f;
	if (c >= 5 && c <= 0xb && (sub == kSideMark + kSideOwn || sub == kSideMark + kSideEnemy)) {
		t_colour = sub == kSideMark + kSideOwn ? kColourOwn : kColourEnemy;
		g_drawOriginal(radar, ctx, code, 0, a, x, y, b, scale);
		t_colour = 0;
		return;
	}
	g_drawOriginal(radar, ctx, code, sub, a, x, y, b, scale);
}

void __attribute__((thiscall)) Sprite(uint8_t* self, uint32_t ctx, uint32_t sprite, uint32_t a, uint32_t x, uint32_t y, uint32_t b, uint32_t colour, uint32_t scale)
{
	g_spriteOriginal(self, ctx, sprite, a, x, y, b, t_colour ? t_colour : colour, scale);
}

// Recent gunfire, for bots to hear (bots.cpp).
RadarShotInfo g_shots[32];
unsigned g_shotNext = 0;
std::mutex g_shotLock;

void __attribute__((thiscall)) Shot(uint8_t* weapon)
{
	CostScope cost(kCostRadar);
	if (Versus()) {
		uint8_t* who = *(uint8_t**)(weapon + g_vas->holder);
		if (who) {
			std::lock_guard<std::mutex> lock(g_shotLock);
			RadarShotInfo& s = g_shots[g_shotNext++ % 32];
			memcpy(s.pos, who + kUnitPos, sizeof(s.pos));
			UnitFacing(who, s.dir);
			s.who = who;
			static bool told = false;
			if (!told) { // the facing is read from the world matrix's Z row: one line to check it against the quaternion at +0x40
				told = true;
				const float* q = (const float*)(who + 0x40);
				float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]), fz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
				XLS_LOG_INFO("mods: radar: first shot: facing %.2f %.2f (matrix), %.2f %.2f (quaternion %.2f %.2f %.2f %.2f).", s.dir[0], s.dir[1], fx, fz, q[0], q[1], q[2], q[3]);
			}
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

void RadarRulesSet(int mode, int players, int vs, int team)
{
	g_team = team;
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
	g_setup = (uint8_t**)(base + (v.setup - 0x400000));
	// the draw: push ebx; push ebp; push esi; mov esi,[a global] (the address differs per build)
	uint8_t* draw = base + (v.draw - 0x400000);
	uint8_t drawHead[9];
	memcpy(drawHead, draw, sizeof(drawHead));
	const uint8_t kDrawStart[] = {0x53, 0x55, 0x56, 0x8b, 0x35};
	if (memcmp(drawHead, kDrawStart, sizeof(kDrawStart)) == 0) {
		g_spriteOriginal = (DrawFn)Detour(base + (v.sprite - 0x400000), kFramePrologue, sizeof(kFramePrologue), (void*)&Sprite);
		g_drawOriginal = g_spriteOriginal ? (DrawFn)Detour(draw, drawHead, sizeof(drawHead), (void*)&Draw) : nullptr;
		g_addOriginal = g_drawOriginal ? (AddFn)Detour(base + (v.add - 0x400000), kFramePrologue, sizeof(kFramePrologue), (void*)&Add) : nullptr;
	}
	if (!g_addOriginal) {
		XLS_LOG_WARN("mods: radar: blip draw not hooked, no team colours.");
	}
	if (!g_checkOriginal || !g_updateOriginal || !g_blipOriginal || !g_shotOriginal) {
		XLS_LOG_WARN("mods: radar: not all hooked: check %d, update %d, blip %d, shot %d.", g_checkOriginal != nullptr,
		             g_updateOriginal != nullptr, g_blipOriginal != nullptr, g_shotOriginal != nullptr);
	}
}

}
