#include "mods/lp2.h"
#include "mods/packages.h"

#include "core/log.h"

#include <windows.h>

#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <unordered_map>
#include <vector>

// Map packages' force volumes (map.json "force_volumes"): push players, VS, akrid, bullets and grenades inside boxes.
// Three modes: continuous (an acceleration while inside: wind, currents, lifts that ramp up), impulse (a velocity added
// once on entering, again only after leaving and coming back) and velocity (the velocity replaced on entering: grav
// lifts, man cannons).
//
// Every unit's update starts with uCharacter::vf55 (oneway.cpp wraps its two calls and hands each unit here), so we
// act at the start of its update and the game's own movement and collision then run on what we did:
//  - Characters have no velocity of their own we could set (they move by their motions; cJump only keeps a vertical
//    speed, mSpd), so each carries one here, cm/s: added to its position every frame. Off the ground it falls with the
//    map's gravity; on the ground it slows down quickly. While we carry a player in the air its cJump mSpd (DX9 +0x17b4,
//    DX11 +0x17c4, just before mGravity) is held at 0, so the game's own fall doesn't add to ours. A move down the game
//    pushes back up (the floor) is a landing: the vertical part ends there.
//  - Bullets, rockets: what we carry is added to their own flight (they keep their speed and direction; velocity mode
//    adds like impulse).
//  - Grenades (uShell01) are Havok bodies: their velocity is set through the physics manager (DX9 FUN_007f9860,
//    thiscall(manager, &vec4, body): the body's motion vtable +0x40, as uShell01::vf52 sets the throw), m/s; the
//    current velocity is taken from the last frames' move.
// Runs on every machine on its own copies (remote players are put back by their own machine, which pushes them too).

namespace xls::mods {

namespace {

struct Build {
	uint32_t setVelocity, manager; // the physics manager's set-velocity call, its global (RVAs)
	uint32_t body;                 // uShell01's Havok body
	uint32_t spd;                  // cJump mSpd in the character
};
const Build kBuilds[kBuildCount] = {
	{0x408670, 0x1452190, 0xb50, 0x17c4}, // LP2DX11.exe
	{0x3f9860, 0x14420f0, 0xb40, 0x17b4}, // LP2DX9.exe
	{0x408820, 0x1453190, 0xb50, 0x17c4}, // LP2DX11.exe 1.1
	{0x3f9920, 0x14420f0, 0xb40, 0x17b4}, // LP2DX9.exe 1.1
};
const uint8_t kSetVelocityStart[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x18, 0x56, 0x57, 0x8b, 0x7d, 0x0c, 0x8b, 0xf1};
const double kStale = 0.25;      // s without an update: a new unit (or a deleted one's address reused)
const double kMaxStep = 0.1;     // s, longest step integrated at once (hitches)
const float kEarth = 980.0f;     // cm/s^2
const float kGroundFriction = 10.0f; // 1/s: how fast what we carry dies away on the ground
const float kAirDrag = 0.15f;    // 1/s
const float kRest = 2.0f;        // cm/s: below this (on the ground) nothing is carried

struct Unit {
	double at = 0.0;
	float v[3] = {0, 0, 0};      // carried velocity, cm/s (characters, bullets)
	float placed[3] = {0, 0, 0}; // where we left it last frame
	bool air = false;            // carried off the ground (characters)
	float step = 0.0f;           // our last vertical move, cm (landing / ceiling tests)
	std::vector<bool> inside;    // per volume, last frame
};

uint8_t* g_base = nullptr;
const Build* g_build = nullptr;
typedef void(__thiscall* SetVelocityFn)(void*, const float*, void*);
SetVelocityFn g_setVelocity = nullptr;
std::vector<ForceVolume> g_volumes; // the map's (game thread)
float g_gravity = 1.0f;
std::unordered_map<uint8_t*, Unit> g_units;
std::vector<bool> g_logged;
double g_freq = 0.0;

double Now()
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / g_freq;
}

const char* KindName(int kind)
{
	return kind == kOnewayBullets ? "bullet" : kind == kOnewayGrenades ? "grenade" : kind == kOnewayVs ? "VS" :
		kind == kOnewayAkrid ? "akrid" : "player";
}

void SetGrenadeVelocity(uint8_t* shell, const float v[3])
{
	void* manager = *(void**)(g_base + g_build->manager);
	void* body = *(void**)(shell + g_build->body);
	if (!manager || !body) {
		return;
	}
	alignas(16) float m[4] = {v[0] * 0.01f, v[1] * 0.01f, v[2] * 0.01f, 0.0f}; // Havok: metres
	g_setVelocity(manager, m, body);
}

}

void ForceUnit(unsigned char* unit, int kind)
{
	if (g_volumes.empty() || !kind || !g_build) {
		return;
	}
	float* pos = (float*)(unit + 0x30);
	double now = Now();
	if (g_units.size() > 4096) {
		for (auto it = g_units.begin(); it != g_units.end();) {
			it = now - it->second.at > kStale ? g_units.erase(it) : std::next(it);
		}
	}
	Unit& u = g_units[unit];
	bool fresh = u.at > 0.0 && now - u.at <= kStale;
	if (!fresh && u.air) {
		XLS_LOG_INFO("mods: force: a carried %s lost its flight (no update for %.2f s) at %.0f %.0f %.0f.", KindName(kind), now - u.at,
			pos[0], pos[1], pos[2]);
	}
	if (!fresh) {
		u = Unit();
	}
	float dt = fresh ? (float)(now - u.at > kMaxStep ? kMaxStep : now - u.at) : 0.0f;
	u.inside.resize(g_volumes.size(), false);
	bool character = (kind & (kOnewayPlayers | kOnewayVs | kOnewayAkrid)) != 0;
	bool grenade = kind == kOnewayGrenades;
	// what the game did with it since we left it (its update's own move and collision)
	float moved[3] = {pos[0] - u.placed[0], pos[1] - u.placed[1], pos[2] - u.placed[2]};
	if (grenade && !fresh) {
		memcpy(u.placed, pos, 12);   // its velocity needs two frames
		u.at = now;
		return;
	}

	float add[3] = {0, 0, 0};
	bool set = false;
	float to[3] = {0, 0, 0};
	for (size_t i = 0; i < g_volumes.size(); i++) {
		const ForceVolume& v = g_volumes[i];
		bool in = (v.flags & kind) && ScriptVolumeOn(v.sid) && InBox(v.min, v.max, v.turn, pos);
		bool was = u.inside[i];
		u.inside[i] = in;
		if (!in) {
			continue;
		}
		if (!g_logged[i] && !was) {
			g_logged[i] = true;
			XLS_LOG_INFO("mods: force: a %s entered force volume %u at %.0f %.0f %.0f.", KindName(kind), (unsigned)i,
				pos[0], pos[1], pos[2]);
		}
		if (v.mode == kForceContinuous) {
			for (int k = 0; k < 3; k++) {
				add[k] += v.force[k] * dt;
			}
		}
		else if (!was) {
			if (v.mode == kForceVelocity && kind != kOnewayBullets) {
				set = true;
				memcpy(to, v.force, 12);
			}
			else {
				for (int k = 0; k < 3; k++) {
					add[k] += v.force[k];
				}
			}
		}
	}
	bool pushed = set || add[0] != 0.0f || add[1] != 0.0f || add[2] != 0.0f;

	if (grenade) {
		if (pushed && dt > 0.0f) {
			float vel[3];
			for (int k = 0; k < 3; k++) {
				vel[k] = set ? to[k] + add[k] : moved[k] / dt + add[k];
			}
			SetGrenadeVelocity(unit, vel);
		}
		memcpy(u.placed, pos, 12);
		u.at = now;
		return;
	}

	// the game's own small moves (air control, step offsets) aren't a floor or a ceiling: only a push back of most of our
	// own last vertical step is (a false landing mid-arc killed the throw and dropped players into gaps)
	if (character && u.air && u.v[1] < 0.0f && u.step < 0.0f && moved[1] > std::max(0.5f, -0.6f * u.step)) {
		u.air = false;   // our move down was pushed back up: landed
		XLS_LOG_INFO("mods: force: a %s landed at %.0f %.0f %.0f (step %.1f cm, pushed back %.1f, still %.0f cm/s across).", KindName(kind),
			pos[0], pos[1], pos[2], u.step, moved[1], sqrtf(u.v[0] * u.v[0] + u.v[2] * u.v[2]));
		u.v[1] = 0.0f;
	}
	if (character && u.air && u.v[1] > 0.0f && u.step > 0.0f && moved[1] < -std::max(0.5f, 0.6f * u.step)) {
		u.v[1] = 0.0f;   // a ceiling
		XLS_LOG_INFO("mods: force: a %s hit a ceiling at %.0f %.0f %.0f (step %.1f cm, pushed back %.1f).", KindName(kind), pos[0], pos[1],
			pos[2], u.step, moved[1]);
	}
	if (set) {
		memcpy(u.v, to, 12);
	}
	for (int k = 0; k < 3; k++) {
		u.v[k] += add[k];
	}
	if (character) {
		if (!u.air && u.v[1] > kEarth * g_gravity * dt) {
			u.air = true;   // pushed up harder than gravity pulls
		}
		if (u.air) {
			u.v[1] -= kEarth * g_gravity * dt;
			float drag = 1.0f - kAirDrag * dt;
			u.v[0] *= drag;
			u.v[2] *= drag;
		}
		else {
			float f = 1.0f - kGroundFriction * dt;
			f = f < 0.0f ? 0.0f : f;
			u.v[0] *= f;
			u.v[2] *= f;
			u.v[1] = 0.0f;
			if (fabsf(u.v[0]) < kRest && fabsf(u.v[2]) < kRest) {
				u.v[0] = u.v[2] = 0.0f;
			}
		}
	}
	if (u.v[0] != 0.0f || u.v[1] != 0.0f || u.v[2] != 0.0f) {
		float d[3] = {u.v[0] * dt, u.v[1] * dt, u.v[2] * dt};
		for (uint32_t at : {0x30u, 0xd0u}) {
			float* p = (float*)(unit + at);
			for (int k = 0; k < 3; k++) {
				p[k] += d[k];
			}
		}
		if (kind == kOnewayPlayers && u.air) {
			*(float*)(unit + g_build->spd) = 0.0f;   // the game's own fall would add to ours
			// cJump mFallTimer (mSpd + 0x18): uHuman::vf634 (DX11 0xc691e0) drops a player who has fallen this long over
			// a no-ground area into the abyss state: a carried arc over a gap isn't a fall
			*(float*)(unit + g_build->spd + 0x18) = 0.0f;
		}
	}
	u.step = u.v[1] * dt;
	memcpy(u.placed, pos, 12);
	u.at = now;
}

void ForceInstall(unsigned char* base)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	g_freq = (double)f.QuadPart;
	int b = GameBuild();
	if (b < 0 || b >= kBuildCount || memcmp(base + kBuilds[b].setVelocity, kSetVelocityStart, sizeof(kSetVelocityStart)) != 0) {
		XLS_LOG_WARN("mods: force: physics call not recognised, force volumes ignored.");
		return;
	}
	g_base = base;
	g_build = &kBuilds[b];
	g_setVelocity = (SetVelocityFn)(base + g_build->setVelocity);
	XLS_LOG_INFO("mods: force: installed (%s).", BuildName(b));
}

void ForceSetMap(const MapPhysics* physics)
{
	g_volumes = physics ? physics->force : std::vector<ForceVolume>();
	g_gravity = physics ? physics->gravity : 1.0f;
	g_logged.assign(g_volumes.size(), false);
	g_units.clear();
	if (!g_volumes.empty()) {
		XLS_LOG_INFO("mods: force: %u volume(s).", (unsigned)g_volumes.size());
	}
}

}
