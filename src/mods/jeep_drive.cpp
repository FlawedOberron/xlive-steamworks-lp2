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

#include "core/config.h"
#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
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

jeep::Params g_params;
jeep::State g_state;
unsigned char* g_vs = nullptr;  // the VS being driven
bool g_quatMode = false;        // +0x40 holds a rotation quaternion (checked on entry): write it; else SetAngle (yaw)
double g_last = 0.0, g_logAt = 0.0;
double g_hitAt[16];
float g_prevPos[16][3];
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

void Enter(unsigned char* vs)
{
	g_vs = vs;
	const float* pos = (const float*)(vs + 0x30);
	float q[4];
	memcpy(q, vs + 0x40, 16);
	g_quatMode = fabsf(Len(q, 4) - 1.0f) < 0.02f;
	float yaw = 0.0f;
	if (g_quatMode) {
		yaw = jeep::Yaw(jeep::Quat{q[0], q[1], q[2], q[3]});
	}
	else {
		float eye[3], target[3], up[3], fov;
		if (RulesCamera(eye, target, up, &fov)) {
			yaw = atan2f(target[0] - eye[0], target[2] - eye[2]);
		}
	}
	g_params = jeep::Params();
	jeep::Init(&g_params, &g_state, {pos[0], pos[1] + kFoot + 20.0f, pos[2]}, yaw);
	g_last = Now();
	XLS_LOG_INFO("mods: jeep: driving the VS at %.0f %.0f %.0f (+0x40 %.3f %.3f %.3f %.3f, %s; yaw %.0f deg; type byte +0x1f %u).", pos[0],
		pos[1], pos[2], q[0], q[1], q[2], q[3], g_quatMode ? "a quaternion: written each frame" : "not a quaternion: SetAngle, yaw only",
		yaw * 57.2958f, vs[0x1f]);
}

void RunOvers(int self)
{
	double now = Now();
	for (int slot = 0; slot < 16; slot++) {
		unsigned char* ch = slot == self ? nullptr : RulesBody(slot);
		if (!ch || (uintptr_t)ch < 0x10000 || ch[0x1f] != 1) {
			g_prevPos[slot][0] = NAN;
			continue;
		}
		const float* p = (const float*)(ch + 0x30);
		jeep::Unit u;
		u.pos = {p[0], p[1], p[2]};
		float dt = (float)(now - g_last);
		if (g_prevPos[slot][0] == g_prevPos[slot][0] && dt > 0.0f) {
			u.vel = {(p[0] - g_prevPos[slot][0]) / dt, (p[1] - g_prevPos[slot][1]) / dt, (p[2] - g_prevPos[slot][2]) / dt};
		}
		memcpy(g_prevPos[slot], p, 12);
		CharacterVitals v;
		if (!CharacterStats(ch, &v) || v.life <= 0.0f || now - g_hitAt[slot] < kRunOverCooldown) {
			continue;
		}
		Vec3 d = Sub(u.pos, g_state.pos);
		if (Dot(d, d) > 600.0f * 600.0f) {
			continue;
		}
		jeep::RunOverResult r = jeep::RunOver(g_params, &g_state, u);
		if (r.impact == jeep::Impact::None) {
			continue;
		}
		g_hitAt[slot] = now;
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
	g_vs = nullptr;
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
	if (onlineMap != g_map) {
		g_map = onlineMap;
		wchar_t name[64];
		swprintf(name, 64, L"Mods\\jeep\\online%02d.col", onlineMap);
		g_haveGround = Load(ExeDirectory() + L"\\" + name);
		if (!g_haveGround) {
			XLS_LOG_INFO("mods: jeep: ModCache\\jeep.txt is on, but online map %d has no %ls: no jeep here.", onlineMap, name);
		}
	}
	for (int i = 0; i < 16; i++) {
		g_hitAt[i] = 0.0;
		g_prevPos[i][0] = NAN;
	}
}

void JeepFrame()
{
	if (!g_on || !g_haveGround) {
		return;
	}
	int self = RulesLocalSlot();
	unsigned char* body = self >= 0 ? RulesBody(self) : nullptr;
	if (!body || (uintptr_t)body < 0x10000 || body[0x1f] != 2) {
		if (g_vs) {
			XLS_LOG_INFO("mods: jeep: left the VS.");
			g_vs = nullptr;
		}
		return;
	}
	if (body != g_vs) {
		Enter(body);
		return;
	}
	double now = Now();
	float dt = (float)std::min(0.1, now - g_last);
	if (dt <= 0.0f) {
		return;
	}
	jeep::Input in;
	float camYaw = jeep::Yaw(g_state.rot);
	float eye[3], target[3], up[3], fov;
	if (RulesCamera(eye, target, up, &fov)) {
		camYaw = atan2f(target[0] - eye[0], target[2] - eye[2]);
	}
	in.lookYaw = jeep::Yaw(g_state.rot); // no keys: coast straight
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
	jeep::Step(g_params, &g_state, in, dt, Ray, nullptr);
	RunOvers(self);
	g_last = now;

	Vec3 foot = jeep::ToWorld(g_state, {0, -kFoot + g_lift, 0});
	float pos[3] = {foot.x, foot.y, foot.z};
	if (g_quatMode) {
		CharacterPlace(g_vs, pos, nullptr);
		jeep::Quat r = g_state.rot;
		if (g_turn != 0.0f) { // r * (turn about Y)
			float s = sinf(g_turn / 2), c = cosf(g_turn / 2);
			r = {r.x * c - r.z * s, r.y * c + r.w * s, r.z * c + r.x * s, r.w * c - r.y * s};
		}
		float q[4] = {r.x, r.y, r.z, r.w};
		memcpy(g_vs + 0x40, q, 16);
	}
	else {
		float euler[3] = {0.0f, jeep::Yaw(g_state.rot) + g_turn, 0.0f};
		CharacterPlace(g_vs, pos, euler);
	}
	if (now >= g_logAt) {
		g_logAt = now + 2.0;
		Vec3 u = jeep::Rotate(g_state.rot, {0, 1, 0});
		XLS_LOG_INFO("mods: jeep: at %.0f %.0f %.0f, %.0f cm/s, %d wheel(s) down%s, up %.2f, yaw %.0f (look %.0f), throttle %.0f.", pos[0],
			pos[1], pos[2], g_state.speed, g_state.groundedWheels, g_state.airborne ? ", airborne" : "", u.y,
			jeep::Yaw(g_state.rot) * 57.2958f, in.lookYaw * 57.2958f, in.throttle);
	}
}

}
