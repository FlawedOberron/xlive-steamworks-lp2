#include "plugin.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

// hold_zoom: zoom while the zoom button is held instead of toggling on each press.
//
// Each frame the character's input code (DX11 0x923d30, DX9 0x9143a0) asks its controller "was zoom pressed?"
// (virtual +0x104, DX11 FUN_00939780: pad+0x3c0 bit 0x100000, set for one frame) and, if so,
// dispatches the zoom action (virtual +0xb8, FUN_00938630), which steps the zoom level
// (FUN_00c78680: level+1, wrapping to 0 after the weapon's last level) and syncs it.
// The pad also has the held state (pad+0x3b8 bit 0x100000). The check is redirected through
// ZoomCheck: a real press still zooms in; when the button is no longer held but the weapon is
// zoomed, the level is set to the last one and a press is reported, so the game's own step wraps
// it to 0 (unzoomed) through the normal, synced path.
//
//   mov ebp,[esp+0x5c]; mov edx,[ebp]; mov eax,[edx+0x104]; mov ecx,ebp; call eax; test al,al;
//   je +0xe; mov edx,[ebp]; mov eax,[edx+0xb8]
// The 13 bytes from mov edx,[ebp] become: mov ecx,ebp; push esi; call ZoomCheck; nop x5.

namespace controls {

namespace {

const uint8_t kSiteBytes[] = {0x8b, 0x6c, 0x24, 0x5c, 0x8b, 0x55, 0x00, 0x8b, 0x82, 0x04, 0x01, 0x00, 0x00, 0x8b, 0xcd, 0xff,
                              0xd0, 0x84, 0xc0, 0x74, 0x0e, 0x8b, 0x55, 0x00, 0x8b, 0x82, 0xb8, 0x00, 0x00, 0x00};
const size_t kPatchAt = 4;
const size_t kPatchSize = 13;
const uint32_t kZoomHeld = 0x100000;

// Addresses differ per exe; the pad layout (index at +0x84, held set at pad+0x3b8) does not.
struct Layout {
	const char* exe;
	uint32_t siteRva;
	uint32_t pads;       // pad object per local player index
	uint32_t ownerFn;    // thiscall(character) -> object holding the zoom state
	uint32_t zoomLevels; // u8 level count, 8 bytes per weapon type
	uint32_t weapon;     // owner field: current weapon (byte +0x4b = weapon type)
	uint32_t level;      // owner field: current zoom level
};
const Layout kLayouts[] = {
	{"LP2DX11.exe", 0x523d30, 0x1852080, 0x9aab90, 0x14d766c, 0x2184, 0x2118},
	{"LP2DX9.exe", 0x5143a0, 0x1841fe0, 0x99b3b0, 0x14c7514, 0x2174, 0x2108},
};
const Layout* g_layout = nullptr;

using CheckFn = bool(__attribute__((thiscall)) *)(void*);
using OwnerFn = uint8_t*(__attribute__((thiscall)) *)(void*);

bool __attribute__((thiscall)) ZoomCheck(uint8_t* self, uint8_t* character)
{
	auto check = (CheckFn)(*(void***)self)[0x104 / 4];
	if (check(self)) {
		return true;
	}
	uint8_t index = self[0x84];
	if (index >= 16 || !character) {
		return false;
	}
	const Layout& L = *g_layout;
	uint8_t* pad = ((uint8_t**)(uintptr_t)L.pads)[index];
	if (!pad || (*(uint32_t*)(pad + 0x3b8) & kZoomHeld)) {
		return false; // no pad, or the button is still held
	}
	uint8_t* owner = ((OwnerFn)(uintptr_t)L.ownerFn)(character);
	if (!owner) {
		return false;
	}
	uint32_t kind = *(uint32_t*)(owner + 4) & 7;
	uint8_t* weapon = *(uint8_t**)(owner + L.weapon);
	if ((kind != 1 && kind != 2) || !weapon || owner[L.level] == 0) {
		return false;
	}
	uint8_t levels = ((uint8_t*)(uintptr_t)L.zoomLevels)[weapon[0x4b] * 8];
	if (levels < 1) {
		return false;
	}
	owner[L.level] = (uint8_t)(levels - 1); // the step from the last level wraps to 0
	return true;
}

}

bool HoldZoomTryInstall(unsigned char* base)
{
	uint8_t* site = nullptr;
	for (const Layout& L : kLayouts) {
		if (InImage(base, L.siteRva, sizeof(kSiteBytes)) && memcmp(base + L.siteRva, kSiteBytes, sizeof(kSiteBytes)) == 0) {
			site = base + L.siteRva;
			g_layout = &L;
			break;
		}
	}
	if (!site) {
		return false;
	}
	uint8_t patch[kPatchSize] = {0x8b, 0xcd, 0x56, 0xe8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
	int32_t rel = (int32_t)((uint8_t*)&ZoomCheck - (site + kPatchAt + 3 + 5));
	memcpy(patch + 4, &rel, 4);
	DWORD old = 0;
	if (!VirtualProtect(site + kPatchAt, kPatchSize, PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	memcpy(site + kPatchAt, patch, kPatchSize);
	VirtualProtect(site + kPatchAt, kPatchSize, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site + kPatchAt, kPatchSize);
	LOG_INFO("hold to aim: zoom check hooked at 0x%p (%s layout).", site + kPatchAt, g_layout->exe);
	return true;
}

}
