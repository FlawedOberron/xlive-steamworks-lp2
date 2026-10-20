#include "mods/lp2.h"
#include "mods/package.h"
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
//   Ammo supply (guns, not grenades): the start total (magazine included) becomes N magazines. It is
//   the weapon data's +0x30, set by the weapon setup (DX11 FUN_00c86160), so before that runs every
//   data entry's start total is set from its magazine (+0x32), at most the cap (+0x34; -1 =
//   infinite, left alone). Pickups (positive deltas) scale by the same new / original start of the
//   weapon's entry; the game clamps them to the cap.
//
// Secondary: a player spawns (DX11 FUN_008fb130, thiscall(spawner, bool remote, ...), player index at
// spawner+0xb0) with the weapons whose ids are in its player info (game->+0x28[index]) at +0x10 +
// slot * 4: slot 0 the weapon in hand, 1 grenades, 7 the carried second weapon (empty at spawn;
// picking a weapon up moves the one in hand there). An empty slot 7 gets the secondary's id first.
// Ids are 0x04KK0000, KK the weapon kind (0x0c sidearm).
//
// Create-match rows: the rules bytes (0x38, saved with the match settings) use offsets up to 0x2d;
// ours are 0x2e secondary (0 none, then kSecondaryKinds), 0x2f unlimited ammo, 0x30 unlimited grenades. The
// row table (DX11 0x17e9840) holds per category (16 x 0x23 bytes: main, sub, then per page a row
// count and 10 rules offsets) which offset each row edits; the Settings 1 page (page 1, 7 rows in 14
// categories) gets ours as rows 7-9. The Flash (id221) has its own rows: OnlineRule.sub1ItemArray
// [category] lists item ids, offsetArray[item] = [MsgOnline value base, DataNo]; the label is
// MsgOnline 38 + item. Ours are items 500-502 (labels 538-540, served by RulesMessage), values
// pushed as DataNo 75-77 next to the game's rules.

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
	uint32_t pushRules; // the create screen's push of every rule to its Flash: thiscall(screen, rules)
	uint32_t setNumber; // the screen's Flash number setter: thiscall(screen, dataNo, value)
	uint32_t rowTable;  // create-match rows (see RowTable)
	uint32_t setup;     // weapon setup (DX11 FUN_00c86160, thiscall(weapon, bool)): start reserve from its data
	uint32_t data;      // weapon -> its 3 data entries (0x54 bytes: +0x30 start total, +0x32 magazine, +0x34 cap)
	uint32_t transfer;  // return address of the add-ammo call that moves a picked-up weapon's ammo (DX11 FUN_00c7cee0)
	uint32_t leftCounter; // the HUD's left (grenade) counter draw, as counter (DX11 FUN_00ecac50)
	uint32_t icon;        // the HUD's icon for an item: thiscall(hud, weapon, a, b, c) -> icon (DX11 FUN_00ed1e00)
};
const RulesVas kRulesVas[] = {
	{0xc7aa20, 0xc7a820, 0xeca930, 0x20fc, 0x8fb130, 0xe8c270, 0xf4a330, 0x17e9840, 0xc86160, 0x2180, 0xc7cf15, 0xecac50, 0xed1e00}, // DX11
	{0xc6ae10, 0xc6ac10, 0xebab30, 0x20ec, 0x8eb7d0, 0xe7c1e0, 0xf3a600, 0x17d97e0, 0xc76550, 0x2170, 0xc6d305, 0xebae50, 0xec1ff0}, // DX9
};
// push esi; push edi; mov edi,[esp+0xc]; test di,di; (then jge rel32, left in place)
const uint8_t kAddAmmoPrologue[] = {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x66, 0x85, 0xff};
const uint8_t kAddAmmoNext[] = {0x0f, 0x8d};
// cmp byte [esp+4],0 (then jne rel8, left in place)
const uint8_t kReservePrologue[] = {0x80, 0x7c, 0x24, 0x04, 0x00};
// sub esp,0x7c; push ebp; mov ebp,ecx
const uint8_t kCounterPrologue[] = {0x83, 0xec, 0x7c, 0x55, 0x8b, 0xe9};
const int kStyleInfinite = 6;
// sub esp,0x8c; push edi; mov edi,ecx
const uint8_t kLeftCounterPrologue[] = {0x81, 0xec, 0x8c, 0x00, 0x00, 0x00, 0x57, 0x8b, 0xf9};
// mov edx,[esp+0x10]; push esi; mov esi,ecx
const uint8_t kIconPrologue[] = {0x8b, 0x54, 0x24, 0x10, 0x56, 0x8b, 0xf1};

// secondary values: none, then the weapon kinds of kSecondaryKinds
const int kSecondaryKinds[] = {0, 0x0c, 0x30, 0x31, 0x32, 0x33, 0x34};
const char* const kSecondaryNames[] = {"None", "Handguns", "Standard", "Short Range", "Long Range", "Heavy Weapons", "Support Weapons"};
// Values that reuse the game's Default Weapon texts (MsgOnline 149-153: Standard, Short Range, Long
// Range, Heavy Weapons, Handguns) so they follow the language; 0 = ours.
const int kSecondaryGameMsg[] = {0, 153, 149, 150, 151, 152, 0};
const int kSecondaryValues = (int)(sizeof(kSecondaryKinds) / sizeof(kSecondaryKinds[0]));
// ammo supply values: magazines to start with (0 = the game's own amounts, as in settings saved
// before the row existed)
const int kSupplyMagazines[] = {0, 1, 2, 3, 5, 8, 12};
const char* const kSupplyNames[] = {"Default", "Dry", "Scarce", "Less", "Normal", "Abundant", "Excessive"};
const int kSupplyValues = (int)(sizeof(kSupplyMagazines) / sizeof(kSupplyMagazines[0]));

const int kMsgItemName = 38; // MsgOnline label of item i: 38 + i
const int kMsgOffOn = 362;   // MsgOnline MSG_COMMON_OFF (Off, On)
const int kMsgSecondary = 600;
const int kMsgSupply = 610;

// The pages' Flash arrays (Settings 2 = sub1, Settings 3 = sub2) and their stock row counts here
// (Settings 3 also has a cross-play row last in the Flash, hidden on PC and not counted here).
const int kPageSettings2 = 1;
const int kPageSettings3 = 2;
struct Row {
	int page;
	int offset;  // in the rules bytes
	int values;
	int item;    // Flash item id (label MsgOnline 38 + item)
	int dataNo;
	int msgBase; // value texts: MsgOnline msgBase + value
	const char* label;
};
const Row kRowDefs[] = {
	{kPageSettings2, 0x2e, kSecondaryValues, 500, 75, kMsgSecondary, "Default Secondary"},
	{kPageSettings2, 0x2f, 2, 501, 76, kMsgOffOn, "Unlimited Ammo"},
	{kPageSettings2, 0x30, 2, 502, 77, kMsgOffOn, "Unlimited Grenades"},
	{kPageSettings3, 0x31, kSupplyValues, 503, 78, kMsgSupply, "Ammo Supply"},
};
const int kRows = (int)(sizeof(kRowDefs) / sizeof(kRowDefs[0]));
enum { kRowSecondary, kRowAmmo, kRowGrenades, kRowSupply };
// Categories 0-13 have the full pages (14 / 15, numbered battle and random, have one row there).
const int kRowCategories = 14;

struct Rules {
	bool ammo = false;
	bool grenades = false;
	int secondary = 0; // weapon kind, 0 = none
	int supply = 0;     // magazines to start with, 0 = the game's amounts (pickups scale with it)
};
const int kSlotSecond = 7;

std::mutex g_mutex;
std::map<uint64_t, Rules> g_memberRules;
std::atomic<bool> g_ammo{false};
std::atomic<bool> g_grenades{false};
std::atomic<int> g_secondary{0};
std::atomic<int> g_supply{0};
uint32_t g_magazine = 0;

std::string Format(const Rules& r)
{
	return FormatA("ammo=%d;grenades=%d;secondary=%d;magazines=%d", r.ammo, r.grenades, r.secondary, r.supply);
}

Rules Parse(const std::string& s)
{
	Rules r;
	r.ammo = s.find("ammo=1") != std::string::npos;
	r.grenades = s.find("grenades=1") != std::string::npos;
	size_t at = s.find("secondary=");
	r.secondary = at == std::string::npos ? 0 : atoi(s.c_str() + at + 10) & 0xff;
	at = s.find("magazines=");
	r.supply = at == std::string::npos ? 0 : atoi(s.c_str() + at + 10);
	if (r.supply < 0 || r.supply > 100) {
		r.supply = 0;
	}
	return r;
}

std::atomic<bool> g_menuSeen{false};
std::atomic<int> g_menu[kRows];

Rules Own()
{
	Rules r;
	if (g_menuSeen) {
		r.ammo = g_menu[kRowAmmo] != 0;
		r.grenades = g_menu[kRowGrenades] != 0;
		r.secondary = kSecondaryKinds[g_menu[kRowSecondary] % kSecondaryValues];
		r.supply = kSupplyMagazines[g_menu[kRowSupply] % kSupplyValues];
		return r;
	}
	r.ammo = Cfg().rulesUnlimitedAmmo;
	r.grenades = Cfg().rulesUnlimitedGrenades;
	for (int i = 0; i < kSecondaryValues; i++) {
		if (_stricmp(Cfg().rulesSecondary.c_str(), kSecondaryNames[i]) == 0) {
			r.secondary = kSecondaryKinds[i];
		}
	}
	return r;
}

// Once a second: the lobby owner's rules (ours outside lobbies or until the owner's arrive).
DWORD WINAPI SyncThread(LPVOID)
{
	std::string logged, published = Format(Own());
	for (;; Sleep(1000)) {
		Rules r = Own();
		if (Format(r) != published) {
			published = Format(r);
			MemberDataSet("rules", published);
			Bytes save(published.begin(), published.end());
			WriteWholeFile(Cfg().modCacheDir + L"rules.txt", save);
		}
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
		g_supply = r.supply;
		std::string now = Format(r);
		if (now != logged) {
			logged = now;
			XLS_LOG_INFO("mods: rules: unlimited ammo %s, unlimited grenades %s, secondary kind 0x%02x, ammo supply %d magazines (0 = default).", r.ammo ? "on" : "off", r.grenades ? "on" : "off", r.secondary, r.supply);
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
	// not grenades: their counter works the count out from this and shows 0 for -1 (no infinity there)
	if (!(raw & 0xff) && weapon && !Grenade(weapon) && g_ammo) {
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

// The grenade counter (left side) gets its count clamped to 0 or more by the HUD update, so it can't
// say "infinite" by count: the icons the HUD picks for grenades are noted, and with unlimited
// grenades a counter showing one of them gets the infinity style.
std::atomic<uint32_t> g_grenadeIcons[8];
using IconFn = uint32_t(__attribute__((thiscall)) *)(void*, uint8_t*, uint32_t, uint32_t, uint32_t);
IconFn g_iconOriginal = nullptr;
CounterFn g_leftCounterOriginal = nullptr;

uint32_t __attribute__((thiscall)) Icon(void* hud, uint8_t* weapon, uint32_t a, uint32_t b, uint32_t c)
{
	uint32_t icon = g_iconOriginal(hud, weapon, a, b, c);
	if ((uintptr_t)weapon >= 0x10000 && icon < 256 && Grenade(weapon) && !(g_grenadeIcons[icon / 32] & (1u << (icon % 32)))) {
		g_grenadeIcons[icon / 32] |= 1u << (icon % 32);
		XLS_LOG_INFO("mods: rules: grenade kind 0x%02x has HUD icon 0x%02x.", weapon[0x1e], icon);
	}
	return icon;
}

void __attribute__((thiscall)) LeftCounter(void* hud, uint32_t slot, uint32_t icon, int32_t count, uint32_t style, uint32_t counterOnly)
{
	uint8_t i = icon & 0xff;
	bool grenade = g_grenades && (g_grenadeIcons[i / 32] & (1u << (i % 32)));
	g_leftCounterOriginal(hud, slot, icon, count, grenade ? kStyleInfinite : style, counterOnly);
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

using PushRulesFn = void(__attribute__((thiscall)) *)(void*, uint8_t*);
using SetNumberFn = void(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t);
PushRulesFn g_pushRulesOriginal = nullptr;
SetNumberFn g_setNumber = nullptr;
const uint8_t kPushRulesPrologue[] = {0x83, 0xec, 0x20, 0x53, 0x55, 0x56}; // sub esp,0x20; push ebx; push ebp; push esi

void __attribute__((thiscall)) PushRules(void* screen, uint8_t* rules)
{
	g_pushRulesOriginal(screen, rules);
	if (!rules) {
		return;
	}
	for (int i = 0; i < kRows; i++) {
		uint8_t v = rules[kRowDefs[i].offset];
		if (v >= kRowDefs[i].values) {
			v = rules[kRowDefs[i].offset] = 0; // saved before these rows existed
		}
		g_menu[i] = v;
		g_setNumber(screen, kRowDefs[i].dataNo, v);
	}
	g_menuSeen = true;
}

bool PatchRowTable(uint8_t* table)
{
	DWORD old = 0;
	if (!VirtualProtect(table, 16 * 0x23, PAGE_READWRITE, &old)) {
		return false;
	}
	int patched = 0;
	for (int c = 0; c < kRowCategories && table[c * 0x23] != 0xff; c++) {
		for (int pageNo : {kPageSettings2, kPageSettings3}) {
			uint8_t* page = table + c * 0x23 + 2 + pageNo * 0xb;
			for (const Row& r : kRowDefs) {
				if (r.page == pageNo && page[0] < 10) {
					page[1 + page[0]++] = (uint8_t)r.offset;
				}
			}
		}
		patched++;
	}
	VirtualProtect(table, 16 * 0x23, old, &old);
	XLS_LOG_INFO("mods: rules: %d create-match categories got the rule rows.", patched);
	return patched > 0;
}

using SetupFn = void(__attribute__((thiscall)) *)(uint8_t*, uint32_t);
SetupFn g_setupOriginal = nullptr;
uint32_t g_dataAt = 0;
uintptr_t g_transfer = 0;
const uint8_t kSetupPrologue[] = {0x56, 0x8b, 0xf1, 0x8b, 0x0d}; // push esi; mov esi,ecx; mov ecx,[game] (9 bytes copied)
const size_t kSetupCopy = 9;
const int kDataEntries = 3;
const size_t kDataEntry = 0x54;
std::mutex g_supplyMutex;
std::map<uint8_t*, int16_t> g_startOriginal; // data entry -> its original start total
std::map<uint8_t*, std::pair<int, int>> g_pickupScale; // weapon -> new / original start total

void __attribute__((thiscall)) Setup(uint8_t* weapon, uint32_t arg)
{
	if (!weapon || Grenade(weapon)) {
		g_setupOriginal(weapon, arg);
		return;
	}
	int magazines = g_supply;
	uint8_t* data = *(uint8_t**)(weapon + g_dataAt);
	std::lock_guard<std::mutex> lock(g_supplyMutex);
	for (int k = 0; data && k < kDataEntries; k++) {
		uint8_t* e = data + k * kDataEntry;
		auto* start = (int16_t*)(e + 0x30);
		int16_t magazine = *(int16_t*)(e + 0x32), cap = *(int16_t*)(e + 0x34);
		auto it = g_startOriginal.find(e);
		if (it == g_startOriginal.end()) {
			if (magazines == 0) {
				continue;
			}
			it = g_startOriginal.emplace(e, *start).first;
		}
		if (cap < 0 || it->second <= 0 || magazine <= 0) {
			continue;
		}
		int want = magazines == 0 ? it->second : magazine * magazines;
		*start = (int16_t)(want > cap ? cap : want);
	}
	g_setupOriginal(weapon, arg);
	// the entry the weapon took is the one whose start it now holds
	g_pickupScale.erase(weapon);
	int16_t total = *(int16_t*)(weapon + g_magazine + 2);
	for (int k = 0; data && magazines != 0 && k < kDataEntries; k++) {
		uint8_t* e = data + k * kDataEntry;
		auto it = g_startOriginal.find(e);
		if (it != g_startOriginal.end() && it->second > 0 && *(int16_t*)(e + 0x30) == total) {
			g_pickupScale[weapon] = {total, it->second};
			break;
		}
	}
}

uint32_t __attribute__((thiscall)) AddAmmo(uint8_t* weapon, int16_t delta)
{
	// not ammo moved over from a picked-up weapon: that was given (and scaled) already
	if (delta > 0 && weapon && g_supply != 0 && !Grenade(weapon) && (uintptr_t)__builtin_return_address(0) != g_transfer) {
		std::lock_guard<std::mutex> lock(g_supplyMutex);
		auto it = g_pickupScale.find(weapon);
		if (it != g_pickupScale.end()) {
			int scaled = delta * it->second.first / it->second.second;
			delta = (int16_t)(scaled > 32767 ? 32767 : scaled < 1 ? 1 : scaled); // the game clamps to the cap
		}
	}
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
	uint8_t* left = base + (kRulesVas[build].leftCounter - 0x400000);
	uint8_t* icon = base + (kRulesVas[build].icon - 0x400000);
	if (memcmp(left, kLeftCounterPrologue, sizeof(kLeftCounterPrologue)) == 0 && memcmp(icon, kIconPrologue, sizeof(kIconPrologue)) == 0) {
		g_iconOriginal = (IconFn)Detour(icon, kIconPrologue, sizeof(kIconPrologue), (void*)&Icon);
		g_leftCounterOriginal = g_iconOriginal ? (CounterFn)Detour(left, kLeftCounterPrologue, sizeof(kLeftCounterPrologue), (void*)&LeftCounter) : nullptr;
	}
	if (!g_leftCounterOriginal) {
		XLS_LOG_WARN("mods: rules: grenade counter not hooked, no infinity sign for unlimited grenades.");
	}
	g_setNumber = (SetNumberFn)(uintptr_t)kRulesVas[build].setNumber;
	g_pushRulesOriginal = (PushRulesFn)Detour(base + (kRulesVas[build].pushRules - 0x400000), kPushRulesPrologue, sizeof(kPushRulesPrologue), (void*)&PushRules);
	if (g_pushRulesOriginal) {
		PatchRowTable(base + (kRulesVas[build].rowTable - 0x400000));
	}
	uint8_t* setup = base + (kRulesVas[build].setup - 0x400000);
	if (memcmp(setup, kSetupPrologue, sizeof(kSetupPrologue)) == 0) {
		uint8_t expect[kSetupCopy];
		memcpy(expect, setup, kSetupCopy);
		g_dataAt = kRulesVas[build].data;
		g_transfer = (uintptr_t)(base + (kRulesVas[build].transfer - 0x400000));
		g_setupOriginal = (SetupFn)Detour(setup, expect, kSetupCopy, (void*)&Setup);
	}
	if (!g_setupOriginal) {
		XLS_LOG_WARN("mods: rules: weapon setup not hooked, ammo supply only scales pickups.");
	}
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
	Bytes saved;
	if (ReadWholeFile(Cfg().modCacheDir + L"rules.txt", &saved)) {
		Rules r = Parse(std::string(saved.begin(), saved.end()));
		for (int i = 0; i < kSecondaryValues; i++) {
			g_menu[kRowSecondary] = kSecondaryKinds[i] == r.secondary ? i : (int)g_menu[kRowSecondary];
		}
		for (int i = 0; i < kSupplyValues; i++) {
			g_menu[kRowSupply] = kSupplyMagazines[i] == r.supply ? i : (int)g_menu[kRowSupply];
		}
		g_menu[kRowAmmo] = r.ammo;
		g_menu[kRowGrenades] = r.grenades;
		g_menuSeen = true;
	}
	MemberDataSet("rules", Format(Own()));
	HANDLE t = CreateThread(nullptr, 0, SyncThread, nullptr, 0, nullptr);
	if (t) {
		CloseHandle(t);
	}
	XLS_LOG_INFO("mods: rules: hooked (%s).", build == 0 ? "DX11" : "DX9");
}


int RulesStep(int rule, int key, uint8_t* value)
{
	for (int i = 0; i < kRows; i++) {
		if (rule != kRowDefs[i].offset || (key != 10 && key != 11)) {
			continue;
		}
		int n = kRowDefs[i].values;
		uint8_t was = *value % n;
		if (i == kRowSupply) { // least to most, no wrap
			int at = was + (key == 10 ? 1 : -1);
			*value = at < 0 || at >= n ? was : (uint8_t)at;
		}
		else {
			*value = (uint8_t)((was + (key == 10 ? 1 : n - 1)) % n);
		}
		g_menu[i] = *value;
		g_menuSeen = true;
		return *value != was;
	}
	return -1;
}

uint32_t RulesMessageAlias(uint32_t id)
{
	if (id >= (uint32_t)kMsgSecondary && id < (uint32_t)(kMsgSecondary + kSecondaryValues) && kSecondaryGameMsg[id - kMsgSecondary]) {
		return kSecondaryGameMsg[id - kMsgSecondary];
	}
	return id;
}

const char* RulesMessage(uint32_t id)
{
	if (id >= (uint32_t)kMsgSecondary && id < (uint32_t)(kMsgSecondary + kSecondaryValues)) {
		return kSecondaryNames[id - kMsgSecondary];
	}
	if (id >= (uint32_t)kMsgSupply && id < (uint32_t)(kMsgSupply + kSupplyValues)) {
		return kSupplyNames[id - kMsgSupply];
	}
	for (const Row& r : kRowDefs) {
		if (id == (uint32_t)(kMsgItemName + r.item)) {
			return r.label;
		}
	}
	return nullptr;
}

namespace {

// AVM1 bytecode for the rows (see the top of the file).
struct Avm1 {
	Bytes code;
	void Push(std::initializer_list<int> values)
	{
		Bytes p;
		for (int v : values) {
			p.push_back(7); // integer
			for (int k = 0; k < 4; k++) {
				p.push_back((uint8_t)(v >> (k * 8)));
			}
		}
		Action(0x96, p);
	}
	void Push(const char* text)
	{
		Bytes p{0}; // string
		p.insert(p.end(), text, text + strlen(text) + 1);
		Action(0x96, p);
	}
	void Action(uint8_t op, const Bytes& payload = Bytes())
	{
		code.push_back(op);
		if (op >= 0x80) {
			code.push_back((uint8_t)payload.size());
			code.push_back((uint8_t)(payload.size() >> 8));
			code.insert(code.end(), payload.begin(), payload.end());
		}
	}
};

Bytes RowsScript()
{
	const uint8_t kGetVariable = 0x1c, kGetMember = 0x4e, kSetMember = 0x4f, kCallMethod = 0x52, kPop = 0x17, kInitArray = 0x42;
	Avm1 a;
	const uint8_t kSubtract = 0x0b;
	auto pageArray = [&](int pageNo, int c) {
		a.Push("OnlineRule");
		a.Action(kGetVariable);
		a.Push(pageNo == kPageSettings2 ? "sub1ItemArray" : "sub2ItemArray");
		a.Action(kGetMember);
		a.Push({c});
		a.Action(kGetMember);
	};
	for (int c = 0; c < kRowCategories; c++) { // the same categories as PatchRowTable
		// Settings 2: sub1ItemArray[c].push(items...)
		int n = 0;
		for (int i = kRows - 1; i >= 0; i--) {
			if (kRowDefs[i].page == kPageSettings2) {
				a.Push({kRowDefs[i].item});
				n++;
			}
		}
		a.Push({n});
		pageArray(kPageSettings2, c);
		a.Push("push");
		a.Action(kCallMethod);
		a.Action(kPop);
		// Settings 3: sub2ItemArray[c].splice(length - 1, 0, items...) (before the cross-play row)
		n = 0;
		for (int i = kRows - 1; i >= 0; i--) {
			if (kRowDefs[i].page == kPageSettings3) {
				a.Push({kRowDefs[i].item});
				n++;
			}
		}
		a.Push({0});
		pageArray(kPageSettings3, c);
		a.Push("length");
		a.Action(kGetMember);
		a.Push({1});
		a.Action(kSubtract);
		a.Push({n + 2});
		pageArray(kPageSettings3, c);
		a.Push("splice");
		a.Action(kCallMethod);
		a.Action(kPop);
	}
	for (const Row& r : kRowDefs) {
		a.Push("OnlineRule");
		a.Action(kGetVariable);
		a.Push("offsetArray");
		a.Action(kGetMember);
		a.Push({r.item, r.dataNo, r.msgBase, 2});
		a.Action(kInitArray);
		a.Action(kSetMember);
	}
	a.code.push_back(0); // ActionEnd
	return a.code;
}

}

// Inserts a DoAction with RowsScript before the root timeline's first ShowFrame.
bool RulesPatchMenuFlash(std::vector<uint8_t>* gfx)
{
	Bytes& f = *gfx;
	if (f.size() < 16 || memcmp(f.data(), "GFX", 3) != 0) {
		return false;
	}
	size_t at = 8;
	int nbits = f[at] >> 3;
	at += (5 + 4 * nbits + 7) / 8 + 4; // frame rect, rate, count
	while (at + 2 <= f.size()) {
		uint16_t head = (uint16_t)(f[at] | f[at + 1] << 8);
		uint32_t code = head >> 6, length = head & 0x3f;
		size_t body = at + 2;
		if (length == 0x3f) {
			if (body + 4 > f.size()) {
				return false;
			}
			length = *(const uint32_t*)&f[body];
			body += 4;
		}
		if (code == 0) {
			return false;
		}
		if (code == 1) { // ShowFrame
			Bytes script = RowsScript();
			Bytes tag;
			uint16_t h = (uint16_t)(12 << 6 | 0x3f); // DoAction, long form
			tag.push_back((uint8_t)h);
			tag.push_back((uint8_t)(h >> 8));
			for (int k = 0; k < 4; k++) {
				tag.push_back((uint8_t)(script.size() >> (k * 8)));
			}
			tag.insert(tag.end(), script.begin(), script.end());
			f.insert(f.begin() + at, tag.begin(), tag.end());
			*(uint32_t*)&f[4] = (uint32_t)f.size();
			XLS_LOG_INFO("mods: rules: create-match rows added to the menu Flash (%zu bytes of script).", script.size());
			return true;
		}
		at = body + length;
	}
	return false;
}

}
