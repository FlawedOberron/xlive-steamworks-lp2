#include "mods/lp2.h"
#include "mods/packages.h"

#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <vector>

// Spawning rules (Game Options > Spawning). Default leaves the game's respawn as it is: the death camera, then the
// respawn menu (pick a base or an owned data post, with its own timer), then the spawn. The instant picks keep the death
// camera, skip the menu and put the new character at a point of ours:
//
//   Controller (rules.cpp ControlUpdate; DX11 FUN_008ff650) +0x19 state: 1 dead: +0x100 the death camera's timer, then
//   the menu while +0xfc runs (the cockpit, uIdCockpitGFx FUN_00ee01e0, shows it while +0x100 <= 0 < +0xfc); 2 sets 3
//   and clears the pick's own state (+0x64), 3 counts +0x104 down, 4 the spawn handshake (FUN_008fa660 on +0x64, talks
//   to the host), 5 the spawn (FUN_008fb130, at the game's chosen point), 6 alive.
//   Once the death camera's timer is done (RespawnAfter, right after the controller's update and before the cockpit's)
//   both timers are cleared and the controller goes to 4 with +0x64 cleared; once the spawn has run (state 6 or later)
//   the character is moved to our point (CharacterTeleport, on the PC that runs that player: its own, the host its bots).
//
// Points:
//   the match's start points: game +0x52e4 rInitPosSet (count +0x70, records +0x7c: pos +0x10, dir u16 degrees +0x20,
//     kind byte +0x26: 0 free-for-all, 1 / 2 the teams, 3 FFA extras, 6 / 7 bases (never used here)).
//   team games load the mode's set only (16 + 16 team points), so the map's free-for-all set (the same file name with
//     KojinBattle: "tool\InitPos\Online\<game +0xac>-KojinBattle-<game +0x255 %02d>", as the area load FUN_0087e290 makes
//     it) is fetched through the resource manager (sResource vf11, as mappics.cpp) and its points copied.
//   the respawn table (what the dialog offers): game +0xd6c count (s8), per slot +0xe80 + slot * 0x100 + i * 0x10 pos,
//     +0xd80 + slot * 0x10 + i owner (s8); entry 0 the base, the rest allowed when owner == the slot's team (the AI's pick,
//     FUN_007f27d0). Free-for-all: entries before game +0x4e90 (s8) are the kind 3 points, data posts after; team games
//     every entry past 0 is a data post.
// Threat (Safe / Unsafe): living enemies near the point, deaths near it in the last kDeathFade (team games: allies' only),
// and spawns there in the last kReuseFor. A point with an enemy closer than kEnemyFloor is left out while others remain.

namespace xls::mods {

namespace {

enum Pick { kPickDefault, kPickRandom, kPickSafe, kPickUnsafe };
enum Area { kAreaMap, kAreaAllies, kAreaZonesSet, kAreaZonesDynamic };
enum Distance { kDistAny, kDistNear, kDistMedium, kDistFar };
enum Posts { kPostsDefault, kPostsNearby, kPostsIgnored };

const float kEnemyFloor = 1200.0f;     // cm: never spawn this close to a living enemy while another point is free
const float kAllyReach = 2500.0f;      // Near Allies: points within this of a living team mate
const float kPostReach = 2000.0f;      // Data Posts, Add Nearby Points: points within this of an owned post
const float kDeathReach = 2000.0f;     // a death counts for points within this
const DWORD kDeathFade = 15000, kReuseFor = 8000;
const float kZoneFlipSeconds = 10.0f;  // Zones: Dynamic: held by the other team alone this long
const int kBest = 3;                   // Safe / Unsafe: the pick is weighted among this many best points

const uint32_t kGameSet = 0x52e4, kSetCount = 0x70, kSetArray = 0x7c, kRecPos = 0x10, kRecDir = 0x20, kRecKind = 0x26;
const uint32_t kGameArea = 0xac, kGameVariant = 0x255, kGameVersus = 0x256, kGameMap = 0x25a;
const uint32_t kTableCount = 0xd6c, kTableOwner = 0xd80, kTablePos = 0xe80, kTablePostBase = 0x4e90;
const uint32_t kCtlState = 0x19, kCtlPick = 0x64, kCtlBody = 0x40, kCtlTimers = 0xfc; // timers: +0xfc the menu, +0x100 the death camera
const uint8_t kStateDeath = 1, kStateCount = 2, kStatePick = 4, kStateAlive = 6;

// sResource (as mappics.cpp): the manager global (RVAs); its vf11 finds / loads (flags 0x80: find only), vf13 releases.
// Called through the manager's own vtable, as the area load does: this runs on the game thread.
const uint32_t kManager[kBuildCount] = {0x14d0ccc, 0x14c0c2c, 0x14d1ccc, 0x14c0c2c}; // DX11, DX9, DX11 1.1, DX9 1.1
typedef void*(__fastcall* FindFn)(void* self, void* edx, void* dti, const char* path, uint32_t flags);
typedef void(__fastcall* ReleaseFn)(void* self, void* edx, void* res);
typedef void*(__attribute__((thiscall)) * DtiFn)(void* self);

struct Point {
	float pos[3];
	float yaw;   // degrees, NaN: keep the spawn's facing
	int kind;    // the record's kind, -1 a data post
};
struct Death {
	float pos[3];
	int team;
	DWORD at;
};
struct Zone {
	SpawnVolume v;
	int owner;       // the team spawning there now (-1 nobody)
	float held[2];   // Dynamic: seconds the other team has held it alone
};

uint8_t* g_base = nullptr;
uint8_t** g_game = nullptr;
std::atomic<int> g_pick{0}, g_area{0}, g_distance{0}, g_posts{0};

// game thread from here on (controller updates, the HUD frame, area loads)
std::vector<Zone> g_zones;
std::vector<Point> g_ffa;           // the map's free-for-all set (team games)
std::string g_ffaFor;               // its file name ("" none fetched)
std::vector<Death> g_deaths, g_spawns;
LifeState g_lastState[16] = {};
float g_lastPos[16][3] = {};
bool g_hasPos[16] = {};
struct Pending {
	bool on = false;
	Point at;
	DWORD since = 0;
};
Pending g_pending[16];
DWORD g_lastFrame = 0;
std::mt19937 g_rng{GetTickCount()};

uint8_t* Game()
{
	return g_game && *g_game ? *g_game : nullptr;
}

float Dist2(const float* a, const float* b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

void** Manager()
{
	int b = GameBuild();
	if (b < 0 || b >= kBuildCount) {
		return nullptr;
	}
	void** m = *(void***)(g_base + kManager[b]);
	return m && !IsBadReadPtr(m, sizeof(void*)) ? m : nullptr;
}

void AddSet(uint8_t* set, std::vector<Point>* out, bool (*want)(int kind))
{
	uint32_t count = set ? *(uint32_t*)(set + kSetCount) : 0;
	uint8_t** records = set ? *(uint8_t***)(set + kSetArray) : nullptr;
	for (uint32_t i = 0; records && i < count && count <= 256; i++) {
		uint8_t* r = records[i];
		if (r && want(r[kRecKind])) {
			Point p;
			memcpy(p.pos, r + kRecPos, sizeof(p.pos));
			p.yaw = (float)(*(uint16_t*)(r + kRecDir) % 360);
			p.kind = r[kRecKind];
			out->push_back(p);
		}
	}
}

// Team games: the map's free-for-all points, from its KojinBattle set of the same variant (kept per file name).
void FetchFfa(uint8_t* game)
{
	char path[160];
	const char* area = (const char*)(game + kGameArea);
	if (!memchr(area, 0, 64)) {
		return;
	}
	snprintf(path, sizeof path, "tool\\InitPos\\Online\\%s-KojinBattle-%02d", area, game[kGameVariant]);
	if (g_ffaFor == path) {
		return;
	}
	g_ffaFor = path;
	g_ffa.clear();
	uint8_t* own = *(uint8_t**)(game + kGameSet);
	void** m = Manager();
	if (!own || !m) {
		return;
	}
	void** vt = *(void***)m;
	void* dti = ((DtiFn)(*(void***)own)[4])(own); // getDTI (DX11 rInitPosSet::getDTI 0xfd1fa0 is vf4): the loaded set's type
	uint8_t* set = (uint8_t*)((FindFn)vt[11])(m, nullptr, dti, path, 0x80); // resident already (the area's archive)?
	bool loaded = false;
	if (!set) {
		set = (uint8_t*)((FindFn)vt[11])(m, nullptr, dti, path, 1); // as the area load asks for its own set
		loaded = set != nullptr;
	}
	if (!set) {
		XLS_LOG_INFO("mods: respawn: %s not found: team games use the team points only.", path);
		return;
	}
	AddSet(set, &g_ffa, [](int kind) { return kind == 0 || kind == 3; });
	((ReleaseFn)vt[13])(m, nullptr, set);
	XLS_LOG_INFO("mods: respawn: %u free-for-all points from %s (%s).", (unsigned)g_ffa.size(), path, loaded ? "loaded" : "resident");
}

bool Teams()
{
	return RulesTeams();
}

bool Enemy(int a, int b)
{
	return a != b && (!Teams() || RulesTeamOf(a) != RulesTeamOf(b));
}

bool Alive(int slot)
{
	return RulesInPlay(slot) && LivesState(slot) == LifeState::Alive && RulesBody(slot);
}

const float* BodyPos(int slot)
{
	uint8_t* body = RulesBody(slot);
	return body ? (const float*)(body + 0x30) : nullptr;
}

// The team kind (1 / 2) of a slot's points: the one whose points lie nearest its base (respawn table entry 0), else
// team 0 -> 1.
int TeamKind(uint8_t* game, int slot, const std::vector<Point>& own)
{
	int team = RulesTeamOf(slot);
	const float* base = (const float*)(game + kTablePos + slot * 0x100);
	if (base[0] != 0.0f || base[2] != 0.0f) {
		float best[3] = {1e30f, 1e30f, 1e30f};
		for (const Point& p : own) {
			if (p.kind == 1 || p.kind == 2) {
				best[p.kind] = std::min(best[p.kind], Dist2(p.pos, base));
			}
		}
		if (best[1] < 1e29f && best[2] < 1e29f) {
			return best[1] <= best[2] ? 1 : 2;
		}
	}
	return team == 1 ? 2 : 1;
}

// the slot's owned data posts (the dialog's own rule), positions from the respawn table
void Posts(uint8_t* game, int slot, std::vector<Point>* out)
{
	int count = (int8_t)game[kTableCount];
	int first = Teams() ? 1 : std::max(1, (int)(int8_t)game[kTablePostBase]);
	int8_t team = (int8_t)(RulesTeamOf(slot) & 0xff);
	for (int i = first; i < count && i < 16; i++) {
		if ((int8_t)game[kTableOwner + slot * 0x10 + i] != team) {
			continue;
		}
		Point p;
		memcpy(p.pos, game + kTablePos + slot * 0x100 + i * 0x10, sizeof(p.pos));
		p.yaw = NAN;
		p.kind = -1;
		if (p.pos[0] != 0.0f || p.pos[1] != 0.0f || p.pos[2] != 0.0f) {
			out->push_back(p);
		}
	}
}

int ZoneOwner(const Zone& z)
{
	return g_area == kAreaZonesDynamic ? z.owner : z.v.team;
}

float Danger(const Point& p, int slot)
{
	float d = 0.0f;
	for (int s = 0; s < 16; s++) {
		const float* at = s != slot && Enemy(slot, s) && Alive(s) ? BodyPos(s) : nullptr;
		if (at) {
			float r = sqrtf(Dist2(p.pos, at)) / 1500.0f;
			d += 4.0f / (1.0f + r * r);
		}
	}
	DWORD now = GetTickCount();
	int team = Teams() ? RulesTeamOf(slot) : -1;
	for (const Death& x : g_deaths) {
		if ((team < 0 || x.team == team) && now - x.at < kDeathFade && Dist2(p.pos, x.pos) < kDeathReach * kDeathReach) {
			d += 2.0f * (1.0f - (float)(now - x.at) / kDeathFade);
		}
	}
	return d;
}

float Reuse(const Point& p)
{
	DWORD now = GetTickCount();
	float r = 0.0f;
	for (const Death& x : g_spawns) {
		if (now - x.at < kReuseFor && Dist2(p.pos, x.pos) < 500.0f * 500.0f) {
			r += 3.0f;
		}
	}
	return r;
}

const char* PointName(const Point& p)
{
	switch (p.kind) {
	case -1: return "data post";
	case 0: return "free-for-all point";
	case 1: case 2: return "team point";
	default: return "extra point";
	}
}

// The point a slot respawns at, by the rules; false: none (the game's own spawn stands).
bool Choose(uint8_t* game, int slot, Point* out, std::string* why)
{
	std::vector<Point> own; // the match's loaded set
	AddSet(*(uint8_t**)(game + kGameSet), &own, [](int kind) { return kind <= 3; });
	bool teams = Teams();
	std::vector<Point> all, pool;
	if (teams) {
		FetchFfa(game);
		int kind = TeamKind(game, slot, own);
		all = g_ffa;
		std::vector<Point> mine;
		for (const Point& p : own) {
			(p.kind == kind ? mine : all).push_back(p); // the enemy's points only count inside zones
		}
		std::vector<Point> enemyPts(all.begin() + g_ffa.size(), all.end());
		all.resize(g_ffa.size());
		int area = g_area;
		if (area == kAreaZonesSet || area == kAreaZonesDynamic) {
			int team = RulesTeamOf(slot);
			std::vector<Point> any = all;
			any.insert(any.end(), mine.begin(), mine.end());
			any.insert(any.end(), enemyPts.begin(), enemyPts.end());
			for (const Point& p : any) {
				for (const Zone& z : g_zones) {
					float probe[3] = {p.pos[0], p.pos[1] + 50.0f, p.pos[2]};
					if (ZoneOwner(z) == team && InBox(z.v.min, z.v.max, z.v.turn, probe)) {
						pool.push_back(p);
						break;
					}
				}
			}
			*why = g_zones.empty() ? "no spawn zones on this map" : pool.empty() ? "no points in the team's zones" : "zones";
		}
		else if (area == kAreaAllies) {
			std::vector<const float*> mates;
			for (int s = 0; s < 16; s++) {
				if (s != slot && !Enemy(slot, s) && Alive(s)) {
					mates.push_back(BodyPos(s));
				}
			}
			std::vector<std::pair<float, Point>> byMate;
			std::vector<Point> any = all;
			any.insert(any.end(), mine.begin(), mine.end());
			for (const Point& p : any) {
				float best = 1e30f;
				for (const float* m : mates) {
					best = std::min(best, Dist2(p.pos, m));
				}
				byMate.push_back({best, p});
			}
			std::sort(byMate.begin(), byMate.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			for (size_t i = 0; i < byMate.size() && !mates.empty(); i++) {
				if (byMate[i].first <= kAllyReach * kAllyReach || i < (size_t)kBest) {
					pool.push_back(byMate[i].second);
				}
			}
			*why = mates.empty() ? "no living team mates" : "near allies";
		}
		if (pool.empty()) { // the whole map: own team's points and the free-for-all ones
			pool = all;
			pool.insert(pool.end(), mine.begin(), mine.end());
			if (why->empty()) {
				*why = "whole map";
			}
			else {
				*why += ", whole map instead";
			}
		}
		all.insert(all.end(), mine.begin(), mine.end());
	}
	else {
		for (const Point& p : own) {
			if (p.kind == 0 || p.kind == 3) {
				pool.push_back(p);
			}
		}
		all = pool;
		*why = "free-for-all points";
	}
	// data posts
	if (g_posts != kPostsIgnored) {
		std::vector<Point> posts;
		Posts(game, slot, &posts);
		for (const Point& post : posts) {
			pool.push_back(post);
			if (g_posts == kPostsNearby) {
				for (const Point& p : all) {
					if (Dist2(p.pos, post.pos) < kPostReach * kPostReach) {
						pool.push_back(p);
					}
				}
			}
		}
		if (!posts.empty()) {
			*why += FormatA(", %u data post(s)", (unsigned)posts.size());
		}
	}
	if (pool.empty()) {
		return false;
	}
	// not on top of a living enemy
	std::vector<Point> safe;
	for (const Point& p : pool) {
		bool close = false;
		for (int s = 0; s < 16 && !close; s++) {
			const float* at = s != slot && Enemy(slot, s) && Alive(s) ? BodyPos(s) : nullptr;
			close = at && Dist2(p.pos, at) < kEnemyFloor * kEnemyFloor;
		}
		if (!close) {
			safe.push_back(p);
		}
	}
	if (!safe.empty()) {
		pool.swap(safe);
	}
	// Spawn Distance: from where the player died, the nearest / middle / farthest third of the points
	if (g_distance != kDistAny && g_hasPos[slot]) {
		std::vector<std::pair<float, Point>> by;
		for (const Point& p : pool) {
			by.push_back({Dist2(p.pos, g_lastPos[slot]), p});
		}
		std::sort(by.begin(), by.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		size_t n = by.size(), third = std::max<size_t>(1, (n + 2) / 3);
		size_t lo = g_distance == kDistNear ? 0 : g_distance == kDistMedium ? std::min(third, n - 1) : n - std::min(third, n);
		size_t hi = std::min(n, lo + third);
		pool.clear();
		for (size_t i = lo; i < hi; i++) {
			pool.push_back(by[i].second);
		}
	}
	int pick = g_pick;
	if (pick == kPickRandom) {
		*out = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(g_rng)];
		return true;
	}
	std::vector<std::pair<float, Point>> scored;
	for (const Point& p : pool) {
		float d = Danger(p, slot), r = Reuse(p);
		scored.push_back({pick == kPickSafe ? d + r : -d + r, p}); // lowest first; reuse is avoided both ways
	}
	std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	int n = std::min<int>(kBest, (int)scored.size()), total = n * (n + 1) / 2;
	int roll = std::uniform_int_distribution<int>(0, total - 1)(g_rng);
	for (int i = 0; i < n; i++) {
		roll -= n - i; // weights n, n-1 .. 1
		if (roll < 0) {
			*out = scored[i].second;
			return true;
		}
	}
	*out = scored[0].second;
	return true;
}

// Zones: Dynamic: a zone goes to the other team once that team alone has held it kZoneFlipSeconds, if it borders (by
// order) a zone that team owns, and never a team's last zone.
void ZonesTick(float dt)
{
	if (g_area != kAreaZonesDynamic || g_zones.empty() || !Teams()) {
		return;
	}
	for (Zone& z : g_zones) {
		int in[2] = {0, 0};
		for (int s = 0; s < 16; s++) {
			const float* at = Alive(s) ? BodyPos(s) : nullptr;
			int t = RulesTeamOf(s);
			if (at && (t == 0 || t == 1)) {
				float probe[3] = {at[0], at[1] + 50.0f, at[2]};
				in[t] += InBox(z.v.min, z.v.max, z.v.turn, probe) ? 1 : 0;
			}
		}
		for (int t = 0; t < 2; t++) {
			bool alone = in[t] > 0 && in[1 - t] == 0 && z.owner != t;
			z.held[t] = alone ? z.held[t] + dt : 0.0f;
			if (z.held[t] < kZoneFlipSeconds) {
				continue;
			}
			z.held[t] = 0.0f;
			int ownedBefore = 0;
			bool borders = true, ordered = false;
			for (const Zone& o : g_zones) {
				ownedBefore += o.owner == z.owner ? 1 : 0;
				ordered |= o.v.order != z.v.order;
			}
			if (ordered) {
				borders = false;
				for (const Zone& o : g_zones) {
					borders |= o.owner == t && abs(o.v.order - z.v.order) <= 1;
				}
			}
			if (!borders || (z.owner >= 0 && ownedBefore <= 1)) {
				continue;
			}
			int was = z.owner;
			z.owner = t;
			XLS_LOG_INFO("mods: respawn: spawn zone %d (order %d) -> team %d (was %d).", (int)(&z - g_zones.data()), z.v.order, t, was);
			int self = RulesLocalSlot();
			int mine = self >= 0 ? RulesTeamOf(self) : -1;
			if (mine == t) {
				LivesMessage("Spawn zone captured", 1, 0x7fd8ff, 3.0f);
			}
			else if (mine == was) {
				LivesMessage("Spawn zone lost", 1, 0xff7a5a, 3.0f);
			}
		}
	}
}

bool On(uint8_t* game)
{
	return game && game[kGameVersus] && g_pick != kPickDefault && !ModeActive();
}

}

void RespawnInit(unsigned char* base, unsigned char** game)
{
	g_base = base;
	g_game = game;
}

void RespawnRulesSet(int pick, int area, int distance, int posts)
{
	if (pick != g_pick || area != g_area || distance != g_distance || posts != g_posts) {
		XLS_LOG_INFO("mods: respawn: rules: pick %d, area %d, distance %d, data posts %d.", pick, area, distance, posts);
	}
	g_pick = pick;
	g_area = area;
	g_distance = distance;
	g_posts = posts;
}

void RespawnSetMap(const MapPhysics* physics)
{
	g_zones.clear();
	for (const SpawnVolume& v : physics ? physics->spawn : std::vector<SpawnVolume>()) {
		Zone z;
		z.v = v;
		z.owner = v.team;
		z.held[0] = z.held[1] = 0.0f;
		g_zones.push_back(z);
	}
	g_ffa.clear();
	g_ffaFor.clear();
	g_deaths.clear();
	g_spawns.clear();
	for (int s = 0; s < 16; s++) {
		g_lastState[s] = LifeState::None;
		g_hasPos[s] = false;
		g_pending[s].on = false;
	}
	if (!g_zones.empty()) {
		XLS_LOG_INFO("mods: respawn: %u spawn zone(s).", (unsigned)g_zones.size());
	}
}

void RespawnFrame()
{
	uint8_t* game = Game();
	DWORD now = GetTickCount();
	float dt = g_lastFrame && now - g_lastFrame < 1000 ? (now - g_lastFrame) / 1000.0f : 0.0f;
	g_lastFrame = now;
	if (!game || !game[kGameVersus]) {
		return;
	}
	for (int s = 0; s < 16; s++) {
		LifeState st = RulesInPlay(s) ? LivesState(s) : LifeState::None;
		const float* at = st == LifeState::Alive ? BodyPos(s) : nullptr;
		if (st == LifeState::Dead && g_lastState[s] == LifeState::Alive && g_hasPos[s]) {
			Death d = {{g_lastPos[s][0], g_lastPos[s][1], g_lastPos[s][2]}, RulesTeamOf(s), now};
			g_deaths.push_back(d);
		}
		if (at && g_lastState[s] != LifeState::Alive && g_lastState[s] != LifeState::None) {
			Death d = {{at[0], at[1], at[2]}, RulesTeamOf(s), now};
			g_spawns.push_back(d);
		}
		if (at) {
			memcpy(g_lastPos[s], at, sizeof(g_lastPos[s]));
			g_hasPos[s] = true;
		}
		g_lastState[s] = st;
	}
	auto old = [&](const Death& d) { return now - d.at > kDeathFade; };
	g_deaths.erase(std::remove_if(g_deaths.begin(), g_deaths.end(), old), g_deaths.end());
	g_spawns.erase(std::remove_if(g_spawns.begin(), g_spawns.end(), old), g_spawns.end());
	ZonesTick(dt);
}

// Called after the controller's update (rules.cpp), on the PC that runs this player. free: no survival hold or
// round-out wait on it. The respawn menu (the cockpit, uIdCockpitGFx, FUN_00ee01e0) shows while the death view's
// camera timer (+0x100) has run out and its menu timer (+0xfc) hasn't; state 1 goes on to 2 when the menu timer runs
// out or the pick is confirmed. So the moment the camera timer is done (or the game went to 2 itself) both timers are
// cleared and the controller goes straight to the spawn handshake (4) in this same update, before the cockpit's.
void RespawnAfter(unsigned char* ctl, int slot, bool free)
{
	if (!ctl || slot < 0 || slot >= 16) {
		return;
	}
	uint8_t* game = Game();
	float* timers = (float*)(ctl + kCtlTimers);
	bool viewDone = ctl[kCtlState] == kStateDeath && !(timers[1] > 0.0f);
	if (free && On(game) && (viewDone || ctl[kCtlState] == kStateCount) && !g_pending[slot].on) {
		Point p;
		std::string why;
		g_pending[slot].on = Choose(game, slot, &p, &why);
		if (g_pending[slot].on) {
			g_pending[slot].at = p;
			g_pending[slot].since = GetTickCount();
			XLS_LOG_INFO("mods: respawn: player %d -> %s at %.0f %.0f %.0f (%s).", slot, PointName(p), p.pos[0], p.pos[1], p.pos[2], why.c_str());
		}
		else {
			XLS_LOG_INFO("mods: respawn: player %d: no point of ours (%s), the game's own spawn.", slot, why.c_str());
		}
		timers[0] = 0.0f;
		timers[1] = 0.0f;
		*(int32_t*)(ctl + kCtlPick) = 0; // as state 2 does
		ctl[kCtlState] = kStatePick;     // the menu and the countdown skipped
		return;
	}
	if (!g_pending[slot].on) {
		return;
	}
	Pending& p = g_pending[slot];
	uint8_t* body = *(uint8_t**)(ctl + kCtlBody);
	if (GetTickCount() - p.since > 15000) {
		p.on = false;
		XLS_LOG_INFO("mods: respawn: player %d never spawned (state %u), point dropped.", slot, (unsigned)ctl[kCtlState]);
		return;
	}
	if (!body || (ctl[kCtlState] < kStateAlive && ctl[kCtlState] != 0)) {
		return;
	}
	CharacterTeleport(body, p.at.pos, p.at.yaw);
	p.on = false;
}

}
