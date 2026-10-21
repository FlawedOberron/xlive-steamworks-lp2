#include "mods/lp2.h"
#include "mods/package.h"
#include "mods/parts.h"
#include "mods/flash/botrows.h"

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
#include <functional>
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
	uint32_t matchMgr;    // global -> the match manager (set by its constructor, DX11 FUN_00855f20)
	uint32_t matchSetup;  // global -> match setup: per-slot records at +0x148 (0x7c each, team first)
	uint32_t localSlot;   // thiscall(net) -> the local player's match slot (DX11 FUN_008029c0)
	uint32_t net;         // global -> the object it takes
	uint32_t ctlUpdate;   // player controller update, thiscall(controller) (DX11 FUN_008ff650; see ControlUpdate)
	uint32_t controllers; // the controllers by match slot (16 pointers)
	uint32_t follow;      // camera follow: thiscall(camera, body, 0xff, float, float) (DX11 FUN_008e5e30)
	uint32_t followArg;   // the float the respawn passes it
	uint32_t endCheck;    // the host's match end check, fastcall(flow) -> bool (DX11 FUN_01008ef0)
	uint32_t results;     // global -> the match results (its prologue reads it)
	uint32_t ranking;     // the end ranking: thiscall(flow, bool) (DX11 FUN_01009b90; see Ranking)
	uint32_t players;     // global -> the players: +0x28 one per slot, +8 bit 0 in play
	uint32_t botCursor;   // training menu: AI Enemy Battle up / down, fastcall(menu) (DX11 FUN_00f44680)
	uint32_t botStep;     // training menu: AI Enemy Battle left / right, fastcall(menu) (DX11 FUN_00f44760)
	uint32_t flashCall;   // a menu's Flash call: thiscall(menu, 0, name, args, count) (DX11 FUN_00f4a390)
	uint32_t input;       // global -> the menu input: +0x330 pressed (up 0x40, down 0x80, left 0x100, right 0x200)
	uint32_t modeStart;   // offline mode start, thiscall(launch, mode) (DX11 FUN_00873b80): AI Enemy Battle adds slots 1..15 as AI
	uint32_t botLoop;     // its `cmp bl,0x10` closing that loop
	uint32_t botPlayers;  // its `mov byte [players+0x254],0x10`
	uint32_t botCount;    // its `mov dword [players+0x50188],0xf`
	uint32_t menuHelp;    // a training menu's bottom bar text: thiscall(menu, MsgCinesco id) (DX11 FUN_00f45eb0)
	uint32_t resultsSkip; // the results screen's `je` skipping its slot-present check offline (74 13)
	uint32_t status;      // global -> the game status: +0x14594 flag bits (0x80000000 >> id), 0x4000000 = cockpit off
};
const RulesVas kRulesVas[] = {
	{0xc7aa20, 0xc7a820, 0xeca930, 0x20fc, 0x8fb130, 0xe8c270, 0xf4a330, 0x17e9840, 0xc86160, 0x2180, 0xc7cf15, 0xecac50, 0xed1e00, 0x18e086c, 0x1852850, 0x8029c0, 0x1852144, 0x8ff650, 0x1852080, 0x8e5e30, 0x16f7c6c, 0x1008ef0, 0x1852238, 0x1009b90, 0x18520c0, 0xf44680, 0xf44760, 0xf4a390, 0x18e0860, 0x873b80, 0x873df6, 0x873d1f, 0x873d5d, 0xf45eb0, 0xeaa1ba, 0x18d08ac}, // DX11
	{0xc6ae10, 0xc6ac10, 0xebab30, 0x20ec, 0x8eb7d0, 0xe7c1e0, 0xf3a600, 0x17d97e0, 0xc76550, 0x2170, 0xc6d305, 0xebae50, 0xec1ff0, 0x18ce780, 0x18427b0, 0x7f3b00, 0x18420a4, 0x8efcf0, 0x1841fe0, 0x8d64d0, 0x16e7098, 0xff8b10, 0x1842198, 0xff97b0, 0x1842020, 0xf34940, 0xf34a20, 0xf3a660, 0x18ce774, 0x8644d0, 0x864746, 0x86466f, 0x8646ad, 0xf36170, 0xe9a44a, 0x18c080c}, // DX9
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

// Survival: a sub-type of Elimination / Team Elimination shown between Kills and Points. The game
// keeps Kills (sub 1, so the category and its rows are Kills') and rules byte 0x32 says survival. The
// sub-type row's label reads our display value (DataNo 79) from our texts (kMsgSubElim / kMsgSubTeam
// + display), which repeat the game's with Survival third.
const int kRuleMain = 0x16, kRuleSub = 0x17, kRuleSurvival = 0x32;
const int kSubKills = 1, kSubPoints = 2;
const int kDataNoSubShown = 79;
const int kItemSubElim = 1, kItemSubTeam = 2; // OnlineRule.ITEM_SCATG_SURV / _TSURV
const int kMsgSubElim = 630, kMsgSubTeam = 640;
const int kSubElimGameMsg[] = {79, 80, 0, 81, 82};     // Battle Gauge, Kills, (Survival), Points, Random
const int kSubTeamGameMsg[] = {83, 84, 0, 85, 86, 87}; // ..., Points, VS Annihilator, Random
const char* const kSurvivalName = "Survival";
// Respawns (Survival) take the Kills row: its label reads our display value (DataNo 80) from our texts
// kMsgKillRow + display: 0-5 the game's kill targets (MsgOnline 96-101), then kLives. The lives index
// is rules byte 0x33; in Survival the game's kill target (byte 9) is kept at infinity (5) so kills
// never end the match.
const int kRuleKillTarget = 9, kKillTargetInfinite = 5, kRuleLives = 0x33;
const int kItemKillRow = 5; // OnlineRule.ITEM_SUB_KILL
const int kDataNoKillShown = 80;
const int kMsgKillRow = 650;
const int kKillTargets = 6;
// shown as respawns (lives - 1)
const int kLives[] = {1, 2, 3, 4, 5, 6, 11, 21, 31, 41, 51};
const char* const kLivesNames[] = {"0", "1", "2", "3", "4", "5", "10", "20", "30", "40", "50"};
const int kLivesValues = (int)(sizeof(kLives) / sizeof(kLives[0]));

bool SurvivalOn(const uint8_t* rules)
{
	return rules[kRuleMain] <= 1 && rules[kRuleSub] == kSubKills && rules[kRuleSurvival] == 1;
}

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
	bool survival = false;
	int lives = 0; // Survival lives per player (Elimination) or team (Team Elimination)
	bool team = false;
};
const int kSlotSecond = 7;

std::mutex g_mutex;
std::map<uint64_t, Rules> g_memberRules;
std::atomic<bool> g_ammo{false};
std::atomic<bool> g_grenades{false};
std::atomic<int> g_secondary{0};
std::atomic<int> g_supply{0};
std::atomic<bool> g_survival{false};
std::atomic<bool> g_offline{true}; // not in a lobby (training): the bots run on this PC
// Training's AI Enemy Battle rows (see BotCursor): armed while that screen was the last one used
std::atomic<bool> g_botArmed{false}, g_botSurvival{false};
std::atomic<int> g_botLives{3}, g_botRule{0}; // index into kLives; the screen's rule (0 Elimination, 1 Team Elimination)
std::atomic<int> g_botCount{15};
std::atomic<bool> g_menuSurvival{false};
std::atomic<int> g_lives{0};
std::atomic<int> g_menuLives{0};
std::atomic<bool> g_team{false};
std::atomic<bool> g_menuTeam{false};
uint32_t g_magazine = 0;

std::string Format(const Rules& r)
{
	return FormatA("ammo=%d;grenades=%d;secondary=%d;magazines=%d;survival=%d;lives=%d;team=%d", r.ammo, r.grenades, r.secondary, r.supply, r.survival, r.lives, r.team);
}

Rules Parse(const std::string& s)
{
	Rules r;
	r.ammo = s.find("ammo=1") != std::string::npos;
	r.grenades = s.find("grenades=1") != std::string::npos;
	size_t at = s.find("secondary=");
	r.secondary = at == std::string::npos ? 0 : atoi(s.c_str() + at + 10) & 0xff;
	r.survival = s.find("survival=1") != std::string::npos;
	r.team = s.find("team=1") != std::string::npos;
	size_t livesAt = s.find("lives=");
	r.lives = livesAt == std::string::npos ? 0 : atoi(s.c_str() + livesAt + 6);
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
		r.survival = g_menuSurvival;
		r.lives = r.survival ? g_menuLives.load() : 0;
		r.team = r.survival && g_menuTeam;
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
		g_offline = !lobby.IsValid();
		if (lobby.IsValid() && SteamReady() && SteamMatchmaking()) {
			uint64_t host = LobbyOwner(SteamMatchmaking(), lobby).ConvertToUint64();
			std::lock_guard<std::mutex> lock(g_mutex);
			auto it = g_memberRules.find(host);
			if (host != SteamLocalId().ConvertToUint64() && it != g_memberRules.end()) {
				r = it->second;
			}
		}
		if (g_offline && g_botArmed && g_botSurvival && g_botRule <= 1) { // a training bot match set to Survival
			r.survival = true;
			r.lives = kLives[g_botLives % kLivesValues];
			r.team = g_botRule == 1;
		}
		// Test override for fixed-rule modes (training bot matches): ModCache\survival-test.txt = "respawns=N;team=0|1"
		Bytes test;
		if (ReadFileBytes(Cfg().modCacheDir + L"survival-test.txt", test)) {
			std::string s(test.begin(), test.end());
			size_t at = s.find("respawns=");
			r.survival = true;
			r.lives = (at == std::string::npos ? 0 : atoi(s.c_str() + at + 9)) + 1;
			r.team = s.find("team=1") != std::string::npos;
		}
		g_ammo = r.ammo;
		g_grenades = r.grenades;
		g_secondary = r.secondary;
		g_supply = r.supply;
		g_survival = r.survival;
		g_lives = r.lives;
		g_team = r.team;
		std::string now = Format(r);
		if (now != logged) {
			logged = now;
			XLS_LOG_INFO("mods: rules: unlimited ammo %s, unlimited grenades %s, secondary kind 0x%02x, ammo supply %d magazines (0 = default), survival %s (%d lives).", r.ammo ? "on" : "off", r.grenades ? "on" : "off", r.secondary, r.supply, r.survival ? "on" : "off", r.lives);
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
using LocalSlotFn = int(__attribute__((thiscall)) *)(void*);
LocalSlotFn g_localSlot = nullptr;
void** g_net = nullptr;
std::atomic<int> g_localIndex{-1}; // the local player's match slot
std::atomic<bool> g_out{false};     // Survival: the local player has no lives left

// Survival: the match manager (global, DX11 0x18e086c) holds the match info owner at +0x144b4, whose
// cInfoGame (+0x3e0) has mNetWork.dieCountPerson (+0x35c, s32 x 16 by match slot) and dieCountTeam
// (+0x39c), synced to every PC; and the session info at +0x144f4 with isRespawn (+0x88, the stock
// Respawn rule). A player out of lives gets isRespawn cleared on its own PC (each PC runs its player).
uint8_t*** g_matchMgr = nullptr;
uint8_t** g_matchSetup = nullptr;
const uint32_t kSetupSlots = 0x148, kSetupSlot = 0x7c;
const uint32_t kMgrInfoOwner = 0x144b4, kOwnerInfoGame = 0x3e0;
const uint32_t kInfoDiePerson = 0x35c, kInfoDieTeam = 0x39c;

// The controller's death states (byte +0x19): 1 death view (timers +0xfc/+0x100 down to 2), 2 -> 3
// respawn countdown (float +0x104, the respawn dialog, which takes the camera), 4 waiting for the
// respawn point pick (talks to the host: holding it there drops the connection), 5 respawn (spawns a
// new character through Spawn). A local player out of lives is held in the death view (1). Match slot
// at +0xb0 (the slot's entry in the controllers is a different object).
using ControlUpdateFn = void(__attribute__((thiscall)) *)(uint8_t*);
ControlUpdateFn g_ctlUpdateOriginal = nullptr;
const uint8_t kCtlUpdatePrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x81, 0xec, 0xa4, 0x00, 0x00, 0x00};
const int kCtlState = 0x19, kCtlSlot = 0xb0, kCtlDeathTimers = 0xfc, kStateDeath = 1, kStateRespawn = 5;

// Spectating: controller +0x480 is the player (active when its +4 & 7 is 1 or 2), whose +0x40 is
// the body the camera follows; +0x484 the camera (only the local player's), following the body at
// +0x200 once told through follow (as the respawn does).
using FollowFn = void(__attribute__((thiscall)) *)(uint8_t*, uint8_t*, int, float, float);
FollowFn g_follow = nullptr;
const float* g_followArg = nullptr;
uint8_t** g_controllers = nullptr;
const uint32_t kCtlPlayer = 0x480, kCtlCamera = 0x484, kPlayerBody = 0x40, kCameraBody = 0x200;
int g_watched = -1;
uint8_t g_slotState[16]; // each slot's controller state, as its update left it

uint8_t* Body(int slot)
{
	uint8_t* ctl = g_controllers[slot];
	uint8_t* player = ctl ? *(uint8_t**)(ctl + kCtlPlayer) : nullptr;
	uint32_t kind = player ? *(uint32_t*)(player + 4) & 7 : 0;
	return kind == 1 || kind == 2 ? *(uint8_t**)(player + kPlayerBody) : nullptr;
}

// A slot worth watching: in play, alive (state 0) and, in Survival, with lives left (its team's in Team Survival).
int32_t SlotTeam(int slot)
{
	uint8_t* setup = *g_matchSetup;
	return setup && slot >= 0 && slot < 16 ? *(int32_t*)(setup + kSetupSlots + slot * kSetupSlot) : -1;
}

// Survival: the slot has no respawns left (its team none, in Team Survival).
bool SlotOut(int slot)
{
	uint8_t *mgr = *g_matchMgr ? (uint8_t*)*g_matchMgr : nullptr, *owner = mgr ? *(uint8_t**)(mgr + kMgrInfoOwner) : nullptr;
	if (!g_survival || g_lives <= 0 || !owner || slot < 0 || slot >= 16) {
		return false;
	}
	if (!g_team) {
		return ((int32_t*)(owner + kOwnerInfoGame + kInfoDiePerson))[slot] >= g_lives;
	}
	int32_t team = SlotTeam(slot);
	return team >= 0 && team < 16 && ((int32_t*)(owner + kOwnerInfoGame + kInfoDieTeam))[team] >= g_lives;
}

// A slot worth watching: in play, alive (state 0), not out and, in Team Survival, on our team.
bool Watchable(int slot, int self)
{
	return slot != self && Body(slot) && g_slotState[slot] == 0 && !SlotOut(slot) && (!g_team || SlotTeam(slot) == SlotTeam(self));
}

// The next watchable slot after (dir 1) or before (dir -1) the watched one, -1 for none.
int NextWatchable(int self, int dir)
{
	int from = g_watched < 0 ? self : g_watched;
	for (int i = 1; i <= 16; i++) {
		int slot = (from + dir * i + 32) % 16;
		if (Watchable(slot, self)) {
			return slot;
		}
	}
	return -1;
}

// The slot's entry is its pad too: actions held at +0x3b8 (fire 0x100 watches the next player, zoom 0x100000 the previous).
const uint32_t kPadHeld = 0x3b8;
const uint64_t kActFire = 0x100, kActZoom = 0x100000;
uint64_t g_padWas = 0;

void Spectate(uint8_t* entry, int self, bool force)
{
	uint8_t* cam = entry ? *(uint8_t**)(entry + kCtlCamera) : nullptr;
	if (!cam || !g_follow) {
		return;
	}
	uint64_t pad = *(uint64_t*)(entry + kPadHeld), pressed = pad & ~g_padWas;
	g_padWas = pad;
	int dir = pressed & kActFire ? 1 : pressed & kActZoom ? -1 : 0;
	if (dir || g_watched < 0 || !Watchable(g_watched, self)) {
		int next = NextWatchable(self, dir ? dir : 1);
		if (next < 0 && g_watched >= 0 && Body(g_watched)) {
			next = g_watched; // the only one left, or the last one standing: stay on them
		}
		if (next != g_watched) {
			XLS_LOG_INFO("mods: rules: survival: spectating player %d.", next);
		}
		g_watched = next;
	}
	uint8_t* body = g_watched >= 0 ? Body(g_watched) : nullptr;
	if (body && (force || *(uint8_t**)(cam + kCameraBody) != body)) { // forced after a held respawn: its pick view takes the camera
		g_follow(cam, body, 0xff, *g_followArg, 0.0f);
	}
}

// An out player's body stays where it fell (the respawn is what removes it, as below), and the bots
// keep shooting at it: a few seconds into the hold it is removed the respawn's way (DX11 0x8ffedf).
// Not while a camera still follows it (the spectator moves on first). The controller is also the
// slot entry's player (+0x480), whose body the same +0x40 is.
const uint32_t kCtlBody = 0x40, kBodyRemoveSlot = 0x30 / 4;
const DWORD kBodyRemoveDelay = 4000;
DWORD g_outSince[16];

void RemoveBody(uint8_t* ctl)
{
	int slot = ctl[kCtlSlot];
	uint8_t* body = *(uint8_t**)(ctl + kCtlBody);
	if (slot >= 16 || !body || ctl[kCtlState] != kStateDeath) {
		return;
	}
	DWORD now = GetTickCount();
	if (!g_outSince[slot]) {
		g_outSince[slot] = now;
	}
	if (now - g_outSince[slot] < kBodyRemoveDelay || !g_controllers[slot] || *(uint8_t**)(g_controllers[slot] + kCtlPlayer) != ctl) {
		return;
	}
	for (int i = 0; i < 16; i++) {
		uint8_t* cam = g_controllers[i] ? *(uint8_t**)(g_controllers[i] + kCtlCamera) : nullptr;
		if (cam && *(uint8_t**)(cam + kCameraBody) == body) {
			return;
		}
	}
	using RemoveFn = void(__attribute__((thiscall)) *)(uint8_t*);
	((RemoveFn)(*(void***)body)[kBodyRemoveSlot])(body);
	*(uint8_t**)(ctl + kCtlBody) = nullptr;
	XLS_LOG_INFO("mods: rules: survival: player %d is out, body removed.", slot);
}

// The cockpit (HUD) is off while the local player spectates: the game's own status flag (events set it;
// the HUD update DX11 0xee1063 skips the cockpit while it is on). Only ever cleared if we set it; the
// survival thread clears it too once nobody spectates (match over, quit).
uint8_t** g_status = nullptr;
const uint32_t kStatusFlags = 0x14594, kStatusCockpitOff = 0x4000000;
std::atomic<bool> g_hudHidden{false};
std::atomic<DWORD> g_hudTouched{0};

void HideHud(bool hide)
{
	LONG* flags = g_status && *g_status ? (LONG*)(*g_status + kStatusFlags) : nullptr;
	if (!flags || hide == g_hudHidden) {
		if (hide) {
			g_hudTouched = GetTickCount();
		}
		return;
	}
	if (hide && (*flags & kStatusCockpitOff)) {
		return; // already off (an event): not ours to clear
	}
	if (hide) {
		InterlockedOr(flags, kStatusCockpitOff);
		g_hudTouched = GetTickCount();
	} else {
		InterlockedAnd(flags, ~(LONG)kStatusCockpitOff);
	}
	g_hudHidden = hide;
	XLS_LOG_INFO("mods: rules: survival: HUD %s.", hide ? "hidden while spectating" : "back");
}

void __attribute__((thiscall)) ControlUpdate(uint8_t* ctl)
{
	if (g_localSlot && *g_net) { // on the game thread, where the game asks it too
		int slot = g_localSlot(*g_net);
		for (int i = 0; i < 16 && slot < 0; i++) { // offline (training) it answers -1: the one with a camera
			slot = g_controllers[i] && *(uint8_t**)(g_controllers[i] + kCtlCamera) ? i : -1;
		}
		g_localIndex = slot;
	}
	int self = g_localIndex;
	// each PC holds its own player; offline (training) the bots run here too
	bool local = ctl && self >= 0 && self < 16 && ctl[kCtlSlot] == self, hold = ctl && (local || g_offline) && SlotOut(ctl[kCtlSlot]);
	uint8_t was = ctl ? ctl[kCtlState] : 0;
	if (hold && was == kStateDeath) {
		float* timers = (float*)(ctl + kCtlDeathTimers);
		timers[0] = timers[0] < 1.0f ? 1.0f : timers[0];
		timers[1] = timers[1] < 1.0f ? 1.0f : timers[1];
	}
	g_ctlUpdateOriginal(ctl);
	if (ctl && ctl[kCtlSlot] < 16) {
		g_slotState[ctl[kCtlSlot]] = ctl[kCtlState];
	}
	if (!hold) {
		if (local) {
			g_watched = -1;
			HideHud(false);
		}
		if (ctl && ctl[kCtlSlot] < 16) {
			g_outSince[ctl[kCtlSlot]] = 0;
		}
		return;
	}
	// leaving the death view (1) for the countdown, or the death state (0) straight for the respawn (5)
	bool held = (ctl[kCtlState] > kStateDeath && ctl[kCtlState] <= kStateRespawn) && (was != 0 || ctl[kCtlState] == kStateRespawn);
	if (held) {
		ctl[kCtlState] = kStateDeath;
		((float*)(ctl + kCtlDeathTimers))[0] = 1.0f;
		((float*)(ctl + kCtlDeathTimers))[1] = 1.0f;
		static DWORD logged = 0;
		if (local && GetTickCount() - logged > 5000) {
			logged = GetTickCount();
			XLS_LOG_INFO("mods: rules: survival: player %d respawn held (state %u -> %u, no respawns left).", self, was, kStateDeath);
		}
	}
	if (local) {
		Spectate(g_controllers[self], self, held);
		HideHud(true);
	}
	RemoveBody(ctl);
}

// Survival's end. The match flow (DX11 FUN_0100e1a0) asks the host's end check every frame (kill
// target, time up); once it answers true the flow ranks the players (or teams), which the result
// screen's victory / defeat follows, and sets isGameFinished for every PC. The check also ends the
// match once one player (one team) with respawns is left; the ranking puts respawns left first.
using EndCheckFn = bool(__attribute__((fastcall)) *)(void*);
EndCheckFn g_endCheckOriginal = nullptr;
using RankingFn = void(__attribute__((thiscall)) *)(uint8_t*, int);
RankingFn g_rankingOriginal = nullptr;
const uint32_t kSetupRule = 0x28, kSetupPlayers = 0x70, kSetupSlotLeft = 0x6a; // +0x1b2 of a slot record: not in the ranking
uint8_t** g_players = nullptr;
const uint32_t kPlayersSlots = 0x28, kPlayerInPlay = 8; // bit 0
const uint32_t kNetHostSlot = 0xa1f8, kNetLocalSlot = 0xa1fc;
const int kRuleTraining = 1, kRuleElimination = 2, kRuleTeamElimination = 5; // 1: the training bot match, ranked by slot like 2
const uint32_t kRankList = 0x74, kRankEntry = 0xc, kRankCount = 0x134; // {rank, slot or team, score}

bool InPlay(int slot)
{
	uint8_t* setup = *g_matchSetup;
	uint8_t* players = *g_players;
	uint8_t* player = players ? ((uint8_t**)(players + kPlayersSlots))[slot] : nullptr;
	return setup && player && (player[kPlayerInPlay] & 1) && !setup[kSetupSlots + slot * kSetupSlot + kSetupSlotLeft];
}

bool Host()
{
	uint8_t* net = (uint8_t*)*g_net;
	int32_t host = net ? *(int32_t*)(net + kNetHostSlot) : 0;
	return g_offline || (net && (host == -1 || host == *(int32_t*)(net + kNetLocalSlot)));
}

// Respawns left (deaths still allowed) for a slot, or a team in Team Survival.
int LivesLeft(int id, bool team)
{
	uint8_t *mgr = *g_matchMgr ? (uint8_t*)*g_matchMgr : nullptr, *owner = mgr ? *(uint8_t**)(mgr + kMgrInfoOwner) : nullptr;
	if (!owner || id < 0 || id >= 16) {
		return 0;
	}
	int32_t deaths = ((int32_t*)(owner + kOwnerInfoGame + (team ? kInfoDieTeam : kInfoDiePerson)))[id];
	return g_lives - deaths > 0 ? g_lives - deaths : 0;
}

bool __attribute__((fastcall)) EndCheck(void* flow)
{
	if (g_endCheckOriginal(flow)) {
		return true;
	}
	uint8_t* setup = *g_matchSetup;
	if (!g_survival || g_lives <= 0 || !setup || !Host()) {
		return false;
	}
	int sides = 0, standing = 0;
	bool seen[16] = {}, up[16] = {};
	for (int slot = 0; slot < 16; slot++) {
		if (!InPlay(slot)) {
			continue;
		}
		int side = g_team ? SlotTeam(slot) : slot;
		if (side < 0 || side >= 16) {
			continue;
		}
		sides += !seen[side];
		seen[side] = true;
		if (!up[side] && !SlotOut(slot)) {
			up[side] = true;
			standing++;
		}
	}
	if (sides < 2 || standing > 1) {
		return false;
	}
	XLS_LOG_INFO("mods: rules: survival: %d of %d %s standing, match over.", standing, sides, g_team ? "teams" : "players");
	return true;
}

void __attribute__((thiscall)) Ranking(uint8_t* flow, int arg)
{
	g_rankingOriginal(flow, arg);
	uint8_t* setup = *g_matchSetup;
	int32_t rule = setup ? *(int32_t*)(setup + kSetupRule) : 0;
	static DWORD logged = 0;
	bool log = GetTickCount() - logged > 10000;
	if (log && g_survival) {
		logged = GetTickCount();
		XLS_LOG_INFO("mods: rules: survival: ranking (rule %d, %d entries).", rule, *(int32_t*)(flow + kRankCount));
	}
	if (!g_survival || g_lives <= 0 || (rule != kRuleTraining && rule != kRuleElimination && rule != kRuleTeamElimination)) {
		return;
	}
	bool team = rule == kRuleTeamElimination;
	int32_t (*list)[3] = (int32_t(*)[3])(flow + kRankList);
	int count = *(int32_t*)(flow + kRankCount);
	count = count < 0 ? 0 : count > 16 ? 16 : count;
	// respawns left first, the score breaks ties (encoded in the score the ranks are told apart by)
	for (int i = 0; i < count; i++) {
		list[i][2] = LivesLeft(list[i][1], team) * 100000 + (list[i][2] < 100000 ? list[i][2] : 99999);
	}
	for (int i = 1; i < count; i++) {
		for (int j = i; j > 0 && list[j][2] > list[j - 1][2]; j--) {
			std::swap(list[j], list[j - 1]);
		}
	}
	if (log && count) {
		XLS_LOG_INFO("mods: rules: survival: ranked first %s %d (%d respawns left, score %d).", team ? "team" : "player", list[0][1], list[0][2] / 100000, list[0][2] % 100000);
	}
}

// Training's AI Enemy Battle screen (flash\training\id300: Map and Game Type rows, mc_BOT) gets two
// rows cloned by our script (flash/botrows.as): Survival (Elimination -> Survival, Team Elimination ->
// Team Survival) and Respawns. The menu (state +0x190 1, phase +0x194 2 for this screen) keeps the row
// at +0x1b0 and the rule at +0x198; the hooks take the cursor over 4 rows and step our two.
using MenuFn = void(__attribute__((fastcall)) *)(uint8_t*);
MenuFn g_botCursorOriginal = nullptr, g_botStepOriginal = nullptr;
struct FlashArg {
	uint8_t flag;
	uint32_t type; // 3 number (float), 4 string
	const char* text;
};
using FlashCallFn = bool(__attribute__((thiscall)) *)(uint8_t*, int, const char*, FlashArg*, int);
FlashCallFn g_flashCall = nullptr;
uint8_t** g_input = nullptr;
const uint32_t kMenuState = 0x190, kMenuPhase = 0x194, kMenuRule = 0x198, kMenuMode = 0x1a0, kMenuRow = 0x1b0, kMenuFlags = 0x1d4;
const uint32_t kMenuMouseOn = 0x168, kMenuMouse = 0x154, kMenuClick = 0x15c, kInputPressed = 0x330;
const int kPhaseBot = 2, kModeBusy = 0x3e5, kMouseBotRow = 0x22e, kMouseArrowL = 10, kMouseArrowR = 11;
const uint32_t kKeyUp = 0x40, kKeyDown = 0x80, kKeyLeft = 0x100, kKeyRight = 0x200, kFlagRow = 0x80;

bool BotScreen(uint8_t* menu)
{
	return *(int32_t*)(menu + kMenuState) == 1 && *(int32_t*)(menu + kMenuPhase) == kPhaseBot && *(int32_t*)(menu + kMenuMode) != kModeBusy;
}

// The rows by their item number (the game's Map 0 and Game Type 1, our Survival 2, Respawns 3,
// Bots 4) in screen order, the shown ones only: Survival for the elimination rules, Respawns with
// Survival on.
const int kRowBots = 4;
int BotRowsShown(uint8_t* menu, int* rows)
{
	int n = 0;
	rows[n++] = kRowBots;
	rows[n++] = 0;
	rows[n++] = 1;
	if (*(int32_t*)(menu + kMenuRule) <= 1) {
		rows[n++] = 2;
		if (g_botSurvival) {
			rows[n++] = 3;
		}
	}
	return n;
}

void BotRowsDraw(uint8_t* menu)
{
	g_botRule = *(int32_t*)(menu + kMenuRule);
	int rows[5], shown = BotRowsShown(menu, rows);
	std::string bots = std::to_string(g_botCount.load());
	FlashArg args[8] = {{0, 4, "SURVIVAL"}, {0, 4, g_botSurvival ? "On" : "Off"}, {0, 4, "RESPAWNS"}, {0, 4, kLivesNames[g_botLives % kLivesValues]},
	                    {0, 4, shown > 3 ? "1" : "0"}, {0, 4, shown > 4 ? "1" : "0"}, {0, 4, "BOTS"}, {0, 4, bots.c_str()}};
	if (!g_flashCall || !g_flashCall(menu, 0, "lp2BotRows", args, 8)) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			XLS_LOG_WARN("mods: rules: training: the menu Flash has no lp2BotRows (rows not added).");
		}
	}
}

void __attribute__((fastcall)) BotCursorBody(uint8_t* menu)
{
	int32_t state = *(int32_t*)(menu + kMenuState);
	if (state == 0 || state == 1) { // back on the top menu, or a training screen: armed only on ours
		g_botArmed = state == 1 && *(int32_t*)(menu + kMenuPhase) == kPhaseBot;
	}
	static uint8_t* drawn = nullptr;
	static int drawnRule = -1;
	if (state != 1 || *(int32_t*)(menu + kMenuPhase) != kPhaseBot) { // left the screen (not just busy)
		drawn = nullptr;
		return;
	}
	if (!BotScreen(menu) || !*g_input) {
		return;
	}
	int32_t& row = *(int32_t*)(menu + kMenuRow);
	if (drawn != menu) { // shown: the cursor starts on the top row
		row = kRowBots;
		*(uint32_t*)(menu + kMenuFlags) |= kFlagRow;
	}
	if (drawn != menu || drawnRule != *(int32_t*)(menu + kMenuRule)) { // shown, or the rule changed
		drawn = menu;
		drawnRule = *(int32_t*)(menu + kMenuRule);
		BotRowsDraw(menu);
	}
	uint32_t pressed = *(uint32_t*)(*g_input + kInputPressed);
	int rows[5], shown = BotRowsShown(menu, rows), at = shown - 1; // a hidden row: the last shown
	for (int i = 0; i < shown; i++) {
		if (rows[i] == row) {
			at = i;
		}
	}
	int was = row;
	if (pressed & kKeyUp) {
		at = at == 0 ? shown - 1 : at - 1;
	} else if (pressed & kKeyDown) {
		at = at + 1 >= shown ? 0 : at + 1;
	}
	row = rows[at];
	int mouse = *(int32_t*)(menu + kMenuMouse) - kMouseBotRow;
	for (int i = 0; menu[kMenuMouseOn] && i < shown; i++) {
		if (rows[i] == mouse) {
			row = mouse;
		}
	}
	if (row != was || (pressed & (kKeyUp | kKeyDown))) {
		*(uint32_t*)(menu + kMenuFlags) |= kFlagRow;
	}
}

// The game's caller passes the menu in ecx on to the next handler (f34a20 / f44760) without
// reloading it, so the replacement must keep ecx (and edx) as the original does.
extern "C" void __attribute__((naked)) BotCursor()
{
	asm volatile("push %%ecx\n\tpush %%edx\n\tcall %P0\n\tpop %%edx\n\tpop %%ecx\n\tret" ::"i"(BotCursorBody));
}

void __attribute__((fastcall)) BotStep(uint8_t* menu)
{
	if (!BotScreen(menu) || !*g_input || *(int32_t*)(menu + kMenuRow) < 2) {
		g_botStepOriginal(menu);
		return;
	}
	uint32_t pressed = *(uint32_t*)(*g_input + kInputPressed);
	bool click = menu[kMenuClick] & 1;
	int mouse = *(int32_t*)(menu + kMenuMouse);
	int dir = (pressed & kKeyRight) || (click && mouse == kMouseArrowR) ? 1 : (pressed & kKeyLeft) || (click && mouse == kMouseArrowL) ? -1 : 0;
	if (!dir) {
		return;
	}
	if (*(int32_t*)(menu + kMenuRow) == kRowBots) {
		int at = g_botCount + dir;
		if (at < 1 || at > 15) {
			return;
		}
		g_botCount = at;
	} else if (*(int32_t*)(menu + kMenuRow) == 2) {
		if (*(int32_t*)(menu + kMenuRule) > 1) {
			return; // Survival only for the elimination rules
		}
		g_botSurvival = !g_botSurvival;
	} else {
		int at = g_botLives + dir;
		if (at < 0 || at >= kLivesValues) {
			return;
		}
		g_botLives = at;
	}
	BotRowsDraw(menu);
	XLS_LOG_INFO("mods: rules: training: %d bots, survival %s, %s respawns.", g_botCount.load(), g_botSurvival ? "on" : "off", kLivesNames[g_botLives % kLivesValues]);
}

// The bottom bar text: AI Enemy Battle asks for MsgCinesco 0x106 + row, so our rows get ours
// (the Bots row's would be DUMMY).
using MenuHelpFn = void(__attribute__((thiscall)) *)(uint8_t*, int);
MenuHelpFn g_menuHelpOriginal = nullptr;
const int kHelpBotRow = 0x106;

extern "C" void __attribute__((thiscall)) MenuHelpBody(uint8_t* menu, int id)
{
	static const char* const kHelp[] = {"Limit respawns: the last player or team standing wins.", "How many times each player can respawn.",
	                                    "How many AI enemies join the battle (1 to 15)."};
	int row = id - kHelpBotRow;
	if (!BotScreen(menu) || row < 2 || row > kRowBots) {
		g_menuHelpOriginal(menu, id);
		return;
	}
	if (menu[0x1d0]) { // as the original: only while the bar is up
		FlashArg arg = {0, 4, kHelp[row - 2]};
		g_flashCall(menu, 0, "_root.mc_CINESCO.cinescoMsg", &arg, 1);
	}
}

// Its callers keep the menu in ecx across the call, so the replacement keeps ecx and edx.
extern "C" void __attribute__((naked)) MenuHelp()
{
	asm volatile("push %%ecx\n\tpush %%edx\n\tpushl 12(%%esp)\n\tcall %P0\n\tpop %%edx\n\tpop %%ecx\n\tret $4" ::"i"(MenuHelpBody));
}

// Offline mode start: AI Enemy Battle (modes 51..75, 5 maps x 5 rules) fills slots 1..15 with AI
// in a loop, and sets the player and AI counts to 16 and 15. Their immediates take the Bots row's
// count for a match from that screen, and the game's own otherwise.
using ModeStartFn = uint32_t(__attribute__((thiscall)) *)(void*, int);
ModeStartFn g_modeStartOriginal = nullptr;
uint8_t *g_botLoop = nullptr, *g_botPlayers = nullptr, *g_botCountImm = nullptr, *g_resultsSkip = nullptr;
const int kModeBotFirst = 51, kModeBotLast = 75;

void WriteImm(uint8_t* at, uint8_t value)
{
	DWORD old;
	if (VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) {
		*at = value;
		VirtualProtect(at, 1, old, &old);
	}
}

uint32_t __attribute__((thiscall)) ModeStart(void* launch, int mode)
{
	bool bots = g_botArmed && mode >= kModeBotFirst && mode <= kModeBotLast;
	uint8_t count = bots ? (uint8_t)g_botCount.load() : 15, slots = count + 1;
	if (*g_botLoop != slots) {
		WriteImm(g_botLoop, slots);
		WriteImm(g_botPlayers, slots);
		WriteImm(g_botCountImm, count);
	}
	// Offline the results list all 16 slots; with fewer bots they check, as online, which are present.
	const uint8_t skip[2] = {0x74, 0x13};
	if (g_resultsSkip && (g_resultsSkip[0] == 0x90) != bots) {
		WriteImm(g_resultsSkip, bots ? 0x90 : skip[0]);
		WriteImm(g_resultsSkip + 1, bots ? 0x90 : skip[1]);
	}
	uint32_t result = g_modeStartOriginal(launch, mode);
	uint8_t* setup = *g_matchSetup;
	if (bots && setup) { // the match's player count (16 as set up): the results' "Players n/max"
		*(int32_t*)(setup + kSetupPlayers) = slots;
	}
	XLS_LOG_INFO("mods: rules: training: mode %d started (%u bots%s).", mode, count, bots ? ", from the Bots row" : "");
	return result;
}

template <typename T>
bool ReadGame(const void* at, T* out)
{
	SIZE_T got = 0;
	return at && ReadProcessMemory(GetCurrentProcess(), at, out, sizeof(T), &got) && got == sizeof(T);
}

DWORD WINAPI SurvivalThread(LPVOID)
{
	std::string last;
	bool wasOut = false;
	for (;; Sleep(200)) {
		if (g_hudHidden && GetTickCount() - g_hudTouched > 1000) { // nobody spectating any more
			HideHud(false);
		}
		uint8_t *mgr = nullptr, *owner = nullptr, *setup = nullptr;
		ReadGame(g_matchMgr, &mgr);
		ReadGame(g_matchSetup, &setup);
		if (mgr) {
			ReadGame(mgr + kMgrInfoOwner, &owner);
		}
		int32_t diePerson[16] = {}, dieTeam[16] = {};
		bool have = owner && ReadGame(owner + kOwnerInfoGame + kInfoDiePerson, &diePerson) && ReadGame(owner + kOwnerInfoGame + kInfoDieTeam, &dieTeam);
		int index = g_localIndex;
		int32_t team = -1;
		if (setup && index >= 0 && index < 16) {
			ReadGame(setup + kSetupSlots + index * kSetupSlot, &team);
		}
		bool out = false;
		if (have && g_survival && g_lives > 0 && index >= 0 && index < 16) {
			// Team Survival shares the lives: every death on the team takes one
			out = g_team ? team >= 0 && team < 16 && dieTeam[team] >= g_lives : diePerson[index] >= g_lives;
		}
		g_out = out;
		if (out != wasOut) {
			wasOut = out;
			XLS_LOG_INFO("mods: rules: survival: player %d (team %d) %s.", index, team, out ? "has no respawns left" : "can respawn");
		}
		if (have && g_survival) {
			std::string state = FormatA("slot %d team %d; deaths", index, team);
			for (int i = 0; i < 16; i++) {
				state += FormatA(" %d", diePerson[i]);
			}
			state += "; team deaths";
			for (int i = 0; i < 4; i++) {
				state += FormatA(" %d", dieTeam[i]);
			}
			if (state != last) {
				last = state;
				XLS_LOG_INFO("mods: rules: survival: %s.", state.c_str());
			}
		}
	}
}

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
	if (rules[kRuleSurvival] > 1 || (rules[kRuleSurvival] && !SurvivalOn(rules))) {
		rules[kRuleSurvival] = 0;
	}
	g_menuSurvival = SurvivalOn(rules);
	g_menuTeam = rules[kRuleMain] == 1;
	if (rules[kRuleLives] >= kLivesValues) {
		rules[kRuleLives] = 0;
	}
	if (SurvivalOn(rules)) {
		rules[kRuleKillTarget] = kKillTargetInfinite;
	}
	g_menuLives = kLives[rules[kRuleLives]];
	g_setNumber(screen, kDataNoKillShown, SurvivalOn(rules) ? kKillTargets + rules[kRuleLives] : rules[kRuleKillTarget]);
	int sub = rules[kRuleSub];
	g_setNumber(screen, kDataNoSubShown, SurvivalOn(rules) ? 2 : sub + (sub >= kSubPoints ? 1 : 0));
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
	g_matchMgr = (uint8_t***)(base + (kRulesVas[build].matchMgr - 0x400000));
	g_matchSetup = (uint8_t**)(base + (kRulesVas[build].matchSetup - 0x400000));
	g_net = (void**)(base + (kRulesVas[build].net - 0x400000));
	g_localSlot = (LocalSlotFn)(base + (kRulesVas[build].localSlot - 0x400000));
	g_controllers = (uint8_t**)(base + (kRulesVas[build].controllers - 0x400000));
	g_players = (uint8_t**)(base + (kRulesVas[build].players - 0x400000));
	g_follow = (FollowFn)(base + (kRulesVas[build].follow - 0x400000));
	g_followArg = (const float*)(base + (kRulesVas[build].followArg - 0x400000));
	g_flashCall = (FlashCallFn)(base + (kRulesVas[build].flashCall - 0x400000));
	g_input = (uint8_t**)(base + (kRulesVas[build].input - 0x400000));
	g_status = (uint8_t**)(base + (kRulesVas[build].status - 0x400000));
	const uint8_t kBotCursorPrologue[] = {0x53, 0xbb, 0x01, 0x00, 0x00, 0x00};                         // push ebx; mov ebx,1
	const uint8_t kBotStepPrologue[] = {0x56, 0x8b, 0xf1, 0x83, 0xbe, 0x90, 0x01, 0x00, 0x00, 0x01}; // push esi; mov esi,ecx; cmp [esi+0x190],1
	g_botCursorOriginal = (MenuFn)Detour(base + (kRulesVas[build].botCursor - 0x400000), kBotCursorPrologue, sizeof(kBotCursorPrologue), (void*)&BotCursor);
	g_botStepOriginal = (MenuFn)Detour(base + (kRulesVas[build].botStep - 0x400000), kBotStepPrologue, sizeof(kBotStepPrologue), (void*)&BotStep);
	if (!g_botCursorOriginal || !g_botStepOriginal) {
		XLS_LOG_WARN("mods: rules: training menu not hooked, no Survival rows for bot matches.");
	}
	g_botLoop = base + (kRulesVas[build].botLoop - 0x400000) + 2;       // 80 fb 10
	g_botPlayers = base + (kRulesVas[build].botPlayers - 0x400000) + 6; // c6 83 54 02 00 00 10
	g_botCountImm = base + (kRulesVas[build].botCount - 0x400000) + 6;  // c7 83 88 01 05 00 0f 00 00 00
	const uint8_t kModeStartPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec, 0x74}; // push ebp; mov ebp,esp; and esp,-8; sub esp,0x74
	uint8_t* skip = base + (kRulesVas[build].resultsSkip - 0x400000);
	g_resultsSkip = skip[0] == 0x74 && skip[1] == 0x13 ? skip : nullptr;
	if (g_botLoop[-2] == 0x80 && g_botLoop[0] == 0x10 && g_botPlayers[0] == 0x10 && g_botCountImm[0] == 0x0f) {
		g_modeStartOriginal = (ModeStartFn)Detour(base + (kRulesVas[build].modeStart - 0x400000), kModeStartPrologue, sizeof(kModeStartPrologue), (void*)&ModeStart);
	}
	const uint8_t kMenuHelpPrologue[] = {0x83, 0xec, 0x10, 0x56, 0x57, 0x8b, 0xf1}; // sub esp,0x10; push esi; push edi; mov esi,ecx
	g_menuHelpOriginal = (MenuHelpFn)Detour(base + (kRulesVas[build].menuHelp - 0x400000), kMenuHelpPrologue, sizeof(kMenuHelpPrologue), (void*)&MenuHelp);
	if (!g_modeStartOriginal) {
		XLS_LOG_WARN("mods: rules: AI Enemy Battle start not hooked, the Bots row has no effect.");
	}
	// sub esp,0x5c; mov eax,[results]
	uint8_t endPrologue[8] = {0x83, 0xec, 0x5c, 0xa1};
	memcpy(endPrologue + 4, &kRulesVas[build].results, 4);
	g_endCheckOriginal = (EndCheckFn)Detour(base + (kRulesVas[build].endCheck - 0x400000), endPrologue, sizeof(endPrologue), (void*)&EndCheck);
	const uint8_t kRankingPrologue[] = {0x83, 0xec, 0x58, 0x53, 0x55, 0x56, 0x57}; // sub esp,0x58; push ebx/ebp/esi/edi
	g_rankingOriginal = (RankingFn)Detour(base + (kRulesVas[build].ranking - 0x400000), kRankingPrologue, sizeof(kRankingPrologue), (void*)&Ranking);
	if (!g_endCheckOriginal || !g_rankingOriginal) {
		XLS_LOG_WARN("mods: rules: match end not hooked, survival matches end only by time.");
	}
	g_ctlUpdateOriginal = (ControlUpdateFn)Detour(base + (kRulesVas[build].ctlUpdate - 0x400000), kCtlUpdatePrologue, sizeof(kCtlUpdatePrologue), (void*)&ControlUpdate);
	if (!g_ctlUpdateOriginal) {
		XLS_LOG_WARN("mods: rules: controller update not hooked, survival can't hold respawns.");
	}
	HANDLE survival = CreateThread(nullptr, 0, SurvivalThread, nullptr, 0, nullptr);
	if (survival) {
		CloseHandle(survival);
	}
	XLS_LOG_INFO("mods: rules: hooked (%s).", build == 0 ? "DX11" : "DX9");
}


int RulesStep(int rule, int key, uint8_t* value, const std::function<int()>& original)
{
	if (rule == kRuleSub && (key == 10 || key == 11) && value[kRuleMain - kRuleSub] <= 1) {
		uint8_t& survival = value[kRuleSurvival - kRuleSub];
		bool on = *value == kSubKills && survival == 1;
		int changed = 0;
		if (key == 10 && *value == kSubKills && !on) { // Kills -> Survival
			survival = 1;
			changed = 1;
		}
		else if (key == 11 && on) { // Survival -> Kills
			survival = 0;
			changed = 1;
		}
		else {
			uint8_t was = *value;
			survival = 0;
			changed = original() ? 2 : 0; // 2: the game's step (it made the sound)
			if (changed && key == 11 && was == kSubPoints && *value == kSubKills) { // Points -> Survival
				survival = 1;
			}
		}
		g_menuSurvival = SurvivalOn(value - kRuleSub);
		g_menuTeam = value[kRuleMain - kRuleSub] == 1;
		if (g_menuSurvival) {
			value[kRuleKillTarget - kRuleSub] = kKillTargetInfinite;
		}
		return changed;
	}
	if (rule == kRuleKillTarget && (key == 10 || key == 11) && SurvivalOn(value - kRuleKillTarget)) { // lives, no wrap
		uint8_t& lives = value[kRuleLives - kRuleKillTarget];
		int at = lives % kLivesValues + (key == 10 ? 1 : -1);
		bool changed = at >= 0 && at < kLivesValues;
		lives = (uint8_t)(changed ? at : lives % kLivesValues);
		*value = kKillTargetInfinite;
		g_menuLives = kLives[lives];
		return changed ? 1 : 0;
	}
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
	if (id >= (uint32_t)kMsgKillRow && id < (uint32_t)(kMsgKillRow + kKillTargets)) {
		return 96 + (id - kMsgKillRow); // MSG_KILLCOUNT values
	}
	if (id >= (uint32_t)kMsgSubElim && id < (uint32_t)kMsgSubElim + 5 && kSubElimGameMsg[id - kMsgSubElim]) {
		return kSubElimGameMsg[id - kMsgSubElim];
	}
	if (id >= (uint32_t)kMsgSubTeam && id < (uint32_t)kMsgSubTeam + 6 && kSubTeamGameMsg[id - kMsgSubTeam]) {
		return kSubTeamGameMsg[id - kMsgSubTeam];
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
	if (id == (uint32_t)(kMsgItemName + kItemKillRow) && g_menuSurvival) {
		return "Respawns"; // the Kills row's label while Survival is picked
	}
	if (id == (uint32_t)kMsgSubElim + 2 || id == (uint32_t)kMsgSubTeam + 2) {
		return kSurvivalName;
	}
	if (id >= (uint32_t)(kMsgKillRow + kKillTargets) && id < (uint32_t)(kMsgKillRow + kKillTargets + kLivesValues)) {
		return kLivesNames[id - kMsgKillRow - kKillTargets];
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
	// the sub-type rows read our display value and texts (Survival)
	for (auto [item, msgBase] : {std::pair<int, int>{kItemSubElim, kMsgSubElim}, {kItemSubTeam, kMsgSubTeam}, {kItemKillRow, kMsgKillRow}}) {
		a.Push("OnlineRule");
		a.Action(kGetVariable);
		a.Push("offsetArray");
		a.Action(kGetMember);
		a.Push({item, item == kItemKillRow ? kDataNoKillShown : kDataNoSubShown, msgBase, 2});
		a.Action(kInitArray);
		a.Action(kSetMember);
	}
	a.code.push_back(0); // ActionEnd
	return a.code;
}

}

namespace {

// Inserts a DoAction with the script before the root timeline's first ShowFrame (after the root's own).
bool InsertRootScript(std::vector<uint8_t>* gfx, const Bytes& script, const char* what)
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
			XLS_LOG_INFO("mods: rules: %s added to the menu Flash (%zu bytes of script).", what, script.size());
			return true;
		}
		at = body + length;
	}
	return false;
}

}

bool RulesPatchMenuFlash(std::vector<uint8_t>* gfx)
{
	return InsertRootScript(gfx, RowsScript(), "create-match rows");
}

bool RulesPatchTrainingFlash(std::vector<uint8_t>* gfx)
{
	return InsertRootScript(gfx, Bytes(std::begin(kBotRowsScript), std::end(kBotRowsScript)), "AI Enemy Battle rows");
}

}
