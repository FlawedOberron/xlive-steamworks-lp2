#include "mods/lp2.h"
#include "mods/parts.h"

#include "core/log.h"

#include <atomic>
#include <stdint.h>

// Quick Resupply, for versus matches: running over a weapon you carry takes its ammo and it's gone, with no button or animation.
//
// uFighter::vf65 (DX11 0x9db4c0) runs for each unit near a fighter; for a weapon it sets up the pickup prompt, unless one of the
// fighter's 8 slots holds the same id (top 3 bytes) and that gun can't take more (FUN_00c7acc0, which always says no for a handgun).
//   Ammo: FUN_00c7cee0, what the game's own same-gun pickup calls, so pickups aren't scaled twice (rules.cpp, g_transfer).
//   Handgun: nothing to add, it just goes, so the floor isn't left with pistols nobody who has one can take.
//   Gone: hidden, not destroyed. +0x2157 is the game's "not shown" for a weapon (set for one lost in a fall), +4 bit 0x800 draws it and
//   equipping clears the mark. Other machines' messages can still name the object safely, and vf65 skips one we marked.
// Each machine does this itself, for puppets too: no message says the weapon went, so a machine that doesn't see the touch keeps
// showing it until someone there runs over it. Only the machine that owns a fighter adds its ammo.

namespace xls::mods {

namespace {

struct Vas {
	uint32_t scan, canTake, transfer, popup, popupThis, viewer, game, shift;
};
const Vas kVas[kBuildCount] = {
	{0x9db4c0, 0xc7acc0, 0xc7cee0, 0x859460, 0x1852144, 0x1852078, 0x18520c0, 0},
	{0x9cbb00, 0xc6b0b0, 0xc6d2d0, 0x84a010, 0x18420a4, 0x1841fd8, 0x1842020, 0x10},
	{0x9db3a0, 0xc7ace0, 0xc7cf00, 0x8596a0, 0x1853144, 0x1853078, 0x18530c0, 0},
	{0x9cb7f0, 0xc6aca0, 0xc6cec0, 0x84a140, 0x18420a4, 0x1841fd8, 0x1842020, 0x10},
};
const uint8_t kPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0};
const int kKindHuman = 1, kKindWeapon = 4, kKindHandgun = 0x0c, kSlots = 8, kPopupAmmo = 0x13;
const float kReach = 120.0f, kReachUp = 200.0f; // cm: vf65 runs well before a weapon is underfoot

using ScanFn = void(__attribute__((thiscall)) *)(uint8_t*, int, int, uint8_t*);
using CanTakeFn = uint32_t(__attribute__((thiscall)) *)(uint8_t*);
using TransferFn = uint32_t(__attribute__((thiscall)) *)(uint8_t*, int, int);
using PopupFn = void(__attribute__((thiscall)) *)(void*, int, int, uint32_t, int, int, int);

ScanFn g_scan = nullptr;
CanTakeFn g_canTake = nullptr;
TransferFn g_transfer = nullptr;
PopupFn g_popup = nullptr;
void** g_popupThis = nullptr;
uint8_t** g_viewer = nullptr;
uint8_t** g_game = nullptr;
uint32_t g_shift = 0;
std::atomic<bool> g_on{false};

bool Live(const uint8_t* unit)
{
	uint32_t state = *(const uint32_t*)(unit + 4) & 7;
	return state == 1 || state == 2;
}

uint8_t& Hidden(uint8_t* weapon) { return weapon[0x2157 - g_shift]; }
bool Marked(uint8_t* weapon) { return Hidden(weapon) && !(*(uint32_t*)(weapon + 4) & 0x800); }

void Hide(uint8_t* weapon)
{
	Hidden(weapon) = 1;
	*(uint32_t*)(weapon + 4) &= ~0x800u;
	weapon[0x1ceb - g_shift] = 1;
}

// The viewer's controller, when this fighter is the viewer's character: for the on-screen message.
uint8_t* ViewerControl(uint8_t* fighter)
{
	uint8_t* holder = g_viewer ? *g_viewer : nullptr;
	uint8_t* control = holder ? *(uint8_t**)(holder + 0x480) : nullptr;
	return control && Live(control) && *(uint8_t**)(control + 0x40) == fighter ? control : nullptr;
}

bool Near(const uint8_t* a, const uint8_t* b)
{
	const float* p = (const float*)(a + 0x30);
	const float* q = (const float*)(b + 0x30);
	float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
	return dx * dx + dz * dz < kReach * kReach && dy * dy < kReachUp * kReachUp;
}

// True when the weapon was taken.
bool Touch(uint8_t* fighter, uint8_t* weapon)
{
	uint32_t id = *(uint32_t*)(weapon + 0x1c);
	auto** slots = (uint8_t**)(fighter + 0x1688 - g_shift);
	for (int i = 0; i < kSlots; i++) {
		uint8_t* mine = slots[i];
		if (!mine || !Live(mine) || mine == weapon || *(uint32_t*)(mine + 0x1c) >> 8 != id >> 8) {
			continue;
		}
		bool handgun = weapon[0x1e] == kKindHandgun;
		int16_t ammo = *(int16_t*)(weapon + 0x20fe - g_shift);
		if (!handgun && (ammo <= 0 || !(g_canTake(mine) & 0xff))) {
			return false; // full, or an empty gun: it stays, as it does for the pickup button
		}
		uint8_t role = fighter[0xcd0 - g_shift];
		if (!handgun && role != 2 && role != 3) {
			g_transfer(mine, ammo, 0);
		}
		if (uint8_t* control = ViewerControl(fighter)) {
			if (g_popupThis && *g_popupThis) {
				g_popup(*g_popupThis, kPopupAmmo, (int)(char)control[0xb0], id, 0, 0, 0);
			}
		}
		Hide(weapon);
		static std::atomic<int> logged{0};
		if (logged.fetch_add(1) < 20) {
			XLS_LOG_INFO("mods: resupply: slot %d took %08x (%d rounds, role %d).", i, id, handgun ? 0 : ammo, role);
		}
		return true;
	}
	return false;
}

void __attribute__((thiscall)) Scan(uint8_t* fighter, int a, int b, uint8_t* other)
{
	if (g_on && other && fighter && g_game && *g_game && (*g_game)[0x256] && other[0x1f] == kKindWeapon && Live(other) &&
	    !*(uint8_t**)(other + 0x24)) {
		if (Marked(other)) {
			return;
		}
		if (fighter[0x1f] == kKindHuman && Near(fighter, other) && Touch(fighter, other)) {
			return;
		}
	}
	g_scan(fighter, a, b, other);
}

}

void ResupplySet(bool on)
{
	g_on = on;
}

void ResupplyInstall(uint8_t* base, int build)
{
	const Vas& v = kVas[build];
	auto at = [&](uint32_t va) { return base + (va - 0x400000); };
	g_shift = v.shift;
	g_canTake = (CanTakeFn)at(v.canTake);
	g_transfer = (TransferFn)at(v.transfer);
	g_popup = (PopupFn)at(v.popup);
	g_popupThis = (void**)at(v.popupThis);
	g_viewer = (uint8_t**)at(v.viewer);
	g_game = (uint8_t**)at(v.game);
	g_scan = (ScanFn)Detour(at(v.scan), kPrologue, sizeof(kPrologue), (void*)&Scan);
	if (!g_scan) {
		XLS_LOG_WARN("mods: resupply: scan not hooked, Quick Resupply does nothing.");
	}
}

}
