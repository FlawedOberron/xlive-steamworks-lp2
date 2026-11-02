#include "mods/lp2.h"
#include "mods/packages.h"

#include "core/log.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <mutex>
#include <vector>

// Map packages' post-process volumes (map.json "post_volumes"): the screen is colour graded while the
// camera is inside one.
//
// The game's colour grading is uColorCorrectFilter (size 0x450): 16 correctors at +0x30 (0x30 each: +4 mEnable,
// +8 mType, +0x10 mFactor, +0x20 mFactor2) folded into one colour matrix every update (vf6, DX9 0x478720).
// Types used here: 8 = multiply red / green / blue by mFactor, 5 = saturation (mFactor.x 0.5 = unchanged,
// 0 = grey), 10 = levels (mFactor = black point, mFactor2 = white point, per channel; alpha kept).
// (Until 2026-10-29 levels used type 9, which is a colour-space transform scaled by mFactor.x, not levels: with our
// black point 0 it broke the alpha the fog and depth of field read, so on maps with a post volume any fog drew black
// and the frame blurred.)
//
// The map's own colour filter (its Filter scheduler's ColorCorrectFilter: the map's tone curves and correctors
// 0-1) does the grading: vf6 (update, uColorCorrectFilter::vf6) is patched in place in the three vtables that use it
// (uColorCorrectFilter, uPlanetColorCorrectFilter and its sibling class), and the filter updated most often (the
// one drawn every frame: kOwnerWindow) sets correctors 13-15 (unused by the stock schedulers) to the camera's
// volume, faded over its "fade" seconds, before the game folds them. The other filters get ours off (no double
// grade). Outside every volume the tint stays at kNearOne, not 1: an exactly neutral grade makes the game skip the
// filter's draw, and the pass switching on / off at a volume's edge showed as a dark moment.
// (2026-10-29 first try: only uPlanetColorCorrectFilter's vtable patched; that one updated once, the map's didn't.)
// (Until 2026-10-29 we added a second filter of this class, made like campaign area 6-19's: with the map's own one
// the two took turns, the screen flickering between their grades.)
//
// The camera: sCamera ([0x18c0c3c]) viewport 0 (+0x30, 0x1a0 each) mpCamera (+4), uCamera mCameraPos (+0x40).

namespace xls::mods {

namespace {

struct Build {
	uint32_t factory;    // uPlanetColorCorrectFilter's DTI factory (only to recognise the build: it stores vtables[0])
	uint32_t vtables[3]; // uPlanetColorCorrectFilter, its sibling, uColorCorrectFilter: vf6 = uColorCorrectFilter::vf6
	uint32_t camera;     // sCamera pointer
};
const Build kBuilds[kBuildCount] = {
	{0x3ce7f0, {0xf66ca4, 0xf66ce4, 0x12dfcbc}, 0x14d0cdc}, // LP2DX11.exe
	{0x3bf970, {0xf56ca8, 0xf56ce8, 0x12cfc20}, 0x14c0c3c}, // LP2DX9.exe
	{0x3ce8c0, {0xf66ca4, 0xf66ce4, 0x12e002c}, 0x14d1cdc}, // LP2DX11.exe 1.1
	{0x3bfaa0, {0xf56cb8, 0xf56cf8, 0x12cfe88}, 0x14c0c3c}, // LP2DX9.exe 1.1
};
const int kSeen = 8;             // filters counted at once
const float kOwnerWindow = 1.0f; // s: the filter updated most in each window grades the next
const int kFirst = 13;       // our correctors: 13 tint, 14 saturation, 15 levels (contrast)
const float kNearOne = 0.999f; // the tint outside volumes (see above)

struct Grade {
	float tint[3] = {1.0f, 1.0f, 1.0f};
	float saturation = 1.0f;
	float contrast = 1.0f;
};

uint8_t* g_base = nullptr;
const Build* g_build = nullptr;
void(__fastcall* g_origUpdate)(uint8_t*, void*) = nullptr;

std::mutex g_mutex;               // the map's volumes (set at area load, read by the filter's update)
std::vector<PostVolume> g_volumes;
struct Seen {
	uint8_t* self;
	int calls;
};
Seen g_seen[kSeen];               // filters updated in this window
int g_nSeen = 0;
uint8_t* g_owner = nullptr;       // the one graded (the busiest last window)
LARGE_INTEGER g_windowStart;
Grade g_now;                      // shown now (the screen gets it x kNearOne)
Grade g_from;                     // where the current fade started
int g_target = -1;                // the volume faded towards (-1: none, neutral)
float g_fade = 0.0f;              // its length (s): the entered volume's, or the left one's when leaving
LARGE_INTEGER g_fadeStart;

uint8_t* Global(uint32_t rva)
{
	return *(uint8_t**)(g_base + rva);
}

bool CameraPos(float out[3])
{
	uint8_t* camera = Global(g_build->camera);
	uint8_t* cam = camera ? *(uint8_t**)(camera + 0x30 + 4) : nullptr;
	if (!cam) {
		return false;
	}
	memcpy(out, cam + 0x40, 12);
	return true;
}

void SetCorrector(uint8_t* filter, int i, int type, const float* f, const float* f2, bool on)
{
	uint8_t* c = filter + 0x30 + i * 0x30;
	c[4] = on ? 1 : 0;
	*(int32_t*)(c + 8) = type;
	float* factor = (float*)(c + 0x10);
	float* factor2 = (float*)(c + 0x20);
	for (int k = 0; k < 4; k++) {
		factor[k] = k < 3 ? f[k] : 0.0f;
		factor2[k] = f2 && k < 3 ? f2[k] : 0.0f;
	}
}

float Seconds(const LARGE_INTEGER& since)
{
	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	return (float)(now.QuadPart - since.QuadPart) / (float)freq.QuadPart;
}

void __fastcall Update(uint8_t* self, void* edx)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_volumes.empty()) {   // not our map, or no post volumes: the map's grade alone
			g_origUpdate(self, edx);
			return;
		}
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		int i = 0;
		while (i < g_nSeen && g_seen[i].self != self) {
			i++;
		}
		if (i == g_nSeen && g_nSeen < kSeen) {
			g_seen[g_nSeen++] = {self, 0};
		}
		if (i < g_nSeen) {
			g_seen[i].calls++;
		}
		if (!g_owner) {
			g_owner = self;
		}
		if (Seconds(g_windowStart) >= kOwnerWindow) {
			int best = 0;
			for (int k = 1; k < g_nSeen; k++) {
				if (g_seen[k].calls > g_seen[best].calls) {
					best = k;
				}
			}
			if (g_nSeen && g_seen[best].self != g_owner) {
				g_owner = g_seen[best].self;
				XLS_LOG_INFO("mods: post: grading colour filter %p (vtable %p, %d updates in %.1f s; %d filter(s) seen).",
					g_owner, *(void**)g_owner, g_seen[best].calls, kOwnerWindow, g_nSeen);
				for (int k = 0; k < g_nSeen; k++) {
					XLS_LOG_INFO("mods: post:   filter %p vtable %p: %d updates.", g_seen[k].self, *(void**)g_seen[k].self, g_seen[k].calls);
				}
			}
			for (int k = 0; k < g_nSeen; k++) {
				g_seen[k].calls = 0;
			}
			QueryPerformanceCounter(&g_windowStart);
		}
	}
	if (self != g_owner) {   // not the map's: ours off
		float one[3] = {1.0f, 1.0f, 1.0f};
		for (int k = 0; k < 3; k++) {
			SetCorrector(self, kFirst + k, 8, one, nullptr, false);
		}
		g_origUpdate(self, edx);
		return;
	}
	Grade want;
	int target = -1;
	float fade = 0.0f;
	float pos[3];
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_volumes.empty() && CameraPos(pos)) {
			for (size_t i = 0; i < g_volumes.size(); i++) {
				const PostVolume& v = g_volumes[i];
				if (ScriptVolumeOn(v.sid) && InBox(v.min, v.max, v.turn, pos)) {
					float bright = ScriptValue(v.sid, kSetBrightness, v.brightness);
					for (int k = 0; k < 3; k++) {
						want.tint[k] = ScriptValue(v.sid, kSetTintR + k, v.tint[k]) * bright;
					}
					want.saturation = ScriptValue(v.sid, kSetSaturation, v.saturation);
					float contrast = ScriptValue(v.sid, kSetContrast, v.contrast);
					want.contrast = contrast > 0.05f ? contrast : 0.05f;
					target = (int)i;
					fade = v.fade;
					break;
				}
			}
		}
		if (target != g_target) {
			// a new fade from what shows now: into a volume over its fade time, out of one over the left one's
			if (target < 0 && g_target >= 0 && g_target < (int)g_volumes.size()) {
				fade = g_volumes[g_target].fade;
			}
			if (target >= 0) {
				XLS_LOG_INFO("mods: post: camera in post volume %d (fade %.2f s).", target, fade);
			}
			else {
				XLS_LOG_INFO("mods: post: camera out of post volumes (fade %.2f s).", fade);
			}
			g_from = g_now;
			g_target = target;
			g_fade = fade;
			QueryPerformanceCounter(&g_fadeStart);
		}
	}
	float t = g_fade > 0.0f ? Seconds(g_fadeStart) / g_fade : 1.0f;
	bool settled = t >= 1.0f;
	if (settled) {
		g_now = want;   // done (exactly)
	}
	else {
		t = t * t * (3.0f - 2.0f * t);   // smoothstep
		for (int k = 0; k < 3; k++) {
			g_now.tint[k] = g_from.tint[k] + (want.tint[k] - g_from.tint[k]) * t;
		}
		g_now.saturation = g_from.saturation + (want.saturation - g_from.saturation) * t;
		g_now.contrast = g_from.contrast + (want.contrast - g_from.contrast) * t;
	}
	float sat[3] = {g_now.saturation * 0.5f, 0.0f, 0.0f};
	float lo = 0.5f - 0.5f / g_now.contrast;
	float hi = 0.5f + 0.5f / g_now.contrast;
	float los[3] = {lo, lo, lo};
	float his[3] = {hi, hi, hi};
	float tint[3];
	for (int k = 0; k < 3; k++) {
		tint[k] = g_now.tint[k] * kNearOne;   // never exactly neutral: the pass keeps drawing
	}
	SetCorrector(self, kFirst, 8, tint, nullptr, true);
	SetCorrector(self, kFirst + 1, 5, sat, nullptr, true);
	SetCorrector(self, kFirst + 2, 10, los, his, true);
	g_origUpdate(self, edx);
}

}

void PostInstall(unsigned char* base)
{
	for (const Build& b : kBuilds) {
		if (GameBuild() >= 0 && &b - kBuilds != GameBuild()) {
			continue;
		}
		// the factory stores the vtable: mov dword [esi], vtable
		uint8_t* at = base + b.factory + 0x22;
		if (at[0] != 0xc7 || at[1] != 0x06 || *(uint32_t*)(at + 2) != (uint32_t)(uintptr_t)(base + b.vtables[0])) {
			continue;
		}
		g_base = base;
		g_build = &b;
		void* orig = ((void**)(base + b.vtables[0]))[6];
		for (uint32_t vt : b.vtables) {
			void** slot = (void**)(base + vt) + 6;
			if (*slot != orig) {
				XLS_LOG_WARN("mods: post: vtable %p's update isn't the colour filter's, post-process volumes ignored.", slot - 6);
				return;
			}
		}
		g_origUpdate = (void(__fastcall*)(uint8_t*, void*))orig;
		for (uint32_t vt : b.vtables) {
			void** slot = (void**)(base + vt) + 6;
			DWORD old;
			if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
				XLS_LOG_WARN("mods: post: the colour filter's vtable can't be patched, post-process volumes ignored.");
				return;
			}
			*slot = (void*)&Update;
			VirtualProtect(slot, sizeof(void*), old, &old);
		}
		XLS_LOG_INFO("mods: post: installed (%s).", BuildName((int)(&b - kBuilds)));
		return;
	}
	XLS_LOG_WARN("mods: post: colour filter not recognised, post-process volumes ignored.");
}

void PostSetMap(const MapPhysics* physics)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_volumes = physics ? physics->post : std::vector<PostVolume>();
	g_now = Grade();   // each map starts out of every volume
	g_target = -1;
	g_fade = 0.0f;
	g_nSeen = 0;       // and finds its filter again
	g_owner = nullptr;
	QueryPerformanceCounter(&g_windowStart);
}

}
