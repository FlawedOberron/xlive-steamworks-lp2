// LP2 mods: the jeep test in game (notes/jeep-plan.md). On while ModCache\jeep.txt exists: on an online map whose
// ground file <game>\Mods\jeep\online<NN>.col exists (lp2-tools lp2jeepcol.py from the map's move.sbc1), any VS the
// local player rides is driven by the jeep model (src/mods/jeep) instead of its own movement: the pose is written
// onto the VS every frame from the HUD update.
//   WASD: drive towards that direction relative to the camera (S alone: brake / reverse), Space handbrake, R flip,
//   arrow keys air control.
// Soldiers it hits are pushed, knocked (thrown, hurt) or killed by closing speed (jeep RunOver), on this PC only.
// A test: no seats, no model, no network sync (other players see the VS where their copy puts it).
#include "mods/jeep/jeep.h"
#include "mods/lp2.h"
#include "mods/packages.h"

#include "core/config.h"
#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace xls::mods {

namespace {

using jeep::Vec3;

struct Tri {
	Vec3 a, e1, e2; // corner, edges
};

// the map's triangles in an XZ grid of cells
struct Ground {
	std::vector<Tri> tris;
	float minX = 0, minZ = 0;
	int nx = 0, nz = 0;
	std::vector<uint32_t> start;  // per cell, into items (nx * nz + 1)
	std::vector<uint32_t> items;
	std::vector<uint32_t> stamp;  // per triangle, the last query that tested it
	uint32_t query = 0;
};

const float kCell = 400.0f;      // cm
const float kFoot = 105.0f;      // body origin above the VS origin (the jeep's ride height at rest)
const double kRunOverCooldown = 0.5;

Ground g_ground;
bool g_haveGround = false;
int g_map = -1;
bool g_on = false;              // jeep.txt, read at map load
float g_lift = 0.0f;            // jeep.txt "height=<cm>": raises the VS above the ground point (its model's origin height)
float g_turn = 0.0f;            // jeep.txt "yaw=<degrees>": the model's facing against the jeep's (+Z forward)

jeep::Params g_params;          // shared (Init fills the derived values)

// every VS on the map is a jeep: its own state, stepped from its update (after the game's own move, which we replace)
struct Car {
	jeep::State state;
	bool quatMode = false;      // +0x40 holds a rotation quaternion (checked at first sight): written; else SetAngle (yaw)
	double last = 0.0, seen = 0.0;
	double hitAt[16] = {};
	float prevPos[16][3];
};
std::unordered_map<unsigned char*, Car> g_cars;
unsigned char* g_driven = nullptr; // the VS the local player rides (HUD frame), gets the keys
jeep::Input g_input;            // the local driver's input, this frame
const char* g_how = "";         // how the ride was found (logged)
double g_logAt = 0.0, g_probeAt = 0.0;
double g_freq = 0.0;

double Now()
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	if (!g_freq) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_freq = (double)f.QuadPart;
	}
	return (double)c.QuadPart / g_freq;
}

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

bool Load(const std::wstring& path)
{
	g_ground = Ground();
	FILE* f = _wfopen(path.c_str(), L"rb");
	if (!f) {
		return false;
	}
	char magic[4];
	uint32_t version = 0, count = 0;
	bool ok = fread(magic, 4, 1, f) == 1 && memcmp(magic, "JCOL", 4) == 0 && fread(&version, 4, 1, f) == 1 && version == 1 &&
	          fread(&count, 4, 1, f) == 1 && count > 0 && count < 4000000;
	std::vector<float> raw;
	if (ok) {
		raw.resize((size_t)count * 9);
		ok = fread(raw.data(), 36, count, f) == count;
	}
	fclose(f);
	if (!ok) {
		XLS_LOG_INFO("mods: jeep: %ls isn't a jeep ground file.", path.c_str());
		return false;
	}
	float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
	for (uint32_t i = 0; i < count; i++) {
		const float* t = &raw[i * 9];
		for (int k = 0; k < 3; k++) {
			minX = std::min(minX, t[k * 3]);
			maxX = std::max(maxX, t[k * 3]);
			minZ = std::min(minZ, t[k * 3 + 2]);
			maxZ = std::max(maxZ, t[k * 3 + 2]);
		}
		Vec3 a{t[0], t[1], t[2]}, b{t[3], t[4], t[5]}, c{t[6], t[7], t[8]};
		g_ground.tris.push_back({a, Sub(b, a), Sub(c, a)});
	}
	Ground& g = g_ground;
	g.minX = minX;
	g.minZ = minZ;
	g.nx = (int)((maxX - minX) / kCell) + 1;
	g.nz = (int)((maxZ - minZ) / kCell) + 1;
	std::vector<std::vector<uint32_t>> cells((size_t)g.nx * g.nz);
	for (uint32_t i = 0; i < count; i++) {
		const float* t = &raw[i * 9];
		float x0 = std::min({t[0], t[3], t[6]}), x1 = std::max({t[0], t[3], t[6]});
		float z0 = std::min({t[2], t[5], t[8]}), z1 = std::max({t[2], t[5], t[8]});
		int cx0 = (int)((x0 - minX) / kCell), cx1 = (int)((x1 - minX) / kCell);
		int cz0 = (int)((z0 - minZ) / kCell), cz1 = (int)((z1 - minZ) / kCell);
		for (int z = cz0; z <= cz1; z++) {
			for (int x = cx0; x <= cx1; x++) {
				cells[(size_t)z * g.nx + x].push_back(i);
			}
		}
	}
	g.start.push_back(0);
	for (auto& c : cells) {
		g.items.insert(g.items.end(), c.begin(), c.end());
		g.start.push_back((uint32_t)g.items.size());
	}
	g.stamp.assign(count, 0);
	XLS_LOG_INFO("mods: jeep: ground %ls: %u triangles, %dx%d cells.", path.c_str(), count, g.nx, g.nz);
	return true;
}

jeep::RayHit Ray(void*, const Vec3& from, const Vec3& to)
{
	jeep::RayHit best;
	Ground& g = g_ground;
	if (g.tris.empty()) {
		return best;
	}
	Vec3 d = Sub(to, from);
	int cx0 = (int)((std::min(from.x, to.x) - g.minX) / kCell), cx1 = (int)((std::max(from.x, to.x) - g.minX) / kCell);
	int cz0 = (int)((std::min(from.z, to.z) - g.minZ) / kCell), cz1 = (int)((std::max(from.z, to.z) - g.minZ) / kCell);
	cx0 = std::max(cx0, 0);
	cz0 = std::max(cz0, 0);
	cx1 = std::min(cx1, g.nx - 1);
	cz1 = std::min(cz1, g.nz - 1);
	if (++g.query == 0) {
		std::fill(g.stamp.begin(), g.stamp.end(), 0);
		g.query = 1;
	}
	for (int z = cz0; z <= cz1; z++) {
		for (int x = cx0; x <= cx1; x++) {
			size_t c = (size_t)z * g.nx + x;
			for (uint32_t k = g.start[c]; k < g.start[c + 1]; k++) {
				uint32_t i = g.items[k];
				if (g.stamp[i] == g.query) {
					continue;
				}
				g.stamp[i] = g.query;
				const Tri& t = g.tris[i];
				// Moller-Trumbore, both sides
				Vec3 p = Cross(d, t.e2);
				float det = Dot(t.e1, p);
				if (fabsf(det) < 1e-6f) {
					continue;
				}
				float inv = 1.0f / det;
				Vec3 s = Sub(from, t.a);
				float u = Dot(s, p) * inv;
				if (u < 0.0f || u > 1.0f) {
					continue;
				}
				Vec3 q = Cross(s, t.e1);
				float v = Dot(d, q) * inv;
				if (v < 0.0f || u + v > 1.0f) {
					continue;
				}
				float f = Dot(t.e2, q) * inv;
				if (f < 0.0f || f > best.fraction || (best.hit && f == best.fraction)) {
					continue;
				}
				Vec3 n = Cross(t.e1, t.e2);
				float l = sqrtf(Dot(n, n));
				if (l <= 0.0f) {
					continue;
				}
				n = {n.x / l, n.y / l, n.z / l};
				if (Dot(n, d) > 0.0f) {
					n = {-n.x, -n.y, -n.z}; // facing the ray
				}
				best.hit = true;
				best.fraction = f;
				best.pos = {from.x + d.x * f, from.y + d.y * f, from.z + d.z * f};
				best.normal = n;
			}
		}
	}
	return best;
}

bool Key(int vk)
{
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool GameFocused()
{
	HWND w = GetForegroundWindow();
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	return w && pid == GetCurrentProcessId();
}

float Len(const float* q, int n)
{
	float s = 0;
	for (int i = 0; i < n; i++) {
		s += q[i] * q[i];
	}
	return sqrtf(s);
}


template <typename T>
bool Read(const void* at, T* out)
{
	SIZE_T got = 0;
	return (uintptr_t)at >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), at, out, sizeof(T), &got) && got == sizeof(T);
}

bool IsVs(unsigned char* p)
{
	uint8_t type = 0;
	return Read(p + 0x1f, &type) && type == 2;
}

float Dist2(const unsigned char* a, const unsigned char* b)
{
	const float* p = (const float*)(a + 0x30);
	const float* q = (const float*)(b + 0x30);
	return (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
}

// The VS the local player rides, or null: the body itself (if the game swaps it), the body the camera follows, or a
// pointer to one of the map's VS in the human / the slot's entry while the human is on it (within 3 m).
unsigned char* FindRide(int self)
{
	unsigned char* ctl = RulesController(self);
	unsigned char* body = RulesBody(self);
	if (!ctl || (uintptr_t)body < 0x10000) {
		return nullptr;
	}
	if (IsVs(body)) {
		g_how = "the player's body";
		return body;
	}
	unsigned char *cam = nullptr, *watched = nullptr;
	if (Read(ctl + 0x484, &cam) && Read(cam + 0x200, &watched) && watched != body && IsVs(watched)) {
		g_how = "the camera's body";
		return watched;
	}
	for (auto& [vs, car] : g_cars) {
		if (Dist2(vs, body) > 300.0f * 300.0f) {
			continue;
		}
		for (unsigned char* obj : {body, ctl}) {
			uint32_t span = obj == body ? 0x2400 : 0x600;
			for (uint32_t off = 0; off < span; off += 4) {
				unsigned char* p = nullptr;
				if (Read(obj + off, &p) && p == vs) {
					static char how[64];
					snprintf(how, sizeof(how), "%s +0x%x", obj == body ? "the human" : "the slot's entry", off);
					g_how = how;
					return vs;
				}
			}
		}
	}
	return nullptr;
}

void Pose(unsigned char* vs, Car& car)
{
	Vec3 foot = jeep::ToWorld(car.state, {0, -kFoot + g_lift, 0});
	float pos[3] = {foot.x, foot.y, foot.z};
	if (car.quatMode) {
		CharacterPlace(vs, pos, nullptr);
		jeep::Quat r = car.state.rot;
		if (g_turn != 0.0f) { // r * (turn about Y)
			float s = sinf(g_turn / 2), c = cosf(g_turn / 2);
			r = {r.x * c - r.z * s, r.y * c + r.w * s, r.z * c + r.x * s, r.w * c - r.y * s};
		}
		float q[4] = {r.x, r.y, r.z, r.w};
		memcpy(vs + 0x40, q, 16);
	}
	else {
		float euler[3] = {0.0f, jeep::Yaw(car.state.rot) + g_turn, 0.0f};
		CharacterPlace(vs, pos, euler);
	}
}

void RunOvers(unsigned char* vs, Car& car, float dt)
{
	double now = Now();
	for (int slot = 0; slot < 16; slot++) {
		unsigned char* ch = RulesBody(slot);
		if (!ch || (uintptr_t)ch < 0x10000 || ch[0x1f] != 1 || (vs == g_driven && slot == RulesLocalSlot())) {
			car.prevPos[slot][0] = NAN;
			continue;
		}
		const float* p = (const float*)(ch + 0x30);
		jeep::Unit u;
		u.pos = {p[0], p[1], p[2]};
		if (car.prevPos[slot][0] == car.prevPos[slot][0] && dt > 0.0f) {
			u.vel = {(p[0] - car.prevPos[slot][0]) / dt, (p[1] - car.prevPos[slot][1]) / dt, (p[2] - car.prevPos[slot][2]) / dt};
		}
		memcpy(car.prevPos[slot], p, 12);
		Vec3 d = Sub(u.pos, car.state.pos);
		if (Dot(d, d) > 600.0f * 600.0f || now - car.hitAt[slot] < kRunOverCooldown) {
			continue;
		}
		if (Dot(d, d) < 150.0f * 150.0f && fabsf(car.state.speed) < 300.0f) {
			continue; // a rider (or someone getting in)
		}
		CharacterVitals v;
		if (!CharacterStats(ch, &v) || v.life <= 0.0f) {
			continue;
		}
		jeep::RunOverResult r = jeep::RunOver(g_params, &car.state, u);
		if (r.impact == jeep::Impact::None) {
			continue;
		}
		car.hitAt[slot] = now;
		float vel[3] = {r.unitVel.x, r.unitVel.y, r.unitVel.z};
		ForceThrow(ch, vel);
		float damage = 0.0f;
		if (r.impact == jeep::Impact::Knock) {
			damage = r.damage * v.lifeMax;
		}
		else if (r.impact == jeep::Impact::Kill) {
			damage = std::max(v.life, v.lifeMax) * 1.5f;
		}
		if (damage > 0.0f) {
			CharacterDamage(ch, damage);
		}
		const char* names[] = {"none", "pushed", "knocked down", "killed"};
		XLS_LOG_INFO("mods: jeep: slot %d %s: closing %.0f cm/s, damage %.0f of %.0f, thrown (%.0f, %.0f, %.0f).", slot, names[(int)r.impact],
			r.closing, damage, v.life, vel[0], vel[1], vel[2]);
	}
}

}

void JeepSetMap(int onlineMap)
{
	g_cars.clear();
	g_driven = nullptr;
	g_on = GetFileAttributesW((Cfg().modCacheDir + L"jeep.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
	g_lift = 0.0f;
	g_turn = 0.0f;
	if (FILE* f = _wfopen((Cfg().modCacheDir + L"jeep.txt").c_str(), L"r")) {
		char line[128];
		while (fgets(line, sizeof(line), f)) {
			float v;
			if (sscanf(line, " height = %f", &v) == 1) {
				g_lift = v;
			}
			else if (sscanf(line, " yaw = %f", &v) == 1) {
				g_turn = v * 3.14159265f / 180.0f;
			}
		}
		fclose(f);
		if (g_on && (g_lift != 0.0f || g_turn != 0.0f)) {
			XLS_LOG_INFO("mods: jeep: jeep.txt: height %.0f cm, yaw %.0f deg.", g_lift, g_turn * 57.2958f);
		}
	}
	if (!g_on || onlineMap < 0) {
		g_map = -1;
		g_haveGround = false;
		return;
	}
	if (onlineMap != g_map || onlineMap == 13 || onlineMap == 14) {
		g_map = onlineMap;
		wchar_t name[64];
		swprintf(name, 64, L"Mods\\jeep\\online%02d.col", onlineMap);
		// a map package carries its own (nativePC\jeep\aArea_ONLINE_NN.col, renumbered to its slot like its other files)
		wchar_t rel[64];
		swprintf(rel, 64, L"nativepc\\jeep\\aarea_online_%02d.col", onlineMap);
		std::wstring packaged;
		if ((onlineMap == 13 || onlineMap == 14) && PackagesSlotBound(onlineMap) && PackagesMapFile(rel, &packaged)) {
			g_haveGround = Load(packaged);
		}
		else {
			g_haveGround = Load(ExeDirectory() + L"\\" + name);
		}
		if (!g_haveGround) {
			XLS_LOG_INFO("mods: jeep: ModCache\\jeep.txt is on, but online map %d has no %ls: no jeep here.", onlineMap, name);
		}
	}
	g_params = jeep::Params();
	jeep::State scratch;
	jeep::Init(&g_params, &scratch, {}, 0.0f); // derived spring / drive values
}

// Every VS's update (oneway.cpp's wrapper, after the game's own): the jeep model moves it instead.
void JeepUnit(unsigned char* vs)
{
	if (!g_on || !g_haveGround || vs[0x1f] != 2) {
		return;
	}
	double now = Now();
	auto it = g_cars.find(vs);
	if (it == g_cars.end() || now - it->second.seen > 1.0) { // new (or its address reused)
		if (g_cars.size() > 256) {
			for (auto c = g_cars.begin(); c != g_cars.end();) {
				c = now - c->second.seen > 1.0 ? g_cars.erase(c) : std::next(c);
			}
		}
		Car& car = g_cars[vs];
		car = Car();
		for (auto& p : car.prevPos) {
			p[0] = NAN;
		}
		const float* pos = (const float*)(vs + 0x30);
		float q[4];
		memcpy(q, vs + 0x40, 16);
		car.quatMode = fabsf(Len(q, 4) - 1.0f) < 0.02f;
		float yaw = car.quatMode ? jeep::Yaw(jeep::Quat{q[0], q[1], q[2], q[3]}) : 0.0f;
		jeep::Init(&g_params, &car.state, {pos[0], pos[1] + kFoot + 20.0f - g_lift, pos[2]}, yaw);
		car.last = car.seen = now;
		XLS_LOG_INFO("mods: jeep: a jeep at %.0f %.0f %.0f (+0x40 %.3f %.3f %.3f %.3f: %s, yaw %.0f); %u on the map.", pos[0], pos[1], pos[2],
			q[0], q[1], q[2], q[3], car.quatMode ? "a quaternion" : "not a quaternion: SetAngle, yaw only", yaw * 57.2958f,
			(unsigned)g_cars.size());
		return;
	}
	Car& car = it->second;
	car.seen = now;
	float dt = (float)std::min(0.1, now - car.last);
	if (dt <= 0.0f) {
		return;
	}
	car.last = now;
	jeep::Input in;
	if (vs == g_driven) {
		in = g_input;
	}
	else {
		in.lookYaw = jeep::Yaw(car.state.rot); // nobody driving: rolls, settles, brakes gently
		in.throttle = fabsf(car.state.speed) > 50.0f && car.state.speed > 0 ? -0.15f : 0.0f;
	}
	jeep::Step(g_params, &car.state, in, dt, Ray, nullptr);
	if (fabsf(car.state.speed) > g_params.runOverMin * 0.5f) {
		RunOvers(vs, car, dt);
	}
	Pose(vs, car);
	if (vs == g_driven && now >= g_logAt) {
		g_logAt = now + 2.0;
		Vec3 u = jeep::Rotate(car.state.rot, {0, 1, 0});
		XLS_LOG_INFO("mods: jeep: driving: at %.0f %.0f %.0f, %.0f cm/s, %d wheel(s) down%s, up %.2f, yaw %.0f (want %.0f), throttle %.0f.",
			car.state.pos.x, car.state.pos.y, car.state.pos.z, car.state.speed, car.state.groundedWheels, car.state.airborne ? ", airborne" : "", u.y,
			jeep::Yaw(car.state.rot) * 57.2958f, in.lookYaw * 57.2958f, in.throttle);
	}
}

// The HUD update: which jeep the local player drives, and their keys.
void JeepFrame()
{
	if (!g_on || !g_haveGround) {
		return;
	}
	int self = RulesLocalSlot();
	unsigned char* ride = self >= 0 ? FindRide(self) : nullptr;
	if (ride && g_cars.find(ride) == g_cars.end()) {
		ride = nullptr; // not updated through our hook yet
	}
	if (ride != g_driven) {
		if (ride) {
			XLS_LOG_INFO("mods: jeep: driving the jeep at %.0f %.0f %.0f (found as %s).", ((float*)(ride + 0x30))[0], ((float*)(ride + 0x30))[1],
				((float*)(ride + 0x30))[2], g_how);
		}
		else {
			XLS_LOG_INFO("mods: jeep: out of the jeep.");
		}
		g_driven = ride;
	}
	double now = Now();
	if (!ride && now >= g_probeAt && self >= 0) {
		// not riding as far as we can tell: say what we see when a jeep is close (finding the ride pointer)
		g_probeAt = now + 3.0;
		unsigned char* body = RulesBody(self);
		for (auto& [vs, car] : g_cars) {
			if ((uintptr_t)body >= 0x10000 && Dist2(vs, body) < 400.0f * 400.0f) {
				unsigned char *ctl = RulesController(self), *cam = nullptr, *watched = nullptr;
				Read(ctl + 0x484, &cam);
				Read(cam + 0x200, &watched);
				XLS_LOG_INFO("mods: jeep: probe: a jeep %.0f cm from the local player (slot %d, body type %u, camera on %s), not seen as riding it.",
					sqrtf(Dist2(vs, body)), self, body[0x1f], watched == body ? "the body" : watched == vs ? "the jeep" : "something else");
				break;
			}
		}
	}
	if (!ride) {
		return;
	}
	Car& car = g_cars[ride];
	jeep::Input in;
	float camYaw = jeep::Yaw(car.state.rot);
	float eye[3], target[3], up[3], fov;
	if (RulesCamera(eye, target, up, &fov)) {
		camYaw = atan2f(target[0] - eye[0], target[2] - eye[2]);
	}
	in.lookYaw = jeep::Yaw(car.state.rot); // no keys: coast straight
	if (GameFocused()) {
		// WASD is a direction relative to the camera: the jeep turns towards it and drives (D: 90 degrees right of the
		// camera, W+D: 45). S alone brakes / reverses; S with A / D reverses steering that way
		float x = (Key('D') ? 1.0f : 0.0f) - (Key('A') ? 1.0f : 0.0f);
		float z = (Key('W') ? 1.0f : 0.0f) - (Key('S') ? 1.0f : 0.0f);
		if (z < 0.0f) {
			in.throttle = -1.0f;
			in.lookSteer = false;
			in.steer = x;
		}
		else if (x != 0.0f || z > 0.0f) {
			in.throttle = 1.0f;
			in.lookYaw = camYaw + atan2f(x, z);
		}
		in.handbrake = Key(VK_SPACE);
		in.flip = Key('R');
		in.airPitch = (Key(VK_UP) ? 1.0f : 0.0f) - (Key(VK_DOWN) ? 1.0f : 0.0f);
		in.airRoll = (Key(VK_RIGHT) ? 1.0f : 0.0f) - (Key(VK_LEFT) ? 1.0f : 0.0f);
	}
	g_input = in;
}

}
