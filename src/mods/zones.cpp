// Bot zone outlines on the overlay: the map's zones (map.json "nav_zones": name, tags, outline loops from lp2navmap)
// shown as fences over their borders, the outline extruded up. The map / mode script's "zoneoutline" node picks them
// by name or tag (ZonesShow, on the host); scripts.cpp sends the shown set to everyone with its state message.
#include "mods/lp2.h"
#include "mods/packages.h"

#include "core/log.h"

#include <windows.h>

#include "third-party/imgui/imgui.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace xls::mods {

namespace {

struct Zone {
	std::string name;   // lower case
	std::vector<std::string> tags;
	std::vector<std::vector<std::array<float, 3>>> outline;
};
struct Shown {
	bool on = false;
	uint32_t rgb = 0xffcc33;
	float height = 300.0f;
	double until = 0.0;   // Now() it hides at, 0 never
};

std::mutex g_lock;
std::vector<Zone> g_zones;
std::vector<Shown> g_shown;
double g_loggedCamera = -100.0;

double Now()
{
	return (double)GetTickCount64() / 1000.0;
}

bool Wild(const char* name, const char* pattern)
{
	if (*pattern == '*') {
		return Wild(name, pattern + 1) || (*name && Wild(name + 1, pattern));
	}
	if (!*pattern) {
		return !*name;
	}
	return *name && (*pattern == '?' || *pattern == *name) && Wild(name + 1, pattern + 1);
}

struct V3 {
	float x, y, z;
};
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Norm(V3 a)
{
	float l = sqrtf(Dot(a, a));
	return l > 1e-6f ? V3{a.x / l, a.y / l, a.z / l} : V3{0.0f, 0.0f, 0.0f};
}

struct View {
	V3 eye, r, u, f;
	float scale, cx, cy;
	V3 To(V3 p) const   // world -> view (x right, y up, z forward)
	{
		V3 d = Sub(p, eye);
		return {Dot(d, r), Dot(d, u), Dot(d, f)};
	}
	ImVec2 Screen(V3 v) const
	{
		return ImVec2(cx + v.x * scale / v.z, cy - v.y * scale / v.z);
	}
};

const float kNear = 10.0f;   // cm in front of the eye

struct Pt {
	V3 v;
	float t;   // 0 the floor, 1 the top
};

// one wall piece (a convex quad in view space) clipped to z >= kNear (Sutherland-Hodgman against one plane)
int ClipNear(const Pt* in, int n, Pt* out)
{
	int m = 0;
	for (int i = 0; i < n; i++) {
		const Pt& a = in[i];
		const Pt& b = in[(i + 1) % n];
		bool ia = a.v.z >= kNear, ib = b.v.z >= kNear;
		if (ia) {
			out[m++] = a;
		}
		if (ia != ib) {
			float s = (kNear - a.v.z) / (b.v.z - a.v.z);
			out[m++] = {{a.v.x + (b.v.x - a.v.x) * s, a.v.y + (b.v.y - a.v.y) * s, kNear}, a.t + (b.t - a.t) * s};
		}
	}
	return m;
}

ImU32 Colour(uint32_t rgb, float alpha)
{
	int a = (int)(alpha * 255.0f);
	a = a < 0 ? 0 : a > 255 ? 255 : a;
	return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, a);
}

void Wall(ImDrawList* dl, const View& view, V3 a, V3 b, float height, uint32_t rgb)
{
	Pt quad[4] = {{view.To(a), 0.0f}, {view.To(b), 0.0f}, {view.To({b.x, b.y + height, b.z}), 1.0f},
		{view.To({a.x, a.y + height, a.z}), 1.0f}};
	Pt clipped[8];
	int n = ClipNear(quad, 4, clipped);
	if (n < 3) {
		return;
	}
	ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
	dl->PrimReserve((n - 2) * 3, n);
	ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
	for (int i = 0; i < n; i++) {
		// strongest at the floor, fading up
		dl->PrimWriteVtx(view.Screen(clipped[i].v), uv, Colour(rgb, 0.55f * (1.0f - clipped[i].t) + 0.04f));
	}
	for (int i = 1; i + 1 < n; i++) {
		dl->PrimWriteIdx(base);
		dl->PrimWriteIdx((ImDrawIdx)(base + i));
		dl->PrimWriteIdx((ImDrawIdx)(base + i + 1));
	}
	// the floor edge as a bright line
	Pt edge[2] = {quad[0], quad[1]};
	if (edge[0].v.z < kNear && edge[1].v.z < kNear) {
		return;
	}
	for (int k = 0; k < 2; k++) {
		Pt& p = edge[k];
		const Pt& o = edge[1 - k];
		if (p.v.z < kNear) {
			float s = (kNear - p.v.z) / (o.v.z - p.v.z);
			p.v = {p.v.x + (o.v.x - p.v.x) * s, p.v.y + (o.v.y - p.v.y) * s, kNear};
		}
	}
	dl->AddLine(view.Screen(edge[0].v), view.Screen(edge[1].v), Colour(rgb, 0.95f), 2.5f);
}

void Draw(float width, float height)
{
	std::lock_guard<std::mutex> lock(g_lock);
	double now = Now();
	bool any = false;
	for (Shown& s : g_shown) {
		if (s.on && s.until > 0.0 && now >= s.until) {
			s.on = false;
		}
		any = any || s.on;
	}
	if (!any) {
		return;
	}
	float eye[3], target[3], up[3], fov = 0.0f;
	if (!RulesCamera(eye, target, up, &fov)) {
		return;
	}
	// the game holds degrees (MT: mFov); radians if it is small
	float fovRad = fov > 3.2f ? fov * 3.14159265f / 180.0f : fov;
	if (!(fovRad > 0.05f && fovRad < 3.0f)) {
		fovRad = 60.0f * 3.14159265f / 180.0f;
	}
	if (now - g_loggedCamera > 30.0) {
		g_loggedCamera = now;
		XLS_LOG_INFO("mods: zones: camera eye %.0f %.0f %.0f target %.0f %.0f %.0f up %.2f %.2f %.2f fov %g.", eye[0], eye[1],
			eye[2], target[0], target[1], target[2], up[0], up[1], up[2], fov);
	}
	View view;
	view.eye = {eye[0], eye[1], eye[2]};
	view.f = Norm(Sub({target[0], target[1], target[2]}, view.eye));
	view.r = Norm(Cross(view.f, {up[0], up[1], up[2]}));
	view.u = Cross(view.r, view.f);
	if (Dot(view.f, view.f) < 0.5f || Dot(view.r, view.r) < 0.5f) {
		return;
	}
	view.scale = height * 0.5f / tanf(fovRad * 0.5f);
	view.cx = width * 0.5f;
	view.cy = height * 0.5f;
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	for (size_t i = 0; i < g_shown.size() && i < g_zones.size(); i++) {
		const Shown& s = g_shown[i];
		if (!s.on) {
			continue;
		}
		for (const auto& loop : g_zones[i].outline) {
			for (size_t k = 0; k + 1 < loop.size(); k++) {
				V3 a = {loop[k][0], loop[k][1] + 5.0f, loop[k][2]};
				V3 b = {loop[k + 1][0], loop[k + 1][1] + 5.0f, loop[k + 1][2]};
				Wall(dl, view, a, b, s.height, s.rgb);
			}
		}
	}
}

bool g_installed = false;

}

void ZonesSetMap(const MapPhysics* physics)
{
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_zones.clear();
		if (physics) {
			for (const NavZone& z : physics->nav.zones) {
				Zone o;
				o.name = z.name;
				for (char& c : o.name) {
					c = (char)tolower((unsigned char)c);
				}
				o.tags = z.tags;
				o.outline = z.outline;
				g_zones.push_back(std::move(o));
			}
		}
		g_shown.assign(g_zones.size(), Shown());
	}
	if (!g_installed) {
		g_installed = true;
		UiAddLayer("zones", Draw);
	}
}

int ZonesShow(const char* pattern, bool show, uint32_t rgb, float height, float seconds)
{
	std::string p = pattern ? pattern : "";
	for (char& c : p) {
		c = (char)tolower((unsigned char)c);
	}
	std::lock_guard<std::mutex> lock(g_lock);
	int count = 0;
	for (size_t i = 0; i < g_zones.size(); i++) {
		bool match = Wild(g_zones[i].name.c_str(), p.c_str());
		for (const std::string& t : g_zones[i].tags) {
			match = match || Wild(t.c_str(), p.c_str());
		}
		if (!match) {
			continue;
		}
		Shown& s = g_shown[i];
		s.on = show;
		if (show) {
			s.rgb = rgb & 0xffffff;
			s.height = height > 0.0f ? height : 300.0f;
			s.until = seconds > 0.0f ? Now() + seconds : 0.0;
		}
		count++;
	}
	XLS_LOG_INFO("mods: zones: %s \"%s\": %d zone(s).", show ? "show" : "hide", p.c_str(), count);
	return count;
}

void ZonesPack(std::vector<uint8_t>* m)
{
	std::lock_guard<std::mutex> lock(g_lock);
	double now = Now();
	std::vector<uint8_t> body;
	uint16_t n = 0;
	auto put = [&](const void* v, size_t size) {
		body.insert(body.end(), (const uint8_t*)v, (const uint8_t*)v + size);
	};
	for (size_t i = 0; i < g_shown.size(); i++) {
		const Shown& s = g_shown[i];
		if (!s.on || (s.until > 0.0 && now >= s.until)) {
			continue;
		}
		uint16_t zone = (uint16_t)i;
		float left = s.until > 0.0 ? (float)(s.until - now) : 0.0f;
		put(&zone, 2);
		put(&s.rgb, 4);
		put(&s.height, 4);
		put(&left, 4);
		n++;
	}
	m->insert(m->end(), (const uint8_t*)&n, (const uint8_t*)&n + 2);
	m->insert(m->end(), body.begin(), body.end());
}

void ZonesUnpack(const uint8_t*& p, const uint8_t* end)
{
	uint16_t n = 0;
	if (end - p < 2) {
		return;   // an older host's message: nothing about zones
	}
	memcpy(&n, p, 2);
	p += 2;
	std::lock_guard<std::mutex> lock(g_lock);
	double now = Now();
	std::vector<Shown> shown(g_zones.size());
	for (int i = 0; i < n && end - p >= 14; i++, p += 14) {
		uint16_t zone;
		Shown s;
		float left;
		memcpy(&zone, p, 2);
		memcpy(&s.rgb, p + 2, 4);
		memcpy(&s.height, p + 6, 4);
		memcpy(&left, p + 10, 4);
		s.on = true;
		s.until = left > 0.0f ? now + left : 0.0;
		if (zone < shown.size()) {
			shown[zone] = s;
		}
	}
	g_shown.swap(shown);
}

}
