#include "mods/lp2.h"
#include "mods/parts.h"

#include "api/xsession.h"
#include "core/config.h"
#include "core/log.h"
#include "core/memberdata.h"
#include "core/steam.h"
#include "core/utils.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>

// Extra match rules the stock game doesn't have. The host's rules hold in its lobby: each player
// publishes its own as member data "rules" ("ammo=1;grenades=0"), and everyone applies the lobby
// owner's (outside lobbies, their own). Each player's weapons run on its own PC, so every player
// applies them locally.
//
// Ammo: the weapon's add-ammo function (DX11 FUN_00c7aa20, thiscall(weapon, s16 delta) -> ok) takes
// a shot / throw as a negative delta off the magazine (+0x20fc) and the total (+0x20fe; DX9 both
// -0x10). Weapon kind at +0x1e: 8 and 0x15-0x19 are grenades (magazine = total).
//   Unlimited ammo: the total is put back after a shot, so the magazine still empties and reloads.
//   Unlimited grenades: both are put back.
//
// Secondary: a player spawns (DX11 FUN_008fb130, thiscall(spawner, bool remote, ...), player index at
// spawner+0xb0) with the weapons whose ids are in its player info (game->+0x28[index]) at +0x10 +
// slot * 4: slot 0 the weapon in hand, 1 grenades, 7 the carried second weapon (empty at spawn;
// picking a weapon up moves the one in hand there). An empty slot 7 gets the secondary's id first.
// Ids are 0x04KK0000, KK the weapon kind (0x0c sidearm).

namespace xls::mods {

namespace {

//
// The HUD shows the reserve through DX11 FUN_00c7a820 (thiscall(weapon, bool raw) -> s16): not raw,
// it answers -1 (drawn as the infinity sign, like the sidearm's) for weapons whose data says
// "infinite reserve"; with a rule on it answers -1 for the weapon too. The HUD's counter draw (DX11
// FUN_00eca930, thiscall(hud, slot, icon, count, style, counterOnly)) shows the infinity sign by
// style 6 (from a per-icon table), not by the count, so a count of -1 gets style 6.
struct RulesVas {
	uint32_t addAmmo;
	uint32_t reserve;
	uint32_t counter;
	uint32_t magazine; // the total follows it
	uint32_t spawn;
};
const RulesVas kRulesVas[] = {
	{0xc7aa20, 0xc7a820, 0xeca930, 0x20fc, 0x8fb130}, // DX11
	{0xc6ae10, 0xc6ac10, 0xebab30, 0x20ec, 0x8eb7d0}, // DX9
};
// push esi; push edi; mov edi,[esp+0xc]; test di,di; (then jge rel32, left in place)
const uint8_t kAddAmmoPrologue[] = {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x66, 0x85, 0xff};
const uint8_t kAddAmmoNext[] = {0x0f, 0x8d};
// cmp byte [esp+4],0 (then jne rel8, left in place)
const uint8_t kReservePrologue[] = {0x80, 0x7c, 0x24, 0x04, 0x00};
// sub esp,0x7c; push ebp; mov ebp,ecx
const uint8_t kCounterPrologue[] = {0x83, 0xec, 0x7c, 0x55, 0x8b, 0xe9};
const int kStyleInfinite = 6;

struct Rules {
	bool ammo = false;
	bool grenades = false;
	int secondary = 0; // weapon kind, 0 = none
};
const int kKindSidearm = 0x0c;
const int kSlotSecond = 7;

std::mutex g_mutex;
std::map<uint64_t, Rules> g_memberRules;
std::atomic<bool> g_ammo{false};
std::atomic<bool> g_grenades{false};
std::atomic<int> g_secondary{0};
uint32_t g_magazine = 0;

std::string Format(const Rules& r)
{
	return FormatA("ammo=%d;grenades=%d;secondary=%d", r.ammo, r.grenades, r.secondary);
}

Rules Parse(const std::string& s)
{
	Rules r;
	r.ammo = s.find("ammo=1") != std::string::npos;
	r.grenades = s.find("grenades=1") != std::string::npos;
	size_t at = s.find("secondary=");
	r.secondary = at == std::string::npos ? 0 : atoi(s.c_str() + at + 10) & 0xff;
	return r;
}

Rules Own()
{
	Rules r;
	r.ammo = Cfg().rulesUnlimitedAmmo;
	r.grenades = Cfg().rulesUnlimitedGrenades;
	r.secondary = Cfg().rulesSecondary == "sidearm" ? kKindSidearm : 0;
	return r;
}

// Once a second: the lobby owner's rules (ours outside lobbies or until the owner's arrive).
DWORD WINAPI SyncThread(LPVOID)
{
	std::string logged;
	for (;; Sleep(1000)) {
		Rules r = Own();
		CSteamID lobby = SessionPresenceLobby();
		if (lobby.IsValid() && SteamReady() && SteamMatchmaking()) {
			uint64_t host = LobbyOwner(SteamMatchmaking(), lobby).ConvertToUint64();
			std::lock_guard<std::mutex> lock(g_mutex);
			auto it = g_memberRules.find(host);
			if (host != SteamLocalId().ConvertToUint64() && it != g_memberRules.end()) {
				r = it->second;
			}
		}
		g_ammo = r.ammo;
		g_grenades = r.grenades;
		g_secondary = r.secondary;
		std::string now = Format(r);
		if (now != logged) {
			logged = now;
			XLS_LOG_INFO("mods: rules: unlimited ammo %s, unlimited grenades %s, secondary kind 0x%02x.", r.ammo ? "on" : "off", r.grenades ? "on" : "off", r.secondary);
		}
	}
}

using AddAmmoFn = uint32_t(__attribute__((thiscall)) *)(uint8_t*, int16_t);
AddAmmoFn g_addAmmoOriginal = nullptr;

bool Grenade(const uint8_t* weapon)
{
	uint8_t kind = weapon[0x1e];
	return kind == 8 || (kind >= 0x15 && kind <= 0x19);
}

using ReserveFn = uint32_t(__attribute__((thiscall)) *)(uint8_t*, uint32_t);
ReserveFn g_reserveOriginal = nullptr;

uint32_t __attribute__((thiscall)) Reserve(uint8_t* weapon, uint32_t raw)
{
	if (!(raw & 0xff) && weapon && (Grenade(weapon) ? g_grenades : g_ammo)) {
		return 0xffffffff;
	}
	return g_reserveOriginal(weapon, raw);
}

using CounterFn = void(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t, int32_t, uint32_t, uint32_t);
CounterFn g_counterOriginal = nullptr;

void __attribute__((thiscall)) Counter(void* hud, uint32_t slot, uint32_t icon, int32_t count, uint32_t style, uint32_t counterOnly)
{
	g_counterOriginal(hud, slot, icon, count, count < 0 ? kStyleInfinite : style, counterOnly);
}

// sub esp,0x10; push ebp; push esi; mov esi,ecx; then mov al,[esi+0xb0]
const uint8_t kSpawnPrologue[] = {0x83, 0xec, 0x10, 0x55, 0x56, 0x8b, 0xf1};
const uint8_t kSpawnIndex[] = {0x8a, 0x86, 0xb0, 0x00, 0x00, 0x00};
const size_t kSpawnGameAt = 0x1f; // mov ecx,[game] (8b 0d <addr>) at +0x1d
const uint8_t kSpawnGameOp[] = {0x8b, 0x0d};
using SpawnFn = uint32_t(__attribute__((thiscall)) *)(uint8_t*, uint32_t, void*, void*);
SpawnFn g_spawnOriginal = nullptr;
uint8_t** g_game = nullptr;

uint32_t __attribute__((thiscall)) Spawn(uint8_t* spawner, uint32_t remote, void* a, void* b)
{
	static uint32_t given[16]; // the id we put in each player's slot 7 (it stays in the player info)
	int kind = g_secondary;
	uint8_t index = spawner ? spawner[0xb0] : 0xff;
	if (!(remote & 0xff) && index < 16 && *g_game) {
		auto* info = *(uint8_t**)(*g_game + 0x28 + index * 4);
		auto* second = info ? (uint32_t*)(info + 0x10 + kSlotSecond * 4) : nullptr;
		uint32_t want = kind ? 0x04000000u | (uint32_t)kind << 16 : 0;
		if (second && given[index] && *second == given[index] && *second != want) {
			*second = 0; // ours from an earlier match with other rules
			given[index] = 0;
		}
		if (second && want && *second == 0) {
			*second = given[index] = want;
			XLS_LOG_INFO("mods: rules: player %u spawns with secondary 0x%08x.", index, want);
		}
	}
	return g_spawnOriginal(spawner, remote, a, b);
}

uint32_t __attribute__((thiscall)) AddAmmo(uint8_t* weapon, int16_t delta)
{
	if (delta >= 0 || !weapon) {
		return g_addAmmoOriginal(weapon, delta);
	}
	bool grenade = Grenade(weapon);
	if (!(grenade ? g_grenades : g_ammo)) {
		return g_addAmmoOriginal(weapon, delta);
	}
	auto* magazine = (int16_t*)(weapon + g_magazine);
	int16_t loaded = magazine[0];
	int16_t total = magazine[1];
	uint32_t ok = g_addAmmoOriginal(weapon, delta);
	if (grenade) {
		magazine[0] = loaded;
	}
	magazine[1] = total;
	return ok;
}

}

void RulesInstall(unsigned char* base)
{
	int build = -1;
	for (int b = 0; b < 2 && build < 0; b++) {
		const uint8_t* add = base + (kRulesVas[b].addAmmo - 0x400000);
		const uint8_t* reserve = base + (kRulesVas[b].reserve - 0x400000);
		if (memcmp(add, kAddAmmoPrologue, sizeof(kAddAmmoPrologue)) == 0 && memcmp(add + sizeof(kAddAmmoPrologue), kAddAmmoNext, sizeof(kAddAmmoNext)) == 0 &&
		    memcmp(reserve, kReservePrologue, sizeof(kReservePrologue)) == 0 &&
		    memcmp(base + (kRulesVas[b].counter - 0x400000), kCounterPrologue, sizeof(kCounterPrologue)) == 0 &&
		    memcmp(base + (kRulesVas[b].spawn - 0x400000), kSpawnPrologue, sizeof(kSpawnPrologue)) == 0 &&
		    memcmp(base + (kRulesVas[b].spawn - 0x400000) + sizeof(kSpawnPrologue), kSpawnIndex, sizeof(kSpawnIndex)) == 0 &&
		    memcmp(base + (kRulesVas[b].spawn - 0x400000) + kSpawnGameAt - 2, kSpawnGameOp, sizeof(kSpawnGameOp)) == 0) {
			build = b;
		}
	}
	if (build < 0) {
		XLS_LOG_WARN("mods: rules: add-ammo code not recognised, unlimited ammo / grenades unavailable.");
		return;
	}
	g_magazine = kRulesVas[build].magazine;
	g_addAmmoOriginal = (AddAmmoFn)Detour(base + (kRulesVas[build].addAmmo - 0x400000), kAddAmmoPrologue, sizeof(kAddAmmoPrologue), (void*)&AddAmmo);
	g_reserveOriginal = (ReserveFn)Detour(base + (kRulesVas[build].reserve - 0x400000), kReservePrologue, sizeof(kReservePrologue), (void*)&Reserve);
	g_counterOriginal = (CounterFn)Detour(base + (kRulesVas[build].counter - 0x400000), kCounterPrologue, sizeof(kCounterPrologue), (void*)&Counter);
	uint8_t* spawn = base + (kRulesVas[build].spawn - 0x400000);
	g_game = *(uint8_t***)(spawn + kSpawnGameAt);
	g_spawnOriginal = (SpawnFn)Detour(spawn, kSpawnPrologue, sizeof(kSpawnPrologue), (void*)&Spawn);
	if (!g_addAmmoOriginal || !g_reserveOriginal || !g_counterOriginal || !g_spawnOriginal) {
		XLS_LOG_WARN("mods: rules: add-ammo hook failed.");
		return;
	}
	MemberDataListen("rules", [](uint64_t steamId, XUID, const std::string& value) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_memberRules[steamId] = Parse(value);
	});
	MemberDataSet("rules", Format(Own()));
	HANDLE t = CreateThread(nullptr, 0, SyncThread, nullptr, 0, nullptr);
	if (t) {
		CloseHandle(t);
	}
	XLS_LOG_INFO("mods: rules: hooked (%s).", build == 0 ? "DX11" : "DX9");
}

}
