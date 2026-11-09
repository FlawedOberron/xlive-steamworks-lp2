#include "mods/lp2.h"
#include "mods/package.h"
#include "mods/packages.h"
#include "mods/parts.h"
#include "mods/flash/botrows.h"
#include "mods/flash/setup.h"
#include "mods/gamerules.h"

#include "api/xsession.h"
#include "core/config.h"
#include "core/log.h"
#include "core/memberdata.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

#include <windows.h>
#include <vector>

#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <array>
#include <functional>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

#include "third-party/imgui/imgui.h"

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
// ours are 0x2e secondary (0 none, then kSecondaryKinds), 0x2f infinite ammo / grenades (bits), 0x31 ammo
// supply (0x30 free). The
// row table (DX11 0x17e9840) holds per category (16 x 0x23 bytes: main, sub, then per page a row
// count and 10 rules offsets) which offset each row edits; the Settings 1 page (page 1, 7 rows in 14
// categories) gets ours as rows 7-8. The Flash (id221) has its own rows: OnlineRule.sub1ItemArray
// [category] lists item ids, offsetArray[item] = [MsgOnline value base, DataNo]; the label is
// MsgOnline 38 + item. Ours are items 500, 501, 503, 504 (labels 538 + n, served by RulesMessage), values
// pushed as DataNo 75, 76, 78, 81-83 next to the game's rules. 0x34 Harmonizer (Off / On), 0x35 Grappling (range),
// 0x36 Hit Stagger (items 504-506).

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
	uint32_t listValue;   // the battle HUD's player list (DX11 FUN_00ed4c00): its sprintf call for a row's value
	uint32_t sprintfFn;   // that sprintf
	uint32_t gaugeFormat; // the "%d" it formats the Battle Gauge with
	uint32_t listSlotAt[3]; // the list's calls of the results' slot at a rank (thiscall(results, rank) -> s8)
	uint32_t listRankOf;    // its call of the results' rank of a slot (thiscall(results, slot) -> rank, from 1)
	uint32_t listDraw;      // the list's draw, thiscall(list) (see TeamRows)
	uint32_t flashGet;      // a movie's Flash variable get: thiscall(movie, name, value*) -> bool
	uint32_t flashSet;      // and set: thiscall(movie, name, const value*) -> bool
	uint32_t flashFree;     // a Flash value's release, thiscall(value)
	uint32_t hudUpdate;     // the HUD manager's update, thiscall(hud): no HUD while +0x2c's player has no body
	uint32_t teamBar;       // the cockpit's team gauge bar update (flash\cockpit\id755 mc_TEAM, top centre), thiscall(cockpit)
	uint32_t respawnSecond; // Spawn's respawn case for slot 7 (mov edi,[esi+0xf0]), 0x10 before the info-id case (DX11 0x8fb53c)
	uint32_t listValue2;    // the list's other sprintf call: Kills with a kill target, "%03d/%03d" (kills, target), esi at the kills
};
const RulesVas kRulesVas[kBuildCount] = {
	{0xc7aa20, 0xc7a820, 0xeca930, 0x20fc, 0x8fb130, 0xe8c270, 0xf4a330, 0x17e9840, 0xc86160, 0x2180, 0xc7cf15, 0xecac50, 0xed1e00, 0x18e086c, 0x1852850, 0x8029c0, 0x1852144, 0x8ff650, 0x1852080, 0x8e5e30, 0x16f7c6c, 0x1008ef0, 0x1852238, 0x1009b90, 0x18520c0, 0xf44680, 0xf44760, 0xf4a390, 0x18e0860, 0x873b80, 0x873df6, 0x873d1f, 0x873d5d, 0xf45eb0, 0xeaa1ba, 0x18d08ac, 0xed506d, 0x1224239, 0x16d9ab4, {0xed4c78, 0xed4cc1, 0xed4cdc}, 0xed4e92, 0xed4c00, 0xf4a570, 0xf4a550, 0x77a680, 0xee09d0, 0xed61e0, 0x8fb53c, 0xed5023}, // DX11
	{0xc6ae10, 0xc6ac10, 0xebab30, 0x20ec, 0x8eb7d0, 0xe7c1e0, 0xf3a600, 0x17d97e0, 0xc76550, 0x2170, 0xc6d305, 0xebae50, 0xec1ff0, 0x18ce780, 0x18427b0, 0x7f3b00, 0x18420a4, 0x8efcf0, 0x1841fe0, 0x8d64d0, 0x16e7098, 0xff8b10, 0x1842198, 0xff97b0, 0x1842020, 0xf34940, 0xf34a20, 0xf3a660, 0x18ce774, 0x8644d0, 0x864746, 0x86466f, 0x8646ad, 0xf36170, 0xe9a44a, 0x18c080c, 0xec525d, 0x1213e59, 0x16c9a24, {0xec4e68, 0xec4eb1, 0xec4ecc}, 0xec5082, 0xec4df0, 0xf3a840, 0xf3a820, 0x76b4e0, 0xed0bc0, 0xec63d0, 0x8ebbdc, 0xec5213}, // DX9
	{0xc7aa40, 0xc7a840, 0xecad00, 0x20fc, 0x8fb410, 0xe8c710, 0xf4a790, 0x17ea840, 0xc86180, 0x2180, 0xc7cf35, 0xecb020, 0xed21d0, 0x18e186c, 0x1853850, 0x802ae0, 0x1853144, 0x8ff930, 0x1853080, 0x8e6110, 0x16f805c, 0x10091e0, 0x1853238, 0x1009e80, 0x18530c0, 0xf44aa0, 0xf44b80, 0xf4a7f0, 0x18e1860, 0x873f10, 0x874186, 0x8740af, 0x8740ed, 0xf462d0, 0xeaa5ba, 0x18d18ac, 0xed543d, 0x12245c8, 0x16d9e64, {0xed5048, 0xed5091, 0xed50ac}, 0xed5262, 0xed4fd0, 0xf4a9d0, 0xf4a9b0, 0x77a770, 0xee0da0, 0xed65b0, 0x8fb81c, 0xed53f3}, // DX11 1.1
	{0xc6aa00, 0xc6a800, 0xebabd0, 0x20ec, 0x8eb980, 0xe7c750, 0xf3a6b0, 0x17d97e0, 0xc76140, 0x2170, 0xc6cef5, 0xebaef0, 0xec2090, 0x18ce780, 0x18427b0, 0x7f3c60, 0x18420a4, 0x8efea0, 0x1841fe0, 0x8d6680, 0x16e72f8, 0xff8e20, 0x1842198, 0xff9ac0, 0x1842020, 0xf34a10, 0xf34af0, 0xf3a730, 0x18ce774, 0x864760, 0x8649d6, 0x8648ff, 0x86493d, 0xf36240, 0xe9a4da, 0x18c080c, 0xec52fd, 0x1214028, 0x16c9c8c, {0xec4f08, 0xec4f51, 0xec4f6c}, 0xec5122, 0xec4e90, 0xf3a910, 0xf3a8f0, 0x76b550, 0xed0c60, 0xec6470, 0x8ebd8c, 0xec52b3}, // DX9 1.1
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
const int kMsgInfinite = 620;
// Grappling: the anchor's range (stock 1100 = 11 m) times the scale, 0 = no grappling. One row for on / off and the
// distance: a page holds 10 rows at most (the row table), and the team categories' Settings 3 has 8 with Ammo Supply.
// Value 0 is the game's own (settings saved before the row).
const int kMsgGrapple = 700, kMsgStagger = 710;
// The Anchor page: Range is this list, Speed the same without Off. Rule file values index these.
const char* const kGrappleNames[] = {"Off", "1/4", "1/2", "Default", "2X", "3X", "4X", "5X", "6X", "7X", "8X", "9X", "10X"};
const float kGrappleScales[] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f};
const int kGrappleDefault = 3, kAnchorSpeedDefault = 2, kAnchorSpeedValues = 12;
const int kGrappleValues = (int)(sizeof(kGrappleScales) / sizeof(kGrappleScales[0]));
// Hit Stagger: what a soldier does when a hit of each kind (bullet, explosion, anchor, melee) takes stagger points (see
// StaggerAdd). The points only decide when, and one hit is enough for most, so the rows pick the reaction instead.
// the pools: shrink = flinch, blown = knockdown, third = the electric stun (FUN_009b8bb0; user test 2026-11-09)
// Knockdown Pull / Push: the knockdown plus a throw toward / away from the attacker (Knockdown Force), see KnockdownThrow
const char* const kStaggerNames[] = {"Default", "Off", "Occasional", "Flinch", "Knockdown", "Knockdown Pull", "Knockdown Push", "Stun"};
const int kStaggerValues = (int)(sizeof(kStaggerNames) / sizeof(kStaggerNames[0]));
enum { kReactDefault, kReactOff, kReactOccasional, kReactFlinch, kReactBack, kReactBackPull, kReactBackPush, kReactDown };
const DWORD kOccasionalMs = 1500; // Occasional: at most one reaction this often
// Thresholds (Fall Stagger, Massive Shot Threshold): the game's limit times the scale, higher is harder to reach.
const char* const kThresholdNames[] = {"Default", "0.5x", "0.75x", "1.25x", "1.5x", "2x", "3x", "Never"};
const float kThresholdScales[] = {1.0f, 0.5f, 0.75f, 1.25f, 1.5f, 2.0f, 3.0f, 1000000.0f};
const int kThresholdValues = (int)(sizeof(kThresholdScales) / sizeof(kThresholdScales[0]));
// Massive Shot: bullets filling the knock-back (blown) pool, as a shotgun blast does.
const char* const kMassiveNames[] = {"Default", "Off", "Knockdown", "Knockdown Pull", "Knockdown Push", "Stun"};
const int kMassiveValues = 6;
enum { kMassiveDefault, kMassiveOff, kMassiveBack, kMassiveBackPull, kMassiveBackPush, kMassiveDown };
// Knockdown Force: how hard Knockdown Pull / Push throw (cm/s along the ground, and up; force.cpp ForceThrow). Rule key
// knockdownPush / kpush= (it was the push's own row first).
const char* const kPushNames[] = {"Light", "Medium", "Heavy", "Launch"};
const float kPushOut[] = {450.0f, 700.0f, 1000.0f, 1400.0f}, kPushUp[] = {300.0f, 450.0f, 600.0f, 1000.0f};
const int kPushValues = 4;
// Infinite Ammo: one row for both rules, bit 0 ammo, bit 1 grenades. Rules byte 0x30 held Unlimited
// Grenades before (moved into bit 1 when a saved match is loaded) and is free now.
const char* const kInfiniteNames[] = {"Off", "Weapons", "Grenades", "All"};
const int kInfiniteValues = 4, kInfiniteAmmo = 1, kInfiniteGrenades = 2, kRuleOldGrenades = 0x30;

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
	{kPageSettings2, 0x2f, kInfiniteValues, 501, 76, kMsgInfinite, "Infinite Ammo"},
	{kPageSettings3, 0x31, kSupplyValues, 503, 78, kMsgSupply, "Ammo Supply"},
	{kPageSettings3, 0x34, 2, 504, 81, kMsgOffOn, "Harmonizer"},
	{kPageSettings3, 0x35, 6, 505, 82, kMsgGrapple, "Grappling"},
	{kPageSettings2, 0x36, kStaggerValues, 506, 83, kMsgStagger, "Bullet Stagger"},
};
const int kRows = (int)(sizeof(kRowDefs) / sizeof(kRowDefs[0]));
enum { kRowSecondary, kRowInfinite, kRowSupply, kRowHarmonizer, kRowGrapple, kRowStagger };
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
	bool harmonizer = false; // the Campaign's Harmonizer (auto heal when low) in VS matches too
	int grapple = kGrappleDefault; // kGrappleScales index
	int anchorSpeed = kAnchorSpeedDefault; // the same, less Off
	int fall = 0; // kThresholdScales index
	int stagger = 0; // kStaggerNames index: bullets (and any hit not told apart)
	int staggerExplosion = 0, staggerAnchor = 0, staggerMelee = 0; // kStaggerNames indexes per hit kind
	int massive = 0, massiveThreshold = 0; // kMassiveNames, kThresholdScales indexes
	int push = 0; // kPushNames index
	int radar = 1, detectPlayers = 0, detectVs = 0, radarTeam = 1; // radar.cpp
	int grenadeCount = 0;  // 0 the game's amounts, else 1 + the count (so 1 is none)
	int grenadePickup = 0; // 0 the game's amounts, else the count
	int quickResupply = 0; // running over a carried weapon takes its ammo (resupply.cpp)
	int rounds = 0;        // RoundsTotal index: 0-9 = 1-10 rounds, 10-13 = Best of 3/5/7/9
	int switchEvery = 1;   // sides swap every this many rounds, 0 never
};
const int kSlotSecond = 7;
const DWORD kContextRichMode = 0; // the SPA presence context naming the mode (Campaign, Online...)

std::mutex g_mutex;
std::map<uint64_t, Rules> g_memberRules;
std::atomic<bool> g_ammo{false};
std::atomic<bool> g_grenades{false};
std::atomic<int> g_secondary{0};
std::atomic<int> g_supply{0};
std::atomic<bool> g_harmonizer{false};
uint8_t** g_game = nullptr;
std::atomic<int> g_grapple{kGrappleDefault};
std::atomic<int> g_stagger{0}, g_staggerExplosion{0}, g_staggerAnchor{0}, g_staggerMelee{0};
std::atomic<int> g_staggerExplosionRule{0}, g_staggerAnchorRule{0}, g_staggerMeleeRule{0}; // the setup rows (extras)
std::atomic<int> g_massive{0}, g_massiveThreshold{0}, g_massiveRule{0}, g_massiveThresholdRule{0};
std::atomic<int> g_push{0}, g_pushRule{0};
std::atomic<bool> g_survival{false};
std::atomic<bool> g_offline{true}; // not in a lobby (training): the bots run on this PC
// Training's AI Enemy Battle rows (see BotCursor): armed while that screen was the last one used
std::atomic<bool> g_botArmed{false}, g_botSurvival{false};
std::atomic<int> g_botLives{3}, g_botRule{0}; // index into kLives; the screen's rule (0 Elimination, 1 Team Elimination)
std::atomic<int> g_botCount{15};
std::atomic<int> g_botDifficulty{1}, g_botBrain{1}, g_botPersona{0}, g_botTaunts{2};
std::atomic<bool> g_matchBots{false}, g_botsInMatch{false};
std::atomic<int> g_roundsRule{0}, g_switchRule{1}, g_rounds{0}, g_switchEvery{1};
std::atomic<int> g_quickResupplyRule{0}, g_grenadeCountRule{0}, g_grenadePickupRule{0}, g_grenadeCount{0}, g_grenadePickup{0};
thread_local int g_inSpawn = 0; // weapons set up during a spawn are the player's own, any other is a pickup
std::atomic<int> g_radarTeamRule{1};
std::atomic<int> g_radarMode{1}, g_radarPlayers{0}, g_radarVs{0}, g_anchorRangeRule{kGrappleDefault}, g_anchorSpeedRule{kAnchorSpeedDefault}, g_fallRule{0};
const char* const kDifficultyNames[] = {"Easy", "Normal", "Hard", "Veteran"};
const char* const kBrainNames[] = {"Stock", "Improved"};
const char* const kPersonaNames[] = {"Mixed", "Aggressor", "Tactician", "Cautious", "Show-off", "Grenadier"};
const char* const kTauntNames[] = {"Off", "Rare", "Normal", "Lots"};
std::atomic<bool> g_menuSurvival{false};
std::atomic<int> g_lives{0};
std::atomic<int> g_menuLives{0};
std::atomic<bool> g_team{false};
std::atomic<bool> g_survivalDropped{false};
std::atomic<uint32_t> g_elimMask{0}; // a game mode's eliminated players (held too)
std::atomic<int> g_elimOrder[16] = {}, g_elimCount{0}; // when each was eliminated this round (1 first out), placement
std::atomic<bool> g_menuTeam{false};
uint32_t g_magazine = 0;
uint8_t** g_ruleSlots = nullptr;
bool Host();
uint32_t g_dataAt = 0;

// Grenade totals are in units: the count shown is the total over the entry's cost of a throw (+0x4e; 1 with +5 bit 1), rounded
// up (DX11 FUN_00c7acc0). The weapon's own entry is at g_dataAt + 4.
int GrenadeCost(const uint8_t* entry)
{
	if (!entry || (entry[5] & 2)) {
		return 1;
	}
	int16_t cost = *(const int16_t*)(entry + 0x4e);
	return cost > 0 ? cost : 1;
}

const uint8_t* WeaponEntry(const uint8_t* weapon)
{
	return *(uint8_t* const*)(weapon + g_dataAt + 4);
}

std::string Format(const Rules& r)
{
	return FormatA("ammo=%d;grenades=%d;secondary=%d;magazines=%d;survival=%d;lives=%d;team=%d;harmonizer=%d;grapple=%d;stagger=%d;radar=%d;detectp=%d;detectv=%d;anchor=%d;fall=%d;gcount=%d;gpick=%d;qres=%d;rnd=%d;sw=%d;rteam=%d;sexp=%d;sanc=%d;smel=%d;mshot=%d;mthr=%d;kpush=%d", r.ammo, r.grenades, r.secondary, r.supply, r.survival, r.lives, r.team, r.harmonizer, r.grapple, r.stagger, r.radar, r.detectPlayers, r.detectVs, r.anchorSpeed, r.fall, r.grenadeCount, r.grenadePickup, r.quickResupply, r.rounds, r.switchEvery, r.radarTeam, r.staggerExplosion, r.staggerAnchor, r.staggerMelee, r.massive, r.massiveThreshold, r.push);
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
	r.harmonizer = s.find("harmonizer=1") != std::string::npos;
	size_t grappleAt = s.find("grapple="), staggerAt = s.find("stagger=");
	r.grapple = grappleAt == std::string::npos ? kGrappleDefault : atoi(s.c_str() + grappleAt + 8);
	r.grapple = r.grapple >= 0 && r.grapple < kGrappleValues ? r.grapple : kGrappleDefault;
	r.stagger = staggerAt == std::string::npos ? 0 : atoi(s.c_str() + staggerAt + 8);
	r.stagger = r.stagger >= 0 && r.stagger < kStaggerValues ? r.stagger : 0;
	auto small = [&](const char* key, int count, int fallback) {
		size_t found = s.find(key);
		int value = found == std::string::npos ? fallback : atoi(s.c_str() + found + strlen(key));
		return value >= 0 && value < count ? value : fallback;
	};
	r.radar = small("radar=", 3, 1);
	r.anchorSpeed = small("anchor=", kAnchorSpeedValues, kAnchorSpeedDefault);
	r.fall = small("fall=", kThresholdValues, 0);
	r.grenadeCount = small("gcount=", 12, 0);
	r.grenadePickup = small("gpick=", 11, 0);
	r.quickResupply = small("qres=", 2, 0);
	r.rounds = small("rnd=", 14, 0);
	r.switchEvery = small("sw=", 11, 1);
	r.detectPlayers = small("detectp=", 4, 0);
	r.detectVs = small("detectv=", 4, 0);
	r.radarTeam = small("rteam=", 2, 1);
	r.staggerExplosion = small("sexp=", kStaggerValues, 0);
	r.staggerAnchor = small("sanc=", kStaggerValues, 0);
	r.staggerMelee = small("smel=", kStaggerValues, 0);
	r.massive = small("mshot=", kMassiveValues, 0);
	r.massiveThreshold = small("mthr=", kThresholdValues, 0);
	r.push = small("kpush=", kPushValues, 0);
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
	r.radar = g_radarMode;
	r.detectPlayers = g_radarPlayers;
	r.detectVs = g_radarVs;
	r.radarTeam = g_radarTeamRule;
	r.grapple = g_anchorRangeRule;
	r.anchorSpeed = g_anchorSpeedRule;
	r.fall = g_fallRule;
	r.grenadeCount = g_grenadeCountRule;
	r.grenadePickup = g_grenadePickupRule;
	r.quickResupply = g_quickResupplyRule;
	r.rounds = g_roundsRule;
	r.switchEvery = g_switchRule;
	r.staggerExplosion = g_staggerExplosionRule;
	r.staggerAnchor = g_staggerAnchorRule;
	r.staggerMelee = g_staggerMeleeRule;
	r.massive = g_massiveRule;
	r.massiveThreshold = g_massiveThresholdRule;
	r.push = g_pushRule;
	if (g_menuSeen) {
		r.ammo = (g_menu[kRowInfinite] & kInfiniteAmmo) != 0;
		r.grenades = (g_menu[kRowInfinite] & kInfiniteGrenades) != 0;
		r.secondary = kSecondaryKinds[g_menu[kRowSecondary] % kSecondaryValues];
		r.supply = kSupplyMagazines[g_menu[kRowSupply] % kSupplyValues];
		r.survival = g_menuSurvival;
		r.lives = r.survival ? g_menuLives.load() : 0;
		r.team = r.survival && g_menuTeam;
		r.harmonizer = g_menu[kRowHarmonizer] != 0;
		r.stagger = g_menu[kRowStagger] % kStaggerValues;
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

// A lobby's match setting (session properties as the game advertises them: 0x10000001 rule, 0 Elimination /
// 1 Team Elimination; 0x10000002 sub-type, 1 Kills).
const uint32_t kPropRule = 0x10000001, kPropSubRule = 0x10000002;

bool LobbyRule(CSteamID lobby, uint32_t propertyId, long* out)
{
	const char* v = SteamMatchmaking()->GetLobbyData(lobby, FormatA("%sp%08x", Cfg().lobbyKeyPrefix.c_str(), propertyId).c_str());
	if (!v || !*v) {
		return false;
	}
	*out = strtol(v, nullptr, 10);
	return true;
}

template <typename T>
bool ReadGame(const void* at, T* out)
{
	SIZE_T got = 0;
	return at && ReadProcessMemory(GetCurrentProcess(), at, out, sizeof(T), &got) && got == sizeof(T);
}

uint8_t** g_players = nullptr;
// players +0x8174: what the match scores by (the battle HUD's player list shows it): 1 Battle Gauge, 2 kills, 3 points
const uint32_t kPlayersListMode = 0x8174;
const int32_t kListGauge = 1, kListKills = 2, kListPoints = 3;

// Harmonizer: a human's auto heal (low health -> boot, then heals; uHuman +0x21cc state) starts from DX11
// FUN_00c54e50 (fastcall(human) -> bool, also asked by the bots' brains), which refuses in matches with the
// 16-slot flag (game +0x256: VS, Online, AI Enemy Battle) by `cmp byte [eax+0x256],0; jne <false>`. On, that jne
// becomes two nops, so the Campaign's Harmonizer runs in VS matches too.
const uint32_t kHarmonizerStart[kBuildCount] = {0xc54e50, 0xc45250, 0xc54e90, 0xc44e60};
const uint8_t kHarmonizerCheck[] = {0x80, 0xb8, 0x56, 0x02, 0x00, 0x00, 0x00, 0x75, 0xeb}; // at +0x19
uint8_t* g_harmonizerJump = nullptr; // the jne, at +0x20

void HarmonizerSet(bool on)
{
	if (!g_harmonizerJump || g_harmonizer == on) {
		return;
	}
	g_harmonizer = on;
	const uint8_t bytes[2] = {(uint8_t)(on ? 0x90 : 0x75), (uint8_t)(on ? 0x90 : 0xeb)};
	DWORD old;
	if (VirtualProtect(g_harmonizerJump, 2, PAGE_EXECUTE_READWRITE, &old)) {
		memcpy(g_harmonizerJump, bytes, 2);
		VirtualProtect(g_harmonizerJump, 2, old, &old);
		FlushInstructionCache(GetCurrentProcess(), g_harmonizerJump, 2);
	}
}

// Grapple range: the anchor (uWp00) keeps it at +0x225c (DX11) / +0x224c (DX9), set to 1100 when made, and the aim ray,
// the bots and the rope limit (uCharacter FUN_009cb4f0: a rope longer than this pulls the player in by the difference)
// read it through its vf379 (fld dword [ecx+range]; ret). That getter is replaced by a jump to ours: the range times the
// Grappling row's scale.
// The hook's flight doesn't use it: the shot (FUN_00c89ab0) and the reel (FUN_00c8a720) compare the rope with a shared
// constant, 1100.0 (DX11 0x138a01c), which enemies and VS read too, so only these three instructions are pointed at
// g_grappleLimit. Off: limit 0, the hook goes back straight away.
const uint32_t kGrappleRangeGet[kBuildCount] = {0xc86600, 0xc769f0, 0xc86620, 0xc765f0};
// The disp32 of: shot "mulss xmm0, [1100]" (x 1.2, a locked target's lead), shot and reel "comiss xmm0, [1100]", and the
// green crosshair's reach for humans (FUN_00ed69a0: "movss xmm1, [1100]").
const uint32_t kGrappleLimitAt[kBuildCount][4] = {
	{0xc89d35, 0xc8a242, 0xc8aacd, 0xed6fde},
	{0xc7a155, 0xc7a662, 0xc7aeed, 0xec71ce},
	{0xc89ca5, 0xc8a1b2, 0xc8aa3d, 0xed73ae},
	{0xc79ce5, 0xc7a1f2, 0xc7aa7d, 0xec726e},
};
const float kGrappleStock = 1100.0f;
// Fall Stagger: a human landing (19 sites across its jump and fall actions, uHuman vf337 and friends) compares its fall speed
// (cJump mSpd.y, +0x17c8) with a shared -40.0: at or past it the heavy landing (motion 0x53, the stagger), otherwise the light
// one (0x0c). These compares read ours instead. Others read the constant for other things and keep it.
const float kFallStock = -40.0f;
volatile float g_fallThreshold = kFallStock;
struct FallSites {
	uint32_t constant, first, last;
};
const FallSites kFallSites[kBuildCount] = {
	{0x147e90c, 0xc43158, 0xc73605},
	{0x146e7b4, 0xc33568, 0xc639f5},
	{0x147eb5c, 0xc431b8, 0xc73625},
	{0x146ea1c, 0xc330c8, 0xc635e5},
};
const int kFallSiteCount = 19;
// Anchor Speed: the hook's launch speed, a shared 120.0 read when each of the three launches sets its velocity (+0xc30): the
// shot (FUN_00c89ab0), the second firing (FUN_00c8a720) and FUN_00c8ada0. These "movss xmm, [120]" read ours instead.
// The flight back has its own speed.
const float kAnchorSpeedStock = 120.0f;
volatile float g_anchorSpeed = kAnchorSpeedStock;
const uint32_t kAnchorSpeedAt[kBuildCount][3] = {
	{0xc8a0ae, 0xc8a993, 0xc8b0cc},
	{0xc7a4ce, 0xc7adb3, 0xc7b4ec},
	{0xc8a01e, 0xc8a903, 0xc8b03c},
	{0xc7a05e, 0xc7a943, 0xc7b07c},
};
uint32_t g_grappleRangeAt = 0;
volatile float g_grappleLimit = kGrappleStock;

extern "C" float __attribute__((thiscall)) GrappleRange(uint8_t* anchor)
{
	float range = *(float*)(anchor + g_grappleRangeAt);
	float scaled = range * kGrappleScales[g_grapple % kGrappleValues];
	static std::atomic<float> logged{-1.0f};
	if (logged.exchange(scaled) != scaled) {
		XLS_LOG_INFO("mods: rules: grapple range %.0f x %.2f = %.0f cm (read by 0x%p).", range, kGrappleScales[g_grapple % kGrappleValues], scaled, __builtin_return_address(0));
	}
	return scaled < 1.0f ? 1.0f : scaled;
}

// Grappling Off: the two human actions that fire the anchor (uHuman vf371, action 0x23: anchor state 1, the shot;
// vf468, action 0x46: anchor state 2) do nothing and report finished (vf +0x114 clears 0x100, as they do when they
// end), so there is no animation and no hook. The think goes back to normal the way it does after a grapple.
const uint32_t kAnchorFire[kBuildCount][2] = {
	{0xc5c240, 0xc5e840},
	{0xc4c630, 0xc4ec30},
	{0xc5c260, 0xc5e860},
	{0xc4c220, 0xc4e820},
};
const uint8_t kAnchorFirePrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0}; // push ebp; mov ebp, esp; and esp, -16
typedef void(__attribute__((thiscall)) * AnchorActionFn)(void*);
typedef void(__attribute__((thiscall)) * ClearFlagsFn)(void*, uint32_t, uint32_t);
AnchorActionFn g_anchorFireOriginal[2] = {nullptr, nullptr};

bool AnchorBlocked(void* human)
{
	if (kGrappleScales[g_grapple % kGrappleValues] != 0.0f) {
		return false;
	}
	static std::atomic<bool> logged{false};
	if (!logged.exchange(true)) {
		XLS_LOG_INFO("mods: rules: Grappling is Off, the anchor doesn't fire.");
	}
	void** vtable = *(void***)human;
	((ClearFlagsFn)vtable[0x114 / 4])(human, 0x100, 0);
	return true;
}

extern "C" void __attribute__((thiscall)) AnchorShot(void* human)
{
	if (!AnchorBlocked(human)) {
		g_anchorFireOriginal[0](human);
	}
}

extern "C" void __attribute__((thiscall)) AnchorReel(void* human)
{
	if (!AnchorBlocked(human)) {
		g_anchorFireOriginal[1](human);
	}
}

void GrappleInstall(uint8_t* base, int build)
{
	for (int i = 0; i < 2; i++) {
		g_anchorFireOriginal[i] = (AnchorActionFn)Detour(base + (kAnchorFire[build][i] - 0x400000), kAnchorFirePrologue, sizeof(kAnchorFirePrologue), i == 0 ? (void*)&AnchorShot : (void*)&AnchorReel);
		if (!g_anchorFireOriginal[i]) {
			XLS_LOG_WARN("mods: rules: anchor fire action %d not hooked, Grappling Off won't stop the shot.", i);
		}
	}

	uint8_t* get = base + (kGrappleRangeGet[build] - 0x400000);
	if (get[0] != 0xd9 || get[1] != 0x81 || get[4] != 0x00 || get[5] != 0x00 || get[6] != 0xc3) {
		XLS_LOG_WARN("mods: rules: grapple range getter not found, the Grappling rows have no effect.");
		return;
	}
	g_grappleRangeAt = *(uint32_t*)(get + 2);
	DWORD old;
	if (VirtualProtect(get, 5, PAGE_EXECUTE_READWRITE, &old)) {
		get[0] = 0xe9;
		*(int32_t*)(get + 1) = (int32_t)((uint8_t*)&GrappleRange - (get + 5));
		VirtualProtect(get, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), get, 5);
	}

	bool speedFound = true;
	for (int i = 0; i < 3; i++) {
		uint8_t* site = base + (kAnchorSpeedAt[build][i] - 0x400000);
		const float* constant = *(const float**)site;
		speedFound = speedFound && site[-4] == 0xf3 && site[-3] == 0x0f && site[-2] == 0x10 && !IsBadReadPtr(constant, sizeof(float)) && *constant == kAnchorSpeedStock;
	}
	for (int i = 0; speedFound && i < 3; i++) {
		uint8_t* site = base + (kAnchorSpeedAt[build][i] - 0x400000);
		if (VirtualProtect(site, 4, PAGE_EXECUTE_READWRITE, &old)) {
			*(volatile float**)site = &g_anchorSpeed;
			VirtualProtect(site, 4, old, &old);
			FlushInstructionCache(GetCurrentProcess(), site, 4);
		}
	}
	if (!speedFound) {
		XLS_LOG_WARN("mods: rules: anchor launch speed not found, the Speed row has no effect.");
	}

	const FallSites& fall = kFallSites[build];
	uint32_t fallConstant = (uint32_t)(uintptr_t)(base + (fall.constant - 0x400000));
	uint8_t* fallEnd = base + (fall.last - 0x400000);
	std::vector<uint8_t*> fallAt;
	for (uint8_t* at = base + (fall.first - 0x400000); at <= fallEnd; at++) {
		if (*(uint32_t*)at == fallConstant) {
			fallAt.push_back(at);
		}
	}
	if ((int)fallAt.size() == kFallSiteCount && *(const float*)(uintptr_t)fallConstant == kFallStock) {
		for (uint8_t* at : fallAt) {
			if (VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) {
				*(volatile float**)at = &g_fallThreshold;
				VirtualProtect(at, 4, old, &old);
				FlushInstructionCache(GetCurrentProcess(), at, 4);
			}
		}
	}
	else {
		XLS_LOG_WARN("mods: rules: landing check not recognised (%d sites), the Fall Stagger row has no effect.", (int)fallAt.size());
	}

	// All or none: half of them would let the hook fly past a limit the rest still enforce.
	uint8_t* sites[4];
	for (int i = 0; i < 4; i++) {
		sites[i] = base + (kGrappleLimitAt[build][i] - 0x400000);
		bool opcode = i == 0 ? (sites[i][-4] == 0xf3 && sites[i][-3] == 0x0f && sites[i][-2] == 0x59 && sites[i][-1] == 0x05)
		            : i == 3 ? (sites[i][-4] == 0xf3 && sites[i][-3] == 0x0f && sites[i][-2] == 0x10 && sites[i][-1] == 0x0d)
		                     : (sites[i][-3] == 0x0f && sites[i][-2] == 0x2f && sites[i][-1] == 0x05);
		const float* constant = *(const float**)sites[i];
		if (!opcode || IsBadReadPtr(constant, sizeof(float)) || *constant != kGrappleStock) {
			XLS_LOG_WARN("mods: rules: grapple limit %d not found at 0x%p, the hook keeps its stock 11 m flight.", i, sites[i]);
			return;
		}
	}
	for (uint8_t* site : sites) {
		if (VirtualProtect(site, 4, PAGE_EXECUTE_READWRITE, &old)) {
			*(volatile float**)site = &g_grappleLimit;
			VirtualProtect(site, 4, old, &old);
			FlushInstructionCache(GetCurrentProcess(), site, 4);
		}
	}
}

// Hit Stagger: a character's stagger (shrink) points count down from their max (cCharParam +0xc, s16); a hit takes
// its points off through the character's add (DX11 FUN_009b72a0, thiscall(character, delta, ?) -> bool still
// standing; called by the hit handlers with a negative delta), and at 0 the character flinches and refills. Ours
// scales the negative deltas; refills (positive) pass as they are. VS rules only, so the Campaign's Akrid keep theirs.
using StaggerAddFn = bool(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t);
// Three point pools a hit takes from (uCharacterBase::vf151, DX11 0x9d4210): shrink (+0xc, FUN_009b72a0: flag d7c 0x10),
// blown (+0x12, FUN_009b7350: flag d7c 0x20), and a third (+0x18, FUN_009b7400: FUN_009b8bb0). Which one makes a
// rifle hit flinch a human isn't known yet, so the Hit Stagger scale goes on all three and the log says which ran out.
StaggerAddFn g_staggerOriginal[3] = {};
const uint32_t kStaggerAdd[kBuildCount] = {0x9b72a0, 0x9a7a20, 0x9b75c0, 0x9a7b70};
const uint32_t kStaggerPoolOffset[3] = {0, 0xb0, 0x160};
const char* const kStaggerPools[3] = {"shrink", "blown", "third"};

// The hit's kind, from what called the pools (bot match log 2026-11-09, game-settings-plan.md): melee and anchor hits come
// through their own functions, whose return addresses are on the stack; the third pool is the explosions' knock; the
// rest are bullets. Return addresses (VAs) per build, DX11 Steam found in the log, the rest ported (sigport.py).
enum StaggerKind { kStaggerBullet, kStaggerExplosion, kStaggerAnchor, kStaggerMelee };
const char* const kStaggerKindNames[] = {"bullet", "explosion", "anchor", "melee"};
const uint32_t kStaggerMeleeRet[kBuildCount] = {0x8472a7, 0x837e57, 0x8474a7, 0x837f97};  // FUN_00847270's call (DX11)
const uint32_t kStaggerAnchorRet[kBuildCount] = {0x834650, 0x825860, 0x834880, 0x825910}; // FUN_008345c0's call (DX11)
int g_staggerBuild = -1;

int StaggerKindOf(int pool)
{
	if (pool == 2) {
		return kStaggerExplosion;
	}
	if (g_staggerBuild < 0) {
		return kStaggerBullet;
	}
	uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr) - 0x400000;
	uintptr_t melee = kStaggerMeleeRet[g_staggerBuild] + base, anchor = kStaggerAnchorRet[g_staggerBuild] + base;
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	uintptr_t* sp = (uintptr_t*)__builtin_frame_address(0);
	uintptr_t* top = (uintptr_t*)tib->StackBase;
	for (int k = 0; k < 200 && sp + k < top; k++) {
		if (sp[k] == melee) {
			return kStaggerMelee;
		}
		if (sp[k] == anchor) {
			return kStaggerAnchor;
		}
	}
	return kStaggerBullet;
}

// Per character: when it last reacted (Occasional) and when we last forced a reaction (one per hit: a hit can take from more
// than one pool, and a shotgun's pellets come as many hits).
struct StaggerSeen {
	DWORD reacted = 0, forced = 0;
};
std::mutex g_staggerSeenMutex;
std::map<void*, StaggerSeen> g_staggerSeen;
const DWORD kStaggerSameHitMs = 100;
// The hit handler (uCharacterBase::vf151) counts a reaction level from the pools' answers: shrink out = 1, blown out +1,
// third out +1 and a knockdown (FUN_009b8bb0, which only it calls). The level then sizes the local player's camera shake and
// rumble (FUN_009d2860) and a meter (FUN_009b9190); the flinch and knock-back are set by the pools themselves.
using KnockdownFn = void(__attribute__((thiscall)) *)(void*);
using HitShakeFn = void(__attribute__((thiscall)) *)(void*, uint32_t);
const uint32_t kStaggerKnockdown[kBuildCount] = {0x9b8bb0, 0x9a9330, 0x9b8ed0, 0x9a9480};
const uint32_t kHitShake[kBuildCount] = {0x9d2860, 0x9c2ea0, 0x9d27c0, 0x9c2c20};
KnockdownFn g_knockdown = nullptr;
HitShakeFn g_hitShakeOriginal = nullptr;
// This thread's last hit: no reaction wanted (Off, or Occasional waiting), so the shake goes too
thread_local bool t_hitQuiet = false;
thread_local DWORD t_hitAt = 0;

void __attribute__((thiscall)) HitShake(void* character, uint32_t level)
{
	bool quiet = t_hitQuiet && GetTickCount() - t_hitAt < 50;
	t_hitQuiet = false;
	if (!quiet) {
		g_hitShakeOriginal(character, level);
	}
}

void StaggerLog(const char* format, const char* kind, const char* what)
{
	static LONG logged = 0;
	if (InterlockedIncrement(&logged) <= 100) {
		XLS_LOG_INFO(format, kind, what);
	}
}

StaggerSeen StaggerSeenGet(void* character)
{
	std::lock_guard<std::mutex> lock(g_staggerSeenMutex);
	if (g_staggerSeen.size() > 512) {
		g_staggerSeen.clear();
	}
	return g_staggerSeen[character];
}

void StaggerSeenSet(void* character, bool reacted, bool forced, DWORD now)
{
	std::lock_guard<std::mutex> lock(g_staggerSeenMutex);
	StaggerSeen& seen = g_staggerSeen[character];
	seen.reacted = reacted ? now : seen.reacted;
	seen.forced = forced ? now : seen.forced;
}

// The hit handler (uCharacterBase::vf151, thiscall(character, 4 args) -> float): its third argument is the attacker,
// a character (type +0x1f 1-3) or a shot (type 4, its owner at +0x24; the handler itself follows it so). Kept for this
// thread while it runs (the knockdown throw's direction), and per character for the fall kill credit (FallCredit).
using HitHandlerFn = float(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
const uint32_t kHitHandler[kBuildCount] = {0x9d4210, 0x9c4850, 0x9d4170, 0x9c45d0};
HitHandlerFn g_hitHandlerOriginal = nullptr;
thread_local uint8_t* t_attacker = nullptr;
// No flinch for this hit (Bullet Off, or Occasional waiting): the handler sets "was hit" (+0xd7c bit 8, DX9 +0xd6c)
// on every hit, and the player's think (cThinkPlVsCF::vf14) turns it into the flinch the next frame; cleared after.
thread_local bool t_noFlinch = false;
const DWORD kFallCreditMs = 5000;
struct LastHit {
	DWORD at;
	int slot;
};
std::mutex g_lastHitMutex;
std::map<void*, LastHit> g_lastHit;

uint8_t* AttackerBody(uint8_t* attacker)
{
	if ((uintptr_t)attacker < 0x10000) {
		return nullptr;
	}
	if (attacker[0x1f] == 4) {
		attacker = *(uint8_t**)(attacker + 0x24);
		if ((uintptr_t)attacker < 0x10000) {
			return nullptr;
		}
	}
	uint8_t type = attacker[0x1f];
	uint32_t state = *(uint32_t*)(attacker + 4) & 7;
	return type >= 1 && type <= 3 && (state == 1 || state == 2) ? attacker : nullptr;
}

float __attribute__((thiscall)) HitHandler(void* character, uint32_t a, uint32_t b, uint32_t attacker, uint32_t d)
{
	uint8_t* was = t_attacker;
	uint8_t* body = AttackerBody((uint8_t*)(uintptr_t)attacker);
	t_attacker = body;
	t_noFlinch = false;
	float result = g_hitHandlerOriginal(character, a, b, attacker, d);
	if (t_noFlinch) {
		bool dx9 = g_staggerBuild == 1 || g_staggerBuild == 3;
		*(uint32_t*)((uint8_t*)character + (dx9 ? 0xd6c : 0xd7c)) &= ~8u;
		t_noFlinch = false;
	}
	t_attacker = was;
	if (body && body != character && ((uint8_t*)character)[0x1f] == 1) {
		int slot = RulesSlotOf(body);
		if (slot >= 0 && slot != RulesSlotOf((uint8_t*)character)) {
			std::lock_guard<std::mutex> lock(g_lastHitMutex);
			if (g_lastHit.size() > 256) {
				g_lastHit.clear();
			}
			g_lastHit[character] = {GetTickCount(), slot};
		}
	}
	return result;
}

// A death with no hit (a fall, a kill zone): the game's think (DX9 FUN_00902bc0, thiscall(think, character, hit), hit
// null) takes the killer from the character's "burn attacker" slot (+0x166c DX9 / +0x167c DX11) when +1 is set, as a
// burn death does. A soldier hit by another player in the last 5 s gets that player there first, so the fall counts as
// their kill. Resets at the next spawn (the game's).
using DeathThinkFn = void(__attribute__((thiscall)) *)(void*, uint8_t*, void*);
const uint32_t kDeathThink[kBuildCount] = {0x912590, 0x902bc0, 0x912800, 0x902de0};
DeathThinkFn g_deathThinkOriginal = nullptr;

void __attribute__((thiscall)) DeathThink(void* think, uint8_t* character, void* hit)
{
	if (!hit && character && character[0x1f] == 1) {
		bool dx9 = g_staggerBuild == 1 || g_staggerBuild == 3;
		uint8_t* credit = character + (dx9 ? 0x166c : 0x167c);
		LastHit last{0, -1};
		{
			std::lock_guard<std::mutex> lock(g_lastHitMutex);
			auto it = g_lastHit.find(character);
			if (it != g_lastHit.end()) {
				last = it->second;
				g_lastHit.erase(it);
			}
		}
		if (last.slot >= 0 && GetTickCount() - last.at < kFallCreditMs && !credit[1]) {
			credit[0] = (uint8_t)last.slot;
			credit[1] = 1;
			XLS_LOG_INFO("mods: rules: a death with no hit (player %d), %u ms after player %d's hit: their kill.", RulesSlotOf(character), (unsigned)(GetTickCount() - last.at), last.slot);
		}
	}
	g_deathThinkOriginal(think, character, hit);
}

// Knockdown Pull / Push: a soldier knocked down is thrown toward / away from the attacker along the ground (straight up
// when there's none to go by) and up, by Knockdown Force.
void KnockdownThrow(void* character, bool pull, const char* why)
{
	int force = g_push % kPushValues;
	const float* pos = (const float*)((uint8_t*)character + 0x30);
	uint8_t* attacker = t_attacker;
	float dx = 0.0f, dz = 0.0f;
	if (attacker && (uint8_t*)character != attacker) {
		const float* from = (const float*)(attacker + 0x30);
		dx = pos[0] - from[0];
		dz = pos[2] - from[2];
	}
	float len = sqrtf(dx * dx + dz * dz);
	float out = (pull ? -1.0f : 1.0f) * kPushOut[force];
	if (pull && len > 1.0f) {
		// a pull lands short of the attacker rather than past them (flight time 2 up / g, g taken as 980 cm/s2)
		out = -std::min(kPushOut[force], 0.8f * len * 980.0f / (2.0f * kPushUp[force]));
	}
	float v[3] = {len > 1.0f ? dx / len * out : 0.0f, kPushUp[force], len > 1.0f ? dz / len * out : 0.0f};
	ForceThrow((unsigned char*)character, v);
	static LONG logged = 0;
	if (InterlockedIncrement(&logged) <= 50) {
		XLS_LOG_INFO("mods: rules: knockdown %s (%s, %s): %.0f %.0f %.0f cm/s, attacker %.0f cm away (type %d)%s.", pull ? "pull" : "push", kPushNames[force], why, v[0], v[1], v[2], len, attacker ? attacker[0x1f] : 0, len > 1.0f ? "" : ", none to go by");
	}
}

// Hit probe (diagnostic, log only): for the local player's first hits, which state bytes and flags of the character change
// in the 0.4 s after (bytes +0x10..+0x40, dwords +0xcc0..+0xda0, raw offsets of this build). To find what plays the
// per-hit flinch that no stagger pool starts (user test 2026-11-09: Bullet Stagger Off, rifle hits still flinch).
std::atomic<int> g_hitProbes{0};
std::atomic<bool> g_hitProbeBusy{false};
struct HitProbeArgs {
	uint8_t* body;
	int kind;
};

DWORD WINAPI HitProbeThread(LPVOID param)
{
	HitProbeArgs a = *(HitProbeArgs*)param;
	delete (HitProbeArgs*)param;
	uint8_t was[0x40], now[0x40];
	uint32_t wasF[0x38], nowF[0x38];
	auto grab = [&](uint8_t* bytes, uint32_t* flags) {
		return ReadGame(a.body + 0x10, (uint8_t(*)[0x30])bytes) && ReadGame(a.body + 0xcc0, (uint32_t(*)[0x38])flags);
	};
	int lines = 0;
	if (grab(was, wasF)) {
		XLS_LOG_INFO("mods: rules: hit probe %d: a %s hit on the local player.", g_hitProbes.load(), kStaggerKindNames[a.kind]);
		for (int t = 15; t <= 400 && lines < 60; t += 15) {
			Sleep(15);
			if (!grab(now, nowF)) {
				break;
			}
			for (int i = 0; i < 0x30 && lines < 60; i++) {
				if (now[i] != was[i]) {
					XLS_LOG_INFO("mods: rules: hit probe:   +%3d ms byte +0x%02x %02x -> %02x", t, 0x10 + i, was[i], now[i]);
					lines++;
				}
			}
			for (int i = 0; i < 0x38 && lines < 60; i++) {
				if (nowF[i] != wasF[i]) {
					XLS_LOG_INFO("mods: rules: hit probe:   +%3d ms dword +0x%03x %08x -> %08x", t, 0xcc0 + i * 4, wasF[i], nowF[i]);
					lines++;
				}
			}
			memcpy(was, now, sizeof(was));
			memcpy(wasF, nowF, sizeof(wasF));
		}
	}
	g_hitProbeBusy = false;
	return 0;
}

void HitProbe(void* character, int kind)
{
	if (g_hitProbes.load() >= 8 || character != ResupplyViewerBody() || g_hitProbeBusy.exchange(true)) {
		return;
	}
	g_hitProbes++;
	HANDLE t = CreateThread(nullptr, 0, HitProbeThread, new HitProbeArgs{(uint8_t*)character, kind}, 0, nullptr);
	if (t) {
		CloseHandle(t);
	}
	else {
		g_hitProbeBusy = false;
	}
}

// Soldiers in versus only (the rules are VS rules; VS and Akrid keep the game's). Refills (positive) pass as they are.
template <int Pool>
bool __attribute__((thiscall)) StaggerAdd(void* character, uint32_t delta, uint32_t b)
{
	int16_t points = (int16_t)delta;
	if (points >= 0 || ((uint8_t*)character)[0x1f] != 1) {
		return g_staggerOriginal[Pool](character, delta, b);
	}
	int kind = StaggerKindOf(Pool);
	DWORD now = GetTickCount();
	HitProbe(character, kind);
	if (kind == kStaggerBullet && Pool == 1) {
		// Massive Shot: bullets into the knock-back pool. The threshold divides the points (2x: twice the pellets).
		int massive = g_massive % kMassiveValues;
		float threshold = kThresholdScales[g_massiveThreshold % kThresholdValues];
		if (massive == kMassiveOff || threshold > 1000.0f) {
			return true;
		}
		int16_t scaled = (int16_t)std::max(-30000.0f, points / threshold);
		if (scaled == 0) {
			return true;
		}
		bool standing = g_staggerOriginal[1](character, (uint16_t)scaled, b);
		if (!standing && (massive == kMassiveBackPull || massive == kMassiveBackPush)) {
			KnockdownThrow(character, massive == kMassiveBackPull, "massive shot");
		}
		if (!standing) {
			if (massive == kMassiveDown && g_staggerOriginal[2]) {
				g_staggerOriginal[2](character, (uint16_t)(int16_t)-30000, b);
			}
			StaggerLog("mods: rules: hit stagger: a massive %s shot: %s.", kStaggerKindNames[kind], kMassiveNames[massive]);
		}
		return standing;
	}
	int setting = (kind == kStaggerExplosion ? g_staggerExplosion : kind == kStaggerAnchor ? g_staggerAnchor : kind == kStaggerMelee ? g_staggerMelee : g_stagger) % kStaggerValues;
	t_hitAt = now;
	t_hitQuiet = setting == kReactOff || (setting == kReactOccasional && now - StaggerSeenGet(character).reacted < kOccasionalMs);
	t_noFlinch = t_noFlinch || t_hitQuiet;
	switch (setting) {
	case kReactDefault: {
		bool standing = g_staggerOriginal[Pool](character, delta, b);
		if (!standing) {
			StaggerLog("mods: rules: hit stagger: a %s hit: the game's %s.", kStaggerKindNames[kind], kStaggerPools[Pool]);
		}
		return standing;
	}
	case kReactOff:
		return true;
	case kReactOccasional: {
		if (now - StaggerSeenGet(character).reacted < kOccasionalMs) {
			return true;
		}
		bool standing = g_staggerOriginal[Pool](character, delta, b);
		if (!standing) {
			StaggerSeenSet(character, true, false, now);
			StaggerLog("mods: rules: hit stagger: a %s hit: occasional %s.", kStaggerKindNames[kind], kStaggerPools[Pool]);
		}
		return standing;
	}
	default: {
		// one reaction per hit, whatever its points: the target pool emptied (and the knockdown call, which only the hit
		// handler makes), and the answers say "out" up to the target so the handler's level matches
		int target = setting == kReactFlinch ? 0 : setting == kReactDown ? 2 : 1;
		if (!g_staggerOriginal[target] || (target == 2 && !g_knockdown)) {
			return g_staggerOriginal[Pool](character, delta, b);
		}
		bool first = now - StaggerSeenGet(character).forced >= kStaggerSameHitMs;
		if (first) {
			StaggerSeenSet(character, true, true, now);
			g_staggerOriginal[target](character, (uint16_t)(int16_t)-30000, b);
			if (target == 2 && Pool != 2) {
				g_knockdown(character);
			}
			if (setting == kReactBackPull || setting == kReactBackPush) {
				KnockdownThrow(character, setting == kReactBackPull, kStaggerKindNames[kind]);
			}
			StaggerLog("mods: rules: hit stagger: a %s hit: %s.", kStaggerKindNames[kind], kStaggerNames[setting]);
			return Pool > target;
		}
		return Pool == 2 ? true : Pool > target; // the same hit's later pools (the third: its knockdown is done)
	}
	}
}

void StaggerInstall(uint8_t* base, int build)
{
	if (build >= 0 && build < kBuildCount) {
		const uint8_t kd[] = {0x56, 0x8b, 0xf1}; // push esi; mov esi,ecx
		uint8_t* at = base + (kStaggerKnockdown[build] - 0x400000);
		g_knockdown = memcmp(at, kd, sizeof(kd)) == 0 ? (KnockdownFn)at : nullptr;
		const uint8_t shake[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0}; // push ebp; mov ebp,esp; and esp,-16
		g_hitHandlerOriginal = (HitHandlerFn)Detour(base + (kHitHandler[build] - 0x400000), shake, sizeof(shake), (void*)&HitHandler);
		if (!g_hitHandlerOriginal) {
			XLS_LOG_WARN("mods: rules: hit handler not hooked: knockdown throws go straight up, no fall kill credit.");
		}
		g_hitShakeOriginal = (HitShakeFn)Detour(base + (kHitShake[build] - 0x400000), shake, sizeof(shake), (void*)&HitShake);
		const uint8_t death[] = {0x83, 0xec, 0x14, 0x55, 0x8b, 0x6c, 0x24, 0x1c}; // sub esp,0x14; push ebp; mov ebp,[esp+0x1c]
		g_deathThinkOriginal = (DeathThinkFn)Detour(base + (kDeathThink[build] - 0x400000), death, sizeof(death), (void*)&DeathThink);
		if (!g_deathThinkOriginal) {
			XLS_LOG_WARN("mods: rules: death think not hooked, falls after a hit stay suicides.");
		}
		if (!g_knockdown || !g_hitShakeOriginal) {
			XLS_LOG_WARN("mods: rules: stagger knockdown %s, hit shake %s.", g_knockdown ? "found" : "not found", g_hitShakeOriginal ? "hooked" : "not hooked");
		}
	}
	g_staggerBuild = build >= 0 && build < kBuildCount ? build : -1;
	bool dx9 = build == 1 || build == 3;
	const uint8_t prologue[] = {0x56, 0x8b, 0xf1, 0x8b, 0x8e, (uint8_t)(dx9 ? 0xb4 : 0xc4), 0x0c, 0x00, 0x00}; // push esi; mov esi,ecx; mov ecx,[esi+0xcc4]
	const uint8_t third[] = {0x56, 0x8b, 0xf1, 0x83, 0xbe, (uint8_t)(dx9 ? 0xe0 : 0xf0), 0x17, 0x00, 0x00, 0x00}; // push esi; mov esi,ecx; cmp [esi+0x17f0],0
	void* hooks[3] = {(void*)&StaggerAdd<0>, (void*)&StaggerAdd<1>, (void*)&StaggerAdd<2>};
	for (int i = 0; i < 3; i++) {
		uint8_t* at = base + (kStaggerAdd[build] + kStaggerPoolOffset[i] - 0x400000);
		g_staggerOriginal[i] = i == 2 ? (StaggerAddFn)Detour(at, third, sizeof(third), hooks[i]) : (StaggerAddFn)Detour(at, prologue, sizeof(prologue), hooks[i]);
		if (!g_staggerOriginal[i]) {
			XLS_LOG_WARN("mods: rules: stagger %s add not hooked, the Hit Stagger row doesn't cover it.", kStaggerPools[i]);
		}
	}
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
			// Survival only where the lobby's match is Survival: the rules come from the create screen's last
			// state, which a lobby set up another way (System Link, a later Battle Gauge / Points / VS match)
			// doesn't change
			// The match's own rule and sub-type: the bytes the lobby's panel shows (slots +0x74, +0x75, copied from rules 0x16, 0x17
			// when the session is made). The advertised properties don't follow them: Elimination / Kills advertised 1, 0.
			int8_t rule = -1, sub = -1;
			uint8_t* slots = nullptr;
			bool known = g_ruleSlots && ReadGame(g_ruleSlots, &slots) && slots && ReadGame(slots + 0x74, &rule) && ReadGame(slots + 0x75, &sub);
			if (r.survival && (!known || rule < 0 || rule > 1 || sub != kSubKills)) {
				static int loggedRule = -2, loggedSub = -2;
				if (rule != loggedRule || sub != loggedSub) {
					loggedRule = rule;
					loggedSub = sub;
					XLS_LOG_INFO("mods: rules: survival dropped: the match says rule %d, sub %d.", rule, sub);
				}
				r.survival = false;
				r.lives = 0;
				r.team = false;
			}
		}
		else if (r.survival) {
			// No lobby to ask (System Link): the create screen's last state is no guide either, Survival only
			// while the match being played is scored by kills (the game's own state)
			uint8_t* players = nullptr;
			int32_t mode = 0;
			if (!ReadGame(g_players, &players) || !ReadGame(players + kPlayersListMode, &mode) || mode != kListKills) {
				r.survival = false;
				r.lives = 0;
				r.team = false;
			}
		}
		// The rules are VS match settings: none in the Campaign (the presence context RICH_MODE, id 0, is
		// SINGLE 0 / COOP 1 / CAMPAIGN 5 there; Online, System Link and Training have their own values)
		DWORD mode = 0;
		if (UserGetContext(0, kContextRichMode, &mode) && (mode <= 1 || mode == 5)) {
			r = Rules();
		}
		if (g_offline && g_botArmed) { // a training bot match: its menu's rows only, not the last VS lobby's rules
			r = Rules();
			if (g_botSurvival && g_botRule <= 1) {
				r.survival = true;
				r.lives = kLives[g_botLives % kLivesValues];
				r.team = g_botRule == 1;
			}
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
		if (r.survival && !g_survival.load() && !g_survivalDropped.exchange(true)) {
			XLS_LOG_INFO("mods: rules: the Survival row is ignored now: Survival is a game mode (Modes\\Survival.lp2mode via mode.txt).");
		}
		g_survival = false;   // was r.survival: the stock rules' Survival is replaced by the Survival game mode
		g_lives = r.lives;
		HarmonizerSet(r.harmonizer);
		g_grapple = r.grapple;
		bool versus = g_game && *g_game && (*g_game)[0x256] != 0; // these two leave the Campaign as it is
		g_anchorSpeed = kAnchorSpeedStock * (versus ? kGrappleScales[(r.anchorSpeed + 1) % kGrappleValues] : 1.0f);
		g_fallThreshold = kFallStock * (versus ? kThresholdScales[r.fall % kThresholdValues] : 1.0f);
		g_grappleLimit = kGrappleStock * kGrappleScales[r.grapple % kGrappleValues];
		g_stagger = r.stagger;
		g_staggerExplosion = r.staggerExplosion;
		g_staggerAnchor = r.staggerAnchor;
		g_staggerMelee = r.staggerMelee;
		g_massive = r.massive;
		g_massiveThreshold = r.massiveThreshold;
		g_push = r.push;
		g_grenadeCount = r.grenades ? 0 : r.grenadeCount; // infinite grenades: the counts are hidden and mean nothing
		g_grenadePickup = r.grenades ? 0 : r.grenadePickup;
		RadarRulesSet(r.radar, r.detectPlayers, r.detectVs, r.radarTeam);
		ResupplySet(r.quickResupply != 0);
		g_rounds = r.rounds;
		g_switchEvery = r.switchEvery;
		g_team = r.team;
		std::string now = Format(r);
		if (now != logged) {
			logged = now;
			XLS_LOG_INFO("mods: rules: unlimited ammo %s, unlimited grenades %s, secondary kind 0x%02x, ammo supply %d magazines (0 = default), survival %s (%d lives), harmonizer %s, grappling %s, stagger bullet %s / explosion %s / anchor %s / melee %s, massive shot %s at %s.", r.ammo ? "on" : "off", r.grenades ? "on" : "off", r.secondary, r.supply, r.survival ? "on" : "off", r.lives, r.harmonizer ? "on" : "off", kGrappleNames[r.grapple % kGrappleValues], kStaggerNames[r.stagger % kStaggerValues], kStaggerNames[r.staggerExplosion % kStaggerValues], kStaggerNames[r.staggerAnchor % kStaggerValues], kStaggerNames[r.staggerMelee % kStaggerValues], kMassiveNames[r.massive % kMassiveValues], kThresholdNames[r.massiveThreshold % kThresholdValues]);
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
using LocalSlotFn = int(__attribute__((thiscall)) *)(void*);
LocalSlotFn g_localSlot = nullptr;
void** g_net = nullptr;
std::atomic<int> g_localIndex{-1}; // the local player's match slot
std::atomic<bool> g_out{false};     // Survival: the local player has no lives left
// Team Survival: the slots out (bit by slot). The team's respawns are shared (lives - 1, as one
// player's); once they are used up each player's next death is their last, the others play on.
std::atomic<uint32_t> g_teamOut{0};

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
// Rounds: controllers dead or held out when a round ended (bit per slot), put back in play when the next round starts;
// left alone the death view carries on into the new round, counts down and respawns, which costs a death.
std::atomic<uint32_t> g_roundDead{0};
std::atomic<DWORD> g_roundStartAt{0};

// Spectating: controller +0x480 is the player (active when its +4 & 7 is 1 or 2), whose +0x40 is
// the body the camera follows; +0x484 the camera (only the local player's), following the body at
// +0x200 once told through follow (as the respawn does).
using FollowFn = void(__attribute__((thiscall)) *)(uint8_t*, uint8_t*, int, float, float);
FollowFn g_follow = nullptr;
using CameraSnapFn = void(__attribute__((thiscall)) *)(uint8_t*, uint8_t*);
CameraSnapFn g_cameraSnap = nullptr; // places the camera on what it follows at once (the respawn calls it after the follow; DX11 FUN_008f0810)
const uint32_t kCameraSnap[4] = {0x8f0810, 0x8e0eb0, 0x8f0af0, 0x8e1060};
const float* g_followArg = nullptr;
uint8_t** g_controllers = nullptr;
const uint32_t kCtlPlayer = 0x480, kCtlCamera = 0x484, kPlayerBody = 0x40, kCameraBody = 0x200;
int g_watched = -1;
uint8_t g_slotState[16]; // each slot's controller state, as its update left it

uint8_t* Body(int slot)
{
	uint8_t* ctl = g_controllers[slot];
	uint8_t* player = ctl ? *(uint8_t**)(ctl + kCtlPlayer) : nullptr;
	uint32_t kind = (uintptr_t)player >= 0x10000 ? *(uint32_t*)(player + 4) & 7 : 0; // junk (8) seen at a death
	return kind == 1 || kind == 2 ? *(uint8_t**)(player + kPlayerBody) : nullptr;
}

// A slot worth watching: in play, alive (state 0) and, in Survival, with lives left (its team's in Team Survival).
int32_t SlotTeam(int slot)
{
	uint8_t* setup = *g_matchSetup;
	return setup && slot >= 0 && slot < 16 ? *(int32_t*)(setup + kSetupSlots + slot * kSetupSlot) : -1;
}

// Survival: the slot's deaths so far (synced to every PC), 0 when unknown.
int SlotDeaths(int slot)
{
	uint8_t *mgr = *g_matchMgr ? (uint8_t*)*g_matchMgr : nullptr, *owner = mgr ? *(uint8_t**)(mgr + kMgrInfoOwner) : nullptr;
	return owner && slot >= 0 && slot < 16 ? ((int32_t*)(owner + kOwnerInfoGame + kInfoDiePerson))[slot] : 0;
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
	return (g_teamOut >> slot) & 1;
}

// Team Survival: the team's deaths, summed from its players' (the order they came in decides who is out).
int TeamDeaths(int team)
{
	int deaths = 0;
	for (int slot = 0; slot < 16; slot++) {
		deaths += SlotTeam(slot) == team ? SlotDeaths(slot) : 0;
	}
	return deaths;
}

// Survival: the battle messages say Eliminated for a death with no respawn behind it. The game counts the death before
// it raises the message, and only the template differs, so this runs on the PC showing the message and needs nothing
// synced. The formatter asks the message lookup for the template; maps.cpp's hook on it gives ours for that one call
// (RulesBattleMessage; a function can only carry one detour).
struct EliminatedText {
	int id;
	const char* text;
};
const EliminatedText kEliminatedTexts[] = {
	{0, "Eliminated\n<$DeadPlayerName>"},
	{1, "Eliminated\n<$DeadPlayerName> (VS)"},
	{2, "Eliminated by\n<$KillPlayerName>"},
	{3, "Eliminated by\n<$KillPlayerName> (VS)"},
	{4, "Self-Eliminated"},
	{14, "Self-Eliminated"},
	{45, "<$KillPlayerName> eliminated\n<$DeadPlayerName>"},
	{46, "<$KillPlayerName> eliminated\n<$DeadPlayerName> (VS)"},
	{47, "<$KillPlayerName> (VS) eliminated\n<$DeadPlayerName>"},
	{48, "<$KillPlayerName> (VS) eliminated\n<$DeadPlayerName> (VS)"},
	{49, "<$DeadPlayerName>\nself-eliminated"},
	{60, "<$DeadPlayerName>\nself-eliminated"},
};
const uint32_t kBattleFormat[kBuildCount] = {0x8b06b0, 0x8a1000, 0x8b0a30, 0x8a1260};
// push ebx; push esi; mov esi,[esp+0xc]; mov ebx,ecx
const uint8_t kBattleFormatPrologue[] = {0x53, 0x56, 0x8b, 0x74, 0x24, 0x0c, 0x8b, 0xd9};
typedef void(__attribute__((thiscall)) * BattleFormatFn)(void* self, char* out, int id, uint8_t* data);
BattleFormatFn g_battleFormatOriginal = nullptr;
const char* g_eliminatedText = nullptr;
int g_eliminatedId = -1;

// The death in a message's data had no respawn behind it: the type at +0x20 is 0 for a player, the slot follows.
bool MessageEliminates(const uint8_t* data)
{
	if (!g_survival || g_lives <= 0 || !data || *(const int32_t*)(data + 0x20) != 0) {
		return false;
	}
	int slot = *(const int32_t*)(data + 0x24);
	if (slot < 0 || slot >= 16) {
		return false;
	}
	return g_team ? TeamDeaths(SlotTeam(slot)) >= g_lives : SlotDeaths(slot) >= g_lives;
}

extern bool g_roundBreak;
extern DWORD g_roundBegan;

void __attribute__((thiscall)) BattleFormat(void* self, char* out, int id, uint8_t* data)
{
	// Rounds: deaths from the break and the reload (the deaths count until the reload clears them) are no eliminations
	bool between = g_roundBreak || (g_roundBegan && GetTickCount() - g_roundBegan < 8000);
	if (between) {
		XLS_LOG_INFO("mods: rules: rounds: battle message %d between rounds (slot %d).", id, data ? *(const int32_t*)(data + 0x24) : -1);
	}
	for (const EliminatedText& t : kEliminatedTexts) {
		if (!between && t.id == id && MessageEliminates(data)) {
			g_eliminatedId = id;
			g_eliminatedText = t.text;
			break;
		}
	}
	g_battleFormatOriginal(self, out, id, data);
	g_eliminatedText = nullptr;
}

// The team byte on a slot's body (the one the game fights by; radar.cpp's team), -1 with no body. The out player's own
// body is gone, so its last one seen is kept.
const uint32_t kUnitTeam[4] = {0xce2, 0xcd2, 0xce2, 0xcd2};
uint32_t g_unitTeam = 0;
int g_selfTeam[16];
bool g_selfTeamSet = false;

int UnitTeam(int slot)
{
	uint8_t* body = g_unitTeam ? Body(slot) : nullptr;
	int team = body ? body[g_unitTeam] : -1;
	if (!g_selfTeamSet) {
		g_selfTeamSet = true;
		for (int& t : g_selfTeam) {
			t = -1;
		}
	}
	if (slot >= 0 && slot < 16 && team >= 0 && g_slotState[slot] == 0) { // a dead body's byte is no team to go by
		g_selfTeam[slot] = team;
	}
	return slot >= 0 && slot < 16 ? g_selfTeam[slot] : -1;
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
	int keep = LivesPickWatch(self, g_watched, 0);
	if (dir || g_watched < 0 || keep != g_watched) {
		int next = LivesPickWatch(self, g_watched, dir ? dir : 1);
		if (next < 0 && g_watched >= 0 && Body(g_watched)) {
			next = g_watched; // the only one left, or the last one standing: stay on them
		}
		if (next != g_watched) {
			XLS_LOG_INFO("mods: rules: survival: spectating player %d (team %d, own %d).", next, LivesTeam(next), LivesTeam(self));
		}
		g_watched = next;
	}
	LivesWatching(g_watched);
	uint8_t* body = g_watched >= 0 ? Body(g_watched) : nullptr;
	if (body && (force || *(uint8_t**)(cam + kCameraBody) != body)) { // forced after a held respawn: its pick view takes the camera
		g_follow(cam, body, 0xff, *g_followArg, 0.0f);
		if (g_cameraSnap) {
			g_cameraSnap(cam, nullptr); // no slow glide over from the last one watched
		}
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
	extern bool g_roundBreak;
	if (slot >= 16 || !body || ctl[kCtlState] != kStateDeath || g_roundBreak) { // the next round's reload takes it
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

// Held players (lives.cpp decides: a waiting line, a mode's Hold player): out of play without being out of the match.
// A held slot's character is removed and its controller kept in the held death view (as an out player's); a release
// spawns it the way a round's start does (the spawn with no weapons handed over), then puts it at the chosen point.
// Each PC does this for the players it runs (its own; the host the bots; offline everyone), on the game thread (HudUpdate).
std::atomic<uint32_t> g_heldMask{0};
struct ReleaseReq {
	bool pending = false, at = false;
	float pos[3] = {};
	float yaw = NAN;
};
std::mutex g_releaseMutex;
ReleaseReq g_releaseReq[16];
bool Host();

bool RunsHere(int slot)
{
	int self = g_localIndex;
	if (slot == self || g_offline) {
		return true;
	}
	uint8_t* records = g_ruleSlots ? *g_ruleSlots : nullptr;
	return records && records[0x1b1 + slot * 0x7c] && Host(); // a bot, on the host
}

uint8_t* PlayerOf(int slot)
{
	uint8_t* entry = slot >= 0 && slot < 16 ? g_controllers[slot] : nullptr;
	return entry ? *(uint8_t**)(entry + kCtlPlayer) : nullptr;
}

} // the API below is lp2.h's

void RulesHold(int slot, bool hold)
{
	if (slot < 0 || slot >= 16) {
		return;
	}
	uint32_t bit = 1u << slot;
	uint32_t was = hold ? g_heldMask.fetch_or(bit) : g_heldMask.fetch_and(~bit);
	if (((was & bit) != 0) != hold) {
		XLS_LOG_INFO("mods: rules: player %d %s.", slot, hold ? "held (out of play, waiting)" : "no longer held");
	}
}

bool RulesHeld(int slot)
{
	return slot >= 0 && slot < 16 && ((g_heldMask >> slot) & 1);
}

void RulesEliminate(int slot, bool out)
{
	if (slot < 0 || slot >= 16) {
		return;
	}
	uint32_t bit = 1u << slot;
	uint32_t was = out ? g_elimMask.fetch_or(bit) : g_elimMask.fetch_and(~bit);
	if (((was & bit) != 0) != out) {
		XLS_LOG_INFO("mods: rules: player %d %s.", slot, out ? "eliminated" : "back in the match");
		g_elimOrder[slot] = out ? ++g_elimCount : 0;
	}
	if (out) {
		RulesHold(slot, true);
	}
}

bool RulesEliminated(int slot)
{
	return slot >= 0 && slot < 16 && ((g_elimMask >> slot) & 1);
}



void RulesRelease(int slot, const float* pos, float yaw)
{
	if (slot < 0 || slot >= 16) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_releaseMutex);
	ReleaseReq& r = g_releaseReq[slot];
	r.pending = true;
	r.at = pos != nullptr;
	if (pos) {
		memcpy(r.pos, pos, sizeof(r.pos));
	}
	r.yaw = yaw;
}

namespace {

uint32_t __attribute__((thiscall)) Spawn(uint8_t* spawner, uint32_t respawn, void* a, void* b);

void HeldTick()
{
	uint32_t held = g_heldMask;
	for (int slot = 0; slot < 16; slot++) {
		uint8_t* player = PlayerOf(slot);
		if (!player || !RunsHere(slot)) {
			continue;
		}
		ReleaseReq r;
		{
			std::lock_guard<std::mutex> lock(g_releaseMutex);
			r = g_releaseReq[slot];
			g_releaseReq[slot].pending = false;
		}
		float* timers = (float*)(player + kCtlDeathTimers);
		if (r.pending) {
			g_heldMask &= ~(1u << slot);
			if (!*(uint8_t**)(player + kCtlBody)) {
				Spawn(player, 0, nullptr, nullptr); // a round start's spawn: fresh character, starting loadout
			}
			player[kCtlState] = 0;
			timers[0] = timers[1] = 0.0f;
			uint8_t* body = *(uint8_t**)(player + kCtlBody);
			if (body && r.at) {
				CharacterTeleport(body, r.pos, r.yaw);
			}
			uint8_t* cam = *(uint8_t**)(g_controllers[slot] + kCtlCamera);
			if (slot == g_localIndex && cam && body && g_follow) {
				g_follow(cam, body, 0xff, *g_followArg, 0.0f);
				if (g_cameraSnap) {
					g_cameraSnap(cam, nullptr);
				}
				g_watched = -1;
			}
			XLS_LOG_INFO("mods: rules: player %d released%s (body %p).", slot,
			             r.at ? FormatA(" at %.0f %.0f %.0f", r.pos[0], r.pos[1], r.pos[2]).c_str() : "", body);
			continue;
		}
		if (!((held >> slot) & 1)) {
			continue;
		}
		if (uint8_t* body = *(uint8_t**)(player + kCtlBody)) {
			bool watchedByCamera = false;
			for (int i = 0; i < 16; i++) {
				uint8_t* cam = g_controllers[i] ? *(uint8_t**)(g_controllers[i] + kCtlCamera) : nullptr;
				watchedByCamera = watchedByCamera || (i != slot && cam && *(uint8_t**)(cam + kCameraBody) == body);
			}
			if (watchedByCamera) {
				continue; // a spectator moves on first
			}
			using RemoveFn = void(__attribute__((thiscall)) *)(uint8_t*);
			((RemoveFn)(*(void***)body)[kBodyRemoveSlot])(body);
			*(uint8_t**)(player + kCtlBody) = nullptr;
			XLS_LOG_INFO("mods: rules: player %d held: character removed.", slot);
		}
		player[kCtlState] = kStateDeath;
		timers[0] = timers[0] < 1.0f ? 1.0f : timers[0];
		timers[1] = timers[1] < 1.0f ? 1.0f : timers[1];
	}
}

// While the local player spectates, the cockpit's own-state parts are hidden (see CockpitShow; the
// game's cockpit-off status flag would take the whole HUD). The survival thread ends it once nobody
// spectates (match over, quit).
std::atomic<bool> g_hudHidden{false};
std::atomic<DWORD> g_hudTouched{0};

void HideHud(bool hide)
{
	if (hide) {
		g_hudTouched = GetTickCount();
	}
	if (hide == g_hudHidden) {
		return;
	}
	g_hudHidden = hide;
	XLS_LOG_INFO("mods: rules: survival: HUD %s.", hide ? "hidden while spectating" : "back");
}

// The battle HUD's player list (top left: rank, name, value) shows by game status +0x8174: 1 the
// Battle Gauge (results +0x103c, "%d"), 2 the kills (+0x193c, "%03d/%03d" with a kill target, else
// "%03d/-----"), 3 the points (+0x19bc). In Survival (free for all) it shows the respawns left instead,
// or ELIMINATED in red. Both its sprintf calls are redirected here (online Survival is Kills, so the
// target call too); the slot comes from the pointer the game holds in esi (into one of the arrays),
// the row's colour (ABGR) is the edi it draws with next.
const char* g_gaugeFormat = nullptr;
uint8_t** g_results = nullptr;
const uint32_t kResultsGauge = 0x103c, kResultsKills = 0x193c, kColourEliminated = 0xff3c3cff;

extern "C" int __attribute__((cdecl)) ListValue(char* buf, const char* format, int value, int value2, uint8_t* gauge, uint32_t* colour)
{
	uint8_t* results = g_results ? *g_results : nullptr;
	// the list shows the Battle Gauge (training, Battle Gauge matches) or the kills (Kills, which online
	// Survival is): either array gives the slot
	uint8_t* first = nullptr;
	for (uint32_t at : {kResultsGauge, kResultsKills}) {
		if (results && gauge >= results + at && gauge < results + at + 16 * 4) {
			first = results + at;
		}
	}
	static bool logged = false;
	if (g_survival && !g_team && !logged) {
		logged = true;
		XLS_LOG_INFO("mods: rules: survival: player list value at results+0x%x (%s).", results && gauge ? (unsigned)(gauge - results) : 0u,
		             first ? "shown as respawns" : "not a known array, left alone");
	}
	if (g_survival && !g_team && g_lives > 0 && first) {
		int slot = (int)(gauge - first) / 4;
		if (SlotOut(slot)) {
			*colour = kColourEliminated;
			return sprintf(buf, "ELIMINATED");
		}
		int left = std::max(g_lives - 1 - SlotDeaths(slot), 0); // 0 on the last life
		return left == 0 ? sprintf(buf, "Last Life") : sprintf(buf, left == 1 ? "%d Respawn" : "%d Respawns", left);
	}
	return sprintf(buf, format, value, value2); // the "%d" call ignores the second
}

// Its order: in Survival (free for all) by lives left, then kills (results +0x193c), then the game's
// own rank; equal lives and kills share a rank. The list's calls of the results' rank accessors
// (count +0x9ecc, rank by slot +0x9ed0, slot by rank +0x9f10) are redirected here.
const uint32_t kResultsCount = 0x9ecc, kResultsRankOf = 0x9ed0, kResultsSlotAt = 0x9f10;

bool ListSorted()
{
	return g_survival && !g_team && g_lives > 0;
}

// The slots in list order and their ranks (from 1); the count.
int ListOrder(uint8_t* results, int8_t* slots, int* ranks)
{
	int n = *(int32_t*)(results + kResultsCount);
	n = n < 0 ? 0 : n > 16 ? 16 : n;
	int key[16];
	for (int i = 0; i < n; i++) {
		slots[i] = (int8_t)results[kResultsSlotAt + i];
		int slot = slots[i] & 15;
		int lives = SlotOut(slot) ? 0 : g_lives - SlotDeaths(slot);
		key[i] = lives * 10000 + std::min(std::max(*(int32_t*)(results + kResultsKills + slot * 4), 0), 9999);
	}
	for (int i = 1; i < n; i++) { // stable: the game's order breaks ties
		for (int j = i; j > 0 && key[j] > key[j - 1]; j--) {
			std::swap(key[j], key[j - 1]);
			std::swap(slots[j], slots[j - 1]);
		}
	}
	for (int i = 0; i < n; i++) {
		ranks[i] = i > 0 && key[i] == key[i - 1] ? ranks[i - 1] : i + 1;
	}
	return n;
}

extern "C" int __attribute__((thiscall)) ListSlotAt(uint8_t* results, int rank)
{
	if (!ListSorted()) {
		return rank >= 0 && rank < 16 ? (int8_t)results[kResultsSlotAt + rank] : -1;
	}
	int8_t slots[16];
	int ranks[16], n = ListOrder(results, slots, ranks);
	return rank >= 0 && rank < n ? slots[rank] : -1;
}

extern "C" int __attribute__((thiscall)) ListRankOf(uint8_t* results, int slot)
{
	slot = (int8_t)slot;
	if (slot < 0 || slot > 15) {
		return 0;
	}
	if (!ListSorted()) {
		return *(int32_t*)(results + kResultsRankOf + slot * 4);
	}
	int8_t slots[16];
	int ranks[16], n = ListOrder(results, slots, ranks);
	for (int i = 0; i < n; i++) {
		if (slots[i] == slot) {
			return ranks[i];
		}
	}
	return *(int32_t*)(results + kResultsRankOf + slot * 4);
}

// thiscall (results, arg) wrappers keeping ecx and edx, as the game's leaf accessors do.
extern "C" void __attribute__((naked)) ListSlotAtThunk()
{
	asm volatile("push %%ecx\n\tpush %%edx\n\tpushl 12(%%esp)\n\tcall %P0\n\tpop %%edx\n\tpop %%ecx\n\tret $4" ::"i"(ListSlotAt));
}

extern "C" void __attribute__((naked)) ListRankOfThunk()
{
	asm volatile("push %%ecx\n\tpush %%edx\n\tpushl 12(%%esp)\n\tcall %P0\n\tpop %%edx\n\tpop %%ecx\n\tret $4" ::"i"(ListRankOf));
}

// Team Survival: the list's draw returns at once for team rules (and still runs with the cockpit off,
// while spectating), so its text call writes a line per
// team instead, beside the team kill counters (flash\cockpit\id762 mc_Kill, top left): respawns
// left, then the players still up, then Last Man Standing. The list builds its text style from a
// 16-dword template plus constants, and the call is thiscall(text, list, string, {x, y, 12, 12},
// 0, colour ABGR, style, 0, scale); the globals it uses are read from its own instructions.
using ListDrawFn = void(__attribute__((thiscall)) *)(uint8_t*);
using TextFn = void(__attribute__((thiscall)) *)(void*, uint8_t*, const char*, int32_t*, int, uint32_t, uint32_t*, int, float);
ListDrawFn g_listDrawOriginal = nullptr;
TextFn g_text = nullptr;
void** g_textCtx = nullptr;
const uint32_t* g_textStyle = nullptr;
const uint16_t* g_textFont = nullptr;
const uint32_t kListStyleAt = 0x143, kListFontAt = 0x272, kListCtxAt = 0x2d4, kListTextCall = 0x2d9; // from the list's start
const uint32_t kResultsTeamKills = 0x197c, kColourRed = 0xff3c3cff, kColourBlue = 0xffff8c3c;
std::atomic<int> g_teamRowsX{58}, g_teamRowsY{180}, g_teamRowsGap{13}; // team-hud.txt: x y gap (where the FFA list is)

bool InPlay(int slot);
bool CountersOurs();

// The list's text style (from its template), left aligned, small.
void ListTextStyle(uint32_t* style)
{
	memcpy(style, g_textStyle, 16 * sizeof(uint32_t));
	float* f = (float*)style;
	style[0] = 1; // left aligned (4: right)
	style[1] = (style[1] & 0xffff0000) | *g_textFont;
	style[2] = 1;
	f[5] = f[6] = 0.6f;
	f[7] = 1.13f;
	f[8] = f[9] = 3.0f;
	f[10] = 10.0f;
	f[11] = 0.0f;
	style[12] = style[13] = style[15] = 0;
	f[14] = 1.0f;
}

void TeamRows(uint8_t* list)
{
	uint8_t* results = g_results ? *g_results : nullptr;
	if (!g_survival || !g_team || g_lives <= 0 || !g_text || !*g_textCtx || !results) {
		return;
	}
	uint32_t style[16];
	ListTextStyle(style);
	// the leading team on top, as mc_Kill (red until blue has more kills)
	static bool blueTop = false;
	int32_t red = *(int32_t*)(results + kResultsTeamKills), blue = *(int32_t*)(results + kResultsTeamKills + 4);
	blueTop = red == 0 && blue == 0 ? false : blue > red ? true : red > blue ? false : blueTop;
	for (int row = 0; row < 2; row++) {
		int team = row ^ blueTop;
		int up = 0, members = 0;
		for (int slot = 0; slot < 16; slot++) {
			if (InPlay(slot) && SlotTeam(slot) == team) {
				members++;
				up += !SlotOut(slot);
			}
		}
		if (!members) {
			continue;
		}
		int left = std::max(g_lives - 1 - TeamDeaths(team), 0);
		char text[48];
		if (left > 0) {
			sprintf(text, left == 1 ? "%d Respawn" : "%d Respawns", left);
		} else if (up > 1) {
			sprintf(text, "%d Alive", up);
		} else {
			sprintf(text, up == 1 ? "Last Man Standing" : "ELIMINATED");
		}
		int32_t rect[4] = {g_teamRowsX, g_teamRowsY + row * g_teamRowsGap, 12, 12};
		g_text(*g_textCtx, list, text, rect, 0, team ? kColourBlue : kColourRed, style, 0, 1.0f);
	}
}

// Survival hides the team gauge bar: its update (which shows it for Team Elimination) is skipped, the
// match start's reset hides it.
ListDrawFn g_teamBarOriginal = nullptr;

void __attribute__((thiscall)) TeamBar(uint8_t* cockpit)
{
	if (!g_survival || !g_team || g_lives <= 0) {
		g_teamBarOriginal(cockpit);
	}
}

// Spectating hides the cockpit clips that show the player's own state (each clip's _visible, not the
// movie's: the movies hold more): the HUD manager (the list's owner) holds the movies as wrappers at
// +0x38..+0x64 (Flash at +0x24), with these clips: id750 LIFE, id751 weapons and T-ENG, id752 crosshair, id753 radar, id755
// Battle Gauge, prompts and action text, id756/757 scopes. The list, messages (id759) and the team
// counters stay. Applied on the list's draw (the game thread, every frame, also while spectating).
struct FlashValue {
	uint8_t flag;
	uint32_t type; // 2 bool, 3 number
	float number;
};
using FlashGetFn = bool(__attribute__((thiscall)) *)(uint8_t*, const char*, FlashValue*);
using FlashFreeFn = void(__attribute__((thiscall)) *)(FlashValue*);
FlashGetFn g_flashGet = nullptr, g_flashSet = nullptr;
FlashFreeFn g_flashFree = nullptr;
const uint32_t kHudMoviesFirst = 0x38, kHudMoviesLast = 0x64, kMovieFlash = 0x24;
const char* const kSpectateHide[] = {"_root.mc_cockpitLife._visible", "_root.mc_WPL._visible", "_root.mc_Lockon._visible", "_root.mc_NWSE._visible", "_root.mc_BattleGauge._visible", "_root.mc_TargetScope._visible"};

int CockpitShow(uint8_t* hud, bool show)
{
	// the clips themselves, not their movies' _root: a movie also holds other things the spectator needs
	// (id755: the prompts, next / previous player among them)
	int count = 0;
	for (uint32_t at = kHudMoviesFirst; at <= kHudMoviesLast && g_flashGet; at += 4) {
		uint8_t* movie = *(uint8_t**)(hud + at);
		if (!movie || !*(void**)(movie + kMovieFlash)) {
			continue;
		}
		for (const char* name : kSpectateHide) {
			FlashValue value = {};
			bool there = g_flashGet(movie, name, &value) && value.type == 2; // the clip's _visible: there
			g_flashFree(&value);
			if (there) {
				FlashValue v = {0, 3, show ? 1.0f : 0.0f};
				g_flashSet(movie, name, &v);
				g_flashFree(&v);
				count++;
			}
		}
	}
	return count;
}

void __attribute__((thiscall)) ListDraw(uint8_t* list)
{
	g_listDrawOriginal(list);
	static std::atomic<int> displayLogged{-1};
	int display = (g_survival ? 1 : 0) | (g_team ? 2 : 0) | (CountersOurs() ? 4 : 0);
	if (displayLogged.exchange(display) != display) {
		XLS_LOG_INFO("mods: rules: survival display: survival %d, team %d, lives %d, counters ours %d, local slot %d.", display & 1, (display >> 1) & 1, (int)g_lives, (display >> 2) & 1, (int)g_localIndex);
	}
	if (!CountersOurs()) { // the team counters show the respawns when hooked
		TeamRows(list);
	}
	static bool hidden = false;
	static DWORD applied = 0;
	bool hide = g_survival && g_hudHidden;
	if (hide != hidden || (hide && GetTickCount() - applied > 250)) { // again now and then: movies load late, the game shows clips again
		int count = CockpitShow(list, !hide);
		if (hide != hidden) {
			XLS_LOG_INFO("mods: rules: survival: %d cockpit clips %s.", count, hide ? "hidden while spectating" : "shown");
		}
		hidden = hide;
		applied = GetTickCount();
	}
}

// The HUD manager's update (every frame, game thread) shows nothing once the player of its entry
// (+0x2c, the local slot's) has no body: so while the local player is out and watching someone, the
// update runs on the watched player's entry (their HUD; no death view). With its body removed the out
// player gets no controller update of its own: its spectating goes on here.
ListDrawFn g_hudUpdateOriginal = nullptr;
const uint32_t kHudEntry = 0x2c;
extern std::atomic<DWORD> g_localSeen;

void DeathProbe(uint8_t* ctl);

std::atomic<DWORD> g_hudSeenAt{0}; // the battle HUD's last update: our overlay's HUD shows only while it runs
void ReportLife(int slot);
bool InPlay(int slot);
bool TeamGame();

void __attribute__((thiscall)) HudUpdate(uint8_t* hud)
{
	g_hudSeenAt = GetTickCount();
	int self = g_localIndex;
	HeldTick();
	LivesMatch(self, (g_survival && g_lives > 0) || ModeLivesLeft(self) >= 0, TeamGame());
	// every slot, every frame: the controller update only runs while a player is dead
	for (int slot = 0; slot < 16; slot++) {
		if (InPlay(slot)) {
			ReportLife(slot);
		}
	}
	LivesTick();
	ModeFrame();
	JeepFrame();
	uint8_t** entry = (uint8_t**)(hud + kHudEntry);
	uint8_t* own = *entry;
	bool out = self >= 0 && self < 16 && own && own == g_controllers[self] && ((g_survival && SlotOut(self)) || RulesHeld(self));
	if (out && GetTickCount() - g_localSeen > 100) {
		static DWORD logged = 0;
		if (GetTickCount() - logged > 10000) {
			logged = GetTickCount();
			XLS_LOG_INFO("mods: rules: survival: player %d spectates from the HUD update (watching %d).", self, g_watched);
		}
		Spectate(own, self, false);
		HideHud(true);
	}
	int watched = g_watched;
	bool swap = out && watched >= 0 && watched < 16 && watched != self && g_controllers[watched] && Body(watched);
	if (swap) {
		*entry = g_controllers[watched];
	}
	g_hudUpdateOriginal(hud);
	if (swap) {
		*entry = own;
	}
	if (own && !out) {
		DeathProbe(own); // every frame of the match (the controller update only runs while dead)
	}
}

// Points a `call rel32` at the game's code to ours (checked: it called `expect`).
bool RedirectCall(uint8_t* at, uint8_t* expect, void* to)
{
	if (at[0] != 0xe8 || at + 5 + *(int32_t*)(at + 1) != expect) {
		return false;
	}
	int32_t rel = (int32_t)((uint8_t*)to - (at + 5));
	DWORD old;
	if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	memcpy(at + 1, &rel, 4);
	VirtualProtect(at + 1, 4, old, &old);
	return true;
}

// cdecl (buf, format, value, value2) from the list (the "%d" call's value2 is whatever is next on its
// stack: unused), plus esi and the address of the saved edi.
extern "C" void __attribute__((naked)) ListValueThunk()
{
	asm volatile(
	    "push %%edi\n\t"
	    "mov %%esp, %%eax\n\t"
	    "push %%eax\n\t"
	    "push %%esi\n\t"
	    "pushl 28(%%esp)\n\t"
	    "pushl 28(%%esp)\n\t"
	    "pushl 28(%%esp)\n\t"
	    "pushl 28(%%esp)\n\t"
	    "call %P0\n\t"
	    "add $24, %%esp\n\t"
	    "pop %%edi\n\t"
	    "ret" ::"i"(ListValue));
}

std::atomic<DWORD> g_localSeen{0}; // the local player's last controller update

// Secondary: the first spawn (Spawn, flag 0) creates the weapons from the ids in the player info, so
// the id goes into slot 7 there. A respawn (flag 1) takes slots 0, 1 and 4-7 from weapon objects its
// controller requested once, at match start (the loadout steps, DX11 FUN_008fa660), and slot 7's is
// none outside two stock modes; with a secondary set, Spawn points the respawn's slot 7 at the info
// id case slots 2 and 3 use (a 2-byte jump, only for that call).
uint32_t g_secondaryGiven[16]; // the id we put in each player's slot 7 (it stays in the player info)

void PutSecondary(uint8_t index, const char* when)
{
	if (index >= 16 || !g_game || !*g_game) {
		return;
	}
	BotMatchLogSlots();
	auto* info = *(uint8_t**)(*g_game + 0x28 + index * 4);
	auto* second = info ? (uint32_t*)(info + 0x10 + kSlotSecond * 4) : nullptr;
	if (info && strcmp(when, "spawns") == 0) { // the loadout each player (bots too) starts with: weapon ids 0x04KK00VV, KK kind
		const uint32_t* w = (const uint32_t*)(info + 0x10);
		XLS_LOG_INFO("mods: rules: player %u loadout %08x %08x %08x %08x %08x %08x %08x %08x.", index, w[0], w[1], w[2], w[3], w[4],
		             w[5], w[6], w[7]);
	}
	int kind = g_secondary;
	uint32_t want = kind ? 0x04000000u | (uint32_t)kind << 16 : 0;
	if (!second) {
		return;
	}
	if (!want) {
		if (g_secondaryGiven[index] && *second == g_secondaryGiven[index]) {
			*second = 0; // ours from an earlier match with other rules
		}
	}
	else if (*second != want) {
		XLS_LOG_INFO("mods: rules: player %u %s with secondary 0x%08x (slot held 0x%08x).", index, when, want, *second);
		*second = want;
	}
	g_secondaryGiven[index] = want;
}

// Death probe (finding how the game kills a falling player, for custom maps' kill floor): while
// ModCache\deathprobe.on exists, the local controller (0x600 B), its player (+0x480, 0x1000 B) and
// body (player +0x40, 0x3000 B) are copied every 100 ms into a 12 s ring; when the
// controller update next runs for the local player (it only runs while dead; the ring is fed from the
// HUD update, every frame alive) the ring goes to ModCache\deathprobe\death-<n>.bin (oldest first), and F8
// writes the latest snapshot alone to snap-<n>.bin (on foot vs riding something, to compare):
//   'LPDP' u32 count; per snapshot: u32 tick, u32 state, u32 controller, player, body addresses,
//   then the three copies (unreadable bytes are zero).
struct ProbeShot {
	DWORD tick;
	uint32_t state, ctl, player, body;
	uint8_t c[0x600], p[0x1000], b[0x3000];
};
const int kProbeShots = 120; // 12 s: long falls start well before the death
ProbeShot* g_probe = nullptr;
int g_probeNext = 0, g_probeCount = 0, g_probeFiles = 0;
DWORD g_probeLast = 0, g_probeChecked = 0, g_probeWritten = 0;
bool g_probeOn = false;
bool g_probeArmed = false; // snapshots taken alive since the last write
uint8_t g_probeState = 0;

void ProbeCopy(uint8_t* dst, const void* src, size_t n)
{
	SIZE_T got = 0;
	if (!src || !ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) || got != n) {
		memset(dst, 0, n);
	}
}

void DeathProbe(uint8_t* ctl)
{
	DWORD now = GetTickCount();
	if (now - g_probeChecked > 2000) {
		g_probeChecked = now;
		bool on = GetFileAttributesW((Cfg().modCacheDir + L"deathprobe.on").c_str()) != INVALID_FILE_ATTRIBUTES;
		if (on != g_probeOn) {
			g_probeOn = on;
			XLS_LOG_INFO("mods: rules: death probe %s.", on ? "on" : "off");
		}
	}
	if (!g_probeOn) {
		return;
	}
	if (!g_probe) {
		g_probe = (ProbeShot*)VirtualAlloc(nullptr, sizeof(ProbeShot) * kProbeShots, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!g_probe) {
			return;
		}
	}
	uint8_t state = ctl[kCtlState];
	if (now - g_probeLast >= 100 || state != g_probeState) {
		g_probeLast = now;
		ProbeShot& shot = g_probe[g_probeNext];
		uint8_t* player = *(uint8_t**)(ctl + kCtlPlayer);
		uint8_t* body = nullptr;
		ProbeCopy((uint8_t*)&body, player ? player + kPlayerBody : nullptr, 4);
		shot.tick = now;
		shot.state = state;
		shot.ctl = (uint32_t)(uintptr_t)ctl;
		shot.player = (uint32_t)(uintptr_t)player;
		shot.body = (uint32_t)(uintptr_t)body;
		ProbeCopy(shot.c, ctl, sizeof(shot.c));
		ProbeCopy(shot.p, player, sizeof(shot.p));
		ProbeCopy(shot.b, body, sizeof(shot.b));
		g_probeNext = (g_probeNext + 1) % kProbeShots;
		g_probeCount = g_probeCount < kProbeShots ? g_probeCount + 1 : kProbeShots;
		static DWORD said = 0;
		if (g_probeCount == kProbeShots && now - said > 60000) {
			said = now;
			XLS_LOG_INFO("mods: rules: death probe: recording (state %u, player %p, body %08x).", state, player, shot.body);
		}
	}
	static bool f8 = false;
	bool f8now = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
	if (f8now && !f8 && g_probeCount) {
		std::wstring dir = Cfg().modCacheDir + L"deathprobe\\";
		CreateDirectoryW(dir.c_str(), nullptr);
		static int snaps = 0;
		std::wstring path;
		do {
			path = dir + FormatW(L"snap-%d.bin", ++snaps);
		} while (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
		Bytes out;
		uint32_t head[2] = {0x5044504c, 1};
		out.insert(out.end(), (uint8_t*)head, (uint8_t*)(head + 2));
		const ProbeShot& shot = g_probe[(g_probeNext - 1 + kProbeShots) % kProbeShots];
		out.insert(out.end(), (const uint8_t*)&shot, (const uint8_t*)(&shot + 1));
		WriteWholeFile(path, out);
		XLS_LOG_INFO("mods: rules: death probe: snapshot -> %ls.", path.c_str());
		// which of the player's (and its +0x40's) fields point at weapons (id 0x04KK0000 at +0x1c): the
		// weapon in hand, carried ones, worn gear (the flyer pickup)
		const uint8_t* objs[2] = {(const uint8_t*)(uintptr_t)shot.player, (const uint8_t*)(uintptr_t)shot.body};
		const uint8_t* copies[2] = {shot.p, shot.b};
		const size_t sizes[2] = {sizeof(shot.p), sizeof(shot.b)};
		for (int o = 0; o < 2; o++) {
			for (size_t at = 0; objs[o] && at + 4 <= sizes[o]; at += 4) {
				uint32_t target = *(const uint32_t*)(copies[o] + at);
				uint32_t id = 0;
				if (target < 0x10000 || (target & 3)) {
					continue;
				}
				ProbeCopy((uint8_t*)&id, (const void*)(uintptr_t)(target + 0x1c), 4);
				if ((id & 0xff00ffff) == 0x04000000) {
					XLS_LOG_INFO("mods: rules: death probe:   %s+0x%x -> %08x (weapon kind %02x).", o ? "body" : "player", (unsigned)at, target, (id >> 16) & 0xff);
				}
			}
		}
	}
	f8 = f8now;
	g_probeState = state;
	g_probeArmed = true;
}

// The death: the controller update runs (only while dead) for the local player with the ring armed.
void DeathProbeWrite(uint8_t* ctl)
{
	if (!g_probeOn || !g_probe || !g_probeArmed || !g_probeCount) {
		return;
	}
	g_probeArmed = false;
	uint8_t state = ctl[kCtlState];
	if (g_probeWritten && GetTickCount() - g_probeWritten < 5000) { // still the same death
		g_probeCount = 0;
		return;
	}
	g_probeWritten = GetTickCount();
	{
		std::wstring dir = Cfg().modCacheDir + L"deathprobe\\";
		CreateDirectoryW(dir.c_str(), nullptr);
		std::wstring path;
		do {
			path = dir + FormatW(L"death-%d.bin", ++g_probeFiles);
		} while (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
		Bytes out;
		uint32_t head[2] = {0x5044504c, (uint32_t)g_probeCount};
		out.insert(out.end(), (uint8_t*)head, (uint8_t*)(head + 2));
		for (int i = 0; i < g_probeCount; i++) {
			const ProbeShot& shot = g_probe[(g_probeNext - g_probeCount + i + kProbeShots) % kProbeShots];
			out.insert(out.end(), (const uint8_t*)&shot, (const uint8_t*)(&shot + 1));
		}
		WriteWholeFile(path, out);
		XLS_LOG_INFO("mods: rules: death probe: death %d (state %u), %d snapshots -> %ls.", g_probeFiles, state, g_probeCount, path.c_str());
		g_probeCount = 0;
	}
}

// What the lives core is told about a slot (lives.cpp): its state, its team while alive, respawns left. The death only
// counts once the death view's countdown ends, so one in the death view is taken off here.
void ReportLife(int slot)
{
	uint8_t* setup = g_matchSetup ? *g_matchSetup : nullptr;
	uint8_t* body = Body(slot);
	uint8_t st = g_slotState[slot];
	bool out = g_survival && g_lives > 0 && SlotOut(slot);
	LifeState state = RulesEliminated(slot) ? LifeState::Out : RulesHeld(slot) ? LifeState::Held : out && (st != 0 || !body) ? LifeState::Out : st != 0 || !body ? LifeState::Dead : LifeState::Alive;
	int team = state == LifeState::Alive ? UnitTeam(slot) : -1;
	if (team < 0 && LivesTeam(slot) < 0) {
		team = SlotTeam(slot);
	}
	int left = -1;
	if (ModeActive()) {
		int lives = ModeLivesLeft(slot);   // the mode's respawn rules: a death is counted as it happens
		left = lives < 0 ? -2 : state == LifeState::Alive ? std::max(lives - 1, 0) : lives;
	}
	if (g_survival && g_lives > 0) {
		int pending = st != 0 ? 1 : 0;
		left = std::max(g_lives - 1 - (g_team ? TeamDeaths(SlotTeam(slot)) : SlotDeaths(slot)) - pending, 0);
		if (out) {
			left = 0;
		}
	}
	LivesReport(slot, state, team, left, setup ? (const char*)setup + (slot + 3) * 0x7c : nullptr);
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
	bool local = ctl && self >= 0 && self < 16 && ctl[kCtlSlot] == self;
	// and the host runs a player match's bots
	uint8_t* slotRecords = g_ruleSlots ? *g_ruleSlots : nullptr;
	bool hostBot = ctl && !local && !g_offline && g_game && *g_game && (*g_game)[0x256] && slotRecords && ctl[kCtlSlot] < 16 && slotRecords[0x1b1 + ctl[kCtlSlot] * 0x7c] && Host();
	if (ctl && ctl[kCtlSlot] < 16 && (g_roundDead >> ctl[kCtlSlot]) & 1) {
		int slot = ctl[kCtlSlot];
		static DWORD waitLogged = 0;
		if (GetTickCount() - g_roundStartAt > 60000) { // here: since the round ended
			g_roundDead &= ~(1u << slot);
			XLS_LOG_INFO("mods: rules: rounds: player %d never came back in play (state %u).", slot, (unsigned)ctl[kCtlState]);
		}
		else if (!*(uint8_t**)(ctl + 0x40) || SlotOut(slot)) { // waiting for the round's spawn (a body, kCtlBody) and the deaths' reset
			if (GetTickCount() - waitLogged > 2000) {
				waitLogged = GetTickCount();
				XLS_LOG_INFO("mods: rules: rounds: player %d waits for the new round (state %u, body %p, out %d).", slot, (unsigned)ctl[kCtlState], *(uint8_t**)(ctl + 0x40), (int)SlotOut(slot));
			}
		}
		else {
			g_roundDead &= ~(1u << slot);
			if (ctl[kCtlState] != 0) {
				XLS_LOG_INFO("mods: rules: rounds: player %d back in play for the new round (state %u -> 0).", slot, (unsigned)ctl[kCtlState]);
				ctl[kCtlState] = 0;
				((float*)(ctl + kCtlDeathTimers))[0] = 0.0f;
				((float*)(ctl + kCtlDeathTimers))[1] = 0.0f;
			}
		}
	}
	bool hold = ctl && (local || g_offline || hostBot) && (SlotOut(ctl[kCtlSlot]) || RulesHeld(ctl[kCtlSlot]));
	uint8_t was = ctl ? ctl[kCtlState] : 0;
	if (hold && was == kStateDeath) {
		float* timers = (float*)(ctl + kCtlDeathTimers);
		timers[0] = timers[0] < 1.0f ? 1.0f : timers[0];
		timers[1] = timers[1] < 1.0f ? 1.0f : timers[1];
	}
	g_ctlUpdateOriginal(ctl);
	if (ctl && ctl[kCtlSlot] < 16) {
		g_slotState[ctl[kCtlSlot]] = ctl[kCtlState];
		ReportLife(ctl[kCtlSlot]);
	}
	// once its body is removed the out player gets no update of its own (see HudUpdate)
	if (local) {
		g_localSeen = GetTickCount();
		DeathProbeWrite(ctl);
	}
	if (!hold) {
		if (local) {
			// Back in play after spectating (a new round or half): the camera still follows whoever was watched,
			// so point it at our own body again, for up to 5 s while the round's spawn sets it up.
			static DWORD restoreUntil = 0;
			if (g_watched >= 0) {
				restoreUntil = GetTickCount() + 5000;
			}
			// read safely: at a death the player field has held junk (8: a crash reading +0x40, 2026-11-08)
			uint8_t *cam = nullptr, *player = nullptr, *own = nullptr, *watching = nullptr;
			ReadGame(ctl + kCtlCamera, &cam);
			ReadGame(ctl + kCtlPlayer, &player);
			if ((uintptr_t)player < 0x10000 || !ReadGame(player + kPlayerBody, &own) || (uintptr_t)own < 0x10000) {
				own = nullptr;
			}
			if ((uintptr_t)cam < 0x10000 || !ReadGame(cam + kCameraBody, &watching)) {
				cam = nullptr;
			}
			if (restoreUntil && GetTickCount() < restoreUntil && cam && own && g_follow && ctl[kCtlState] == 0 && watching != own) {
				g_follow(cam, own, 0xff, *g_followArg, 0.0f);
				if (g_cameraSnap) {
					g_cameraSnap(cam, nullptr);
				}
				XLS_LOG_INFO("mods: rules: survival: camera back on player %d after spectating.", (int)ctl[kCtlSlot]);
				restoreUntil = 0;
			}
			else if (restoreUntil && GetTickCount() >= restoreUntil) {
				restoreUntil = 0;
			}
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
const uint32_t kPlayersSlots = 0x28, kPlayerInPlay = 8; // bit 0
const uint32_t kNetHostSlot = 0xa1f8, kNetLocalSlot = 0xa1fc;
// 1: the training bot match, ranked by slot like 2; Team Elimination is 4, 5 the team kills rule (mc_Kill)
const int kRuleTraining = 1, kRuleElimination = 2, kRuleTeamElimination = 4, kRuleTeamKills = 5;
const uint32_t kRankList = 0x74, kRankEntry = 0xc, kRankCount = 0x134; // {rank, slot or team, score}

// A team game type: the lobby's rule byte (slots +0x74: 0 Elimination, the rest are team types), a training bot match's
// screen (0 Elimination, 1 Team Elimination)
bool TeamGame()
{
	if (g_offline && g_botArmed) {
		return g_botRule == 1;
	}
	int8_t rule = -1;
	uint8_t* slots = nullptr;
	if (g_ruleSlots && ReadGame(g_ruleSlots, &slots) && slots && ReadGame(slots + 0x74, &rule) && rule >= 0) {
		return rule >= 1;
	}
	return g_team;
}

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
	if (team) { // its respawns left, then its players still up
		int up = 0;
		for (int slot = 0; slot < 16; slot++) {
			up += InPlay(slot) && SlotTeam(slot) == id && !SlotOut(slot);
		}
		return std::max(g_lives - 1 - TeamDeaths(id), 0) + up;
	}
	int32_t deaths = ((int32_t*)(owner + kOwnerInfoGame + kInfoDiePerson))[id];
	return g_lives - deaths > 0 ? g_lives - deaths : 0;
}

bool __attribute__((fastcall)) EndCheck(void* flow)
{
	if (g_endCheckOriginal(flow)) {
		return true;
	}
	uint8_t* setup = *g_matchSetup;
	if (setup && Host() && ModeEndPending(nullptr, nullptr, nullptr)) {
		static DWORD logged = 0;
		if (GetTickCount() - logged > 5000) {
			logged = GetTickCount();
			XLS_LOG_INFO("mods: rules: the game mode ends the round.");
		}
		return true;
	}
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

// Rounds (the stock halftime, made N rounds): the flow's state 5 (match end) runs FUN_01007110, then breaks for the
// second half while slots+0x84 (halftime) is set and +0x1f44 (second half) isn't, else ends the match; the break runs
// FUN_010068e0 (+0x1f44 = 1) and reloads the map with scores kept. +0x1f44 also picks the swapped start points. So:
// RoundEnd counts the round's winner and leaves +0x1f44 0 while rounds are left (another break), NextRound sets it to
// the next round's sides, RoundTime gives each round the full time (the game halves it with halftime on) and marks a
// new match. The result ranks by rounds won.
const uint32_t kSlotsHalftime = 0x84, kSlotsSecondHalf = 0x1f44;
int g_round = 0, g_roundWins[16] = {}, g_roundBase[16] = {}, g_roundScore[16] = {}, g_rawScore[16] = {};
bool g_roundSeen[16] = {}, g_roundBreak = false;
DWORD g_roundBegan = 0; // when the latest round past the first reloaded
DWORD g_roundBreakAt = 0, g_rankAt = 0;
uint8_t* g_rankFlow = nullptr;
int g_rankArg = 0;

using VoidFn = void (*)();
using SlotsFn = void(__attribute__((fastcall)) *)(uint8_t*);
VoidFn g_roundEndOriginal = nullptr, g_roundTimeOriginal = nullptr;
SlotsFn g_nextRoundOriginal = nullptr;

int RoundsTotal(int index)
{
	return index < 10 ? index + 1 : 3 + (index - 10) * 2;
}

bool RoundsOn()
{
	return g_nextRoundOriginal && g_rounds > 0 && *g_matchSetup && g_game && *g_game && (*g_game)[0x256];
}

// With a game mode the placement is the mode's, apart from the game's points (kills etc.: the stock results and the
// career points read each player's own score, only the order / ties come from this list; FUN_0100d360 adds a placement
// bonus in the elimination types). Order: rounds won (no rounds: the round's End winner), the mode's score (Score
// node), still standing / out last, then the game's points. The list's score becomes the tier (equal = tied).
void ModeRanking(int32_t (*list)[3], int count, bool rounds)
{
	struct Key {
		int64_t k[4];
		bool operator<(const Key& o) const { return std::lexicographical_compare(k, k + 4, o.k, o.k + 4); }
		bool operator==(const Key& o) const { return std::equal(k, k + 4, o.k); }
	};
	bool team = TeamGame();   // the ranking's ids: teams in team types, players otherwise
	int endSide = -1;
	bool endTeam = false;
	bool ended = ModeEndPending(&endSide, &endTeam, nullptr) && endSide >= 0;
	Key match[16], round[16];
	for (int i = 0; i < count; i++) {
		int id = list[i][1];
		bool valid = id >= 0 && id < 16;
		bool won = ended && (team == endTeam ? id == endSide : endTeam ? (valid && SlotTeam(id) == endSide) : SlotTeam(endSide) == id);
		int scoreMatch = 0, scoreRound = 0;
		ModeScores(id, team, &scoreMatch, &scoreRound);
		int64_t standing = 0;   // FFA: in = 1000, out = the order they went out; teams: players still in
		if (team) {
			for (int k = 0; k < 16; k++) {
				standing += RulesInPlay(k) && SlotTeam(k) == id && !RulesEliminated(k) ? 1 : 0;
			}
		}
		else if (valid) {
			standing = RulesEliminated(id) ? g_elimOrder[id].load() : 1000;
		}
		int64_t points = list[i][2];
		int64_t roundPoints = rounds && valid ? g_roundScore[id] : points;
		int64_t wins = valid ? g_roundWins[id] : 0;
		match[i] = {{rounds ? wins : (int64_t)won, scoreMatch, standing, points}};
		round[i] = {{(int64_t)won, scoreRound, standing, roundPoints}};
	}
	auto tiers = [&](Key* keys, int* out) { // 1 the lowest, equal keys equal
		for (int i = 0; i < count; i++) {
			std::vector<Key> lower;
			for (int j = 0; j < count; j++) {
				if (keys[j] < keys[i] && std::find(lower.begin(), lower.end(), keys[j]) == lower.end()) {
					lower.push_back(keys[j]);
				}
			}
			out[i] = 1 + (int)lower.size();
		}
	};
	int matchTier[16], roundTier[16];
	tiers(match, matchTier);
	tiers(round, roundTier);
	static DWORD logged = 0;
	if (count && GetTickCount() - logged > 10000) {
		logged = GetTickCount();
		int top = 0;
		for (int i = 1; i < count; i++) {
			top = matchTier[i] > matchTier[top] ? i : top;
		}
		const int64_t* k = match[top].k;
		XLS_LOG_INFO("mods: rules: mode ranking: first %s %d (%s %lld, score %lld, standing %lld, points %lld).", team ? "team" : "player",
		             list[top][1], rounds ? "rounds won" : "won", (long long)k[0], (long long)k[1], (long long)k[2], (long long)k[3]);
	}
	for (int i = 0; i < count; i++) {
		int id = list[i][1];
		list[i][2] = matchTier[i];
		if (rounds && id >= 0 && id < 16) {
			g_roundScore[id] = roundTier[i];   // RoundEnd: the round's winner
		}
	}
	for (int i = 1; i < count; i++) {
		for (int j = i; j > 0 && list[j][2] > list[j - 1][2]; j--) {
			std::swap(list[j], list[j - 1]);
		}
	}
}

void __attribute__((thiscall)) Ranking(uint8_t* flow, int arg)
{
	g_rankingOriginal(flow, arg);
	g_rankFlow = flow;
	g_rankArg = arg;
	g_rankAt = GetTickCount();
	uint8_t* setup = *g_matchSetup;
	int32_t rule = setup ? *(int32_t*)(setup + kSetupRule) : 0;
	static DWORD logged = 0;
	bool log = GetTickCount() - logged > 10000;
	if (log && g_survival) {
		logged = GetTickCount();
		XLS_LOG_INFO("mods: rules: survival: ranking (rule %d, %d entries).", rule, *(int32_t*)(flow + kRankCount));
	}
	int32_t (*list)[3] = (int32_t(*)[3])(flow + kRankList);
	int count = *(int32_t*)(flow + kRankCount);
	count = count < 0 ? 0 : count > 16 ? 16 : count;
	bool rounds = RoundsOn();
	if (rounds) { // this round's score: the game keeps scores over the break
		for (int i = 0; i < count; i++) {
			int id = list[i][1];
			if (id >= 0 && id < 16) {
				g_rawScore[id] = list[i][2];
				g_roundSeen[id] = true;
				g_roundScore[id] = list[i][2] >= g_roundBase[id] ? list[i][2] - g_roundBase[id] : list[i][2];
			}
		}
	}
	bool survival = g_survival && g_lives > 0 && (rule == kRuleTraining || rule == kRuleElimination || rule == kRuleTeamElimination || rule == kRuleTeamKills);
	if (survival) {
		bool team = rule == kRuleTeamElimination || rule == kRuleTeamKills;
		// respawns left first, the score breaks ties (encoded in the score the ranks are told apart by)
		for (int i = 0; i < count; i++) {
			int left = LivesLeft(list[i][1], team);
			list[i][2] = left * 100000 + (list[i][2] < 100000 ? list[i][2] : 99999);
			int id = list[i][1];
			if (rounds && id >= 0 && id < 16) {
				g_roundScore[id] = left * 100000 + std::min(g_roundScore[id], 99999);
			}
		}
	}
	if (ModeActive()) {
		ModeRanking(list, count, rounds);
		return;
	}
	int modeSide = -1;
	bool modeTeam = false;
	if (ModeEndPending(&modeSide, &modeTeam, nullptr) && modeSide >= 0) { // the mode's winner tops the round
		bool teamIds = TeamGame();   // the ranking's ids: teams in team types, players otherwise
		for (int i = 0; i < count; i++) {
			int id = list[i][1];
			bool win = teamIds == modeTeam ? id == modeSide : modeTeam ? (id >= 0 && id < 16 && SlotTeam(id) == modeSide) : SlotTeam(modeSide) == id;
			if (win) {
				list[i][2] = std::min(list[i][2], 999999) + 1000000;
				if (rounds && id >= 0 && id < 16) {
					g_roundScore[id] = std::min(g_roundScore[id], 999999) + 1000000;
				}
			}
		}
	}
	if (rounds) { // rounds won first
		for (int i = 0; i < count; i++) {
			int id = list[i][1];
			int wins = id >= 0 && id < 16 ? g_roundWins[id] : 0;
			list[i][2] = wins * 10000000 + std::min(std::max(list[i][2], 0), 9999999);
		}
	}
	if (survival || rounds) {
		for (int i = 1; i < count; i++) {
			for (int j = i; j > 0 && list[j][2] > list[j - 1][2]; j--) {
				std::swap(list[j], list[j - 1]);
			}
		}
	}
	if (log && survival && count) {
		XLS_LOG_INFO("mods: rules: survival: ranked first %d (score %d).", list[0][1], list[0][2]);
	}
}

void RoundEnd()
{
	uint8_t* slots = *g_matchSetup;
	ModeEvent(kEvRoundEnd, g_round + 1);
	if (!RoundsOn() || !slots) {
		g_roundEndOriginal();
		return;
	}
	if (g_rankFlow && GetTickCount() - g_rankAt > 500) {
		Ranking(g_rankFlow, g_rankArg); // a client doesn't always rank here
	}
	int total = RoundsTotal(g_rounds), best = -1, bestScore = 0, ties = 0;
	for (int id = 0; id < 16; id++) {
		if (!g_roundSeen[id]) {
			continue;
		}
		if (best < 0 || g_roundScore[id] > bestScore) {
			best = id;
			bestScore = g_roundScore[id];
			ties = 0;
		}
		else if (g_roundScore[id] == bestScore) {
			ties++;
		}
	}
	int winner = ties ? -1 : best, most = 0;
	if (winner >= 0) {
		g_roundWins[winner]++;
	}
	g_round++;
	for (int id = 0; id < 16; id++) {
		g_roundBase[id] = g_rawScore[id];
		most = std::max(most, g_roundWins[id]);
	}
	bool bestOf = g_rounds >= 10;
	bool modeMatch = false;
	ModeEndPending(nullptr, nullptr, &modeMatch);
	bool over = g_round >= total || (bestOf && most > total / 2) || modeMatch;
	XLS_LOG_INFO("mods: rules: rounds: round %d of %s%d over, %s %d (round score %d)%s; %s.", g_round, bestOf ? "best of " : "", total,
	             winner >= 0 ? "won by" : "drawn, top", best, bestScore, winner >= 0 ? FormatA(", %d wins", g_roundWins[winner]).c_str() : "",
	             over ? "match over" : "next round");
	if (over && g_rankFlow) {
		Ranking(g_rankFlow, g_rankArg); // the result follows rounds won
	}
	{ // the round's result on screen (this runs where the match flow does: the host)
		int self = g_localIndex;
		bool teamGame = TeamGame();
		int own = self >= 0 && self < 16 ? (teamGame ? SlotTeam(self) : self) : -1;
		uint8_t* setupNames = g_matchSetup ? *g_matchSetup : nullptr;
		std::string text;
		uint32_t colour = 0xffffff;
		if (winner < 0) {
			text = FormatA("ROUND %d DRAWN", g_round);
		}
		else if (teamGame) {
			text = FormatA(winner == own ? "YOUR TEAM WINS ROUND %d" : "ENEMY TEAM WINS ROUND %d", g_round);
			colour = winner == own ? 0x4c9cff : 0xff4848;
		}
		else {
			const char* name = setupNames && winner < 16 ? (const char*)setupNames + (winner + 3) * 0x7c : "";
			text = winner == own ? FormatA("YOU WIN ROUND %d", g_round) : FormatA("%.20s WINS ROUND %d", name, g_round);
			colour = winner == own ? 0x4c9cff : 0xff4848;
		}
		LivesMessage(text.c_str(), 0, colour, 4.0f);
	}
	uint32_t dead = 0;
	for (int slot = 0; slot < 16; slot++) {
		dead |= g_slotState[slot] != 0 ? 1u << slot : 0;
	}
	g_roundDead = over ? 0 : dead;
	g_roundStartAt = GetTickCount();
	XLS_LOG_INFO("mods: rules: rounds: down at the round's end: 0x%04x.", (unsigned)dead);
	g_roundEndOriginal();
	slots[kSlotsHalftime] = 1;
	slots[kSlotsSecondHalf] = over ? 1 : 0;
	g_roundBreak = !over;
	g_roundBreakAt = GetTickCount();
}

void __attribute__((fastcall)) NextRound(uint8_t* slots)
{
	g_nextRoundOriginal(slots);
	if (RoundsOn() && slots) {
		int every = g_switchEvery;
		slots[kSlotsSecondHalf] = every > 0 ? (g_round / every) & 1 : 0;
		XLS_LOG_INFO("mods: rules: rounds: round %d next, sides %s.", g_round + 1, slots[kSlotsSecondHalf] ? "swapped" : "as started");
	}
}

void RoundTime()
{
	uint8_t* slots = *g_matchSetup;
	bool versus = g_game && *g_game && (*g_game)[0x256];
	if (!slots || !versus) {
		g_roundTimeOriginal();
		return;
	}
	bool newMatch = !g_roundBreak || GetTickCount() - g_roundBreakAt > 180000;
	if (newMatch) { // a new match
		g_round = 0;
		memset(g_roundWins, 0, sizeof(g_roundWins));
		memset(g_roundBase, 0, sizeof(g_roundBase));
		memset(g_roundScore, 0, sizeof(g_roundScore));
		memset(g_rawScore, 0, sizeof(g_rawScore));
		memset(g_roundSeen, 0, sizeof(g_roundSeen));
		LivesReset();
		if (g_rounds > 0) {
			XLS_LOG_INFO("mods: rules: rounds: new match, %s%d rounds, sides switch %s.", g_rounds >= 10 ? "best of " : "", RoundsTotal(g_rounds),
			             g_switchEvery ? FormatA("every %d", g_switchEvery.load()).c_str() : "never");
		}
	}
	if (!g_roundBreak) {
		g_roundDead = 0;
	}
	LivesReset(); // every round starts with everyone in play again (re-reported every frame)
	g_heldMask = 0; // the round's start spawns everyone: nobody held or eliminated
	g_elimMask = 0;
	g_elimCount = 0;
	for (auto& o : g_elimOrder) {
		o = 0;
	}
	if (newMatch) {
		ModeLoad();
		ModeEvent(kEvMatchStart);
	}
	ModeEvent(kEvRoundStart, g_round + 1);
	if (g_roundBreak && g_rounds > 0) {
		LivesMessage(FormatA("ROUND %d", g_round + 1).c_str(), 0, 0xffffff, 2.5f);
	}
	g_roundBegan = g_roundBreak ? GetTickCount() : 0;
	g_roundBreak = false;
	slots[kSlotsHalftime] = 0; // the full time for every round
	g_roundTimeOriginal();
	slots[kSlotsHalftime] = g_rounds > 0 ? 1 : 0; // our Rounds row stands in for the stock Halftime one
}

// The round counter (ui.cpp layer): top centre during a match with Rounds on. "ROUND n" and the rounds format, then the
// rounds won, own side first in blue, the leading other side in red. Team modes go by team (the round's ranking ids are
// teams there), free for all by player.
void RoundCounter(float width, float height)
{
	uint8_t* slots = g_matchSetup ? *g_matchSetup : nullptr;
	if (!slots || !RoundsOn() || g_rounds <= 0 || GetTickCount() - g_hudSeenAt > 500) {
		return;
	}
	int total = RoundsTotal(g_rounds), self = g_localIndex;
	bool bestOf = g_rounds >= 10;
	int round = std::min(g_round + (g_roundBreak ? 0 : 1), total);
	int own = self >= 0 && self < 16 ? (TeamGame() ? SlotTeam(self) : self) : -1;
	int ownWins = own >= 0 && own < 16 ? g_roundWins[own] : 0, otherWins = 0;
	for (int id = 0; id < 16; id++) {
		if (id != own) {
			otherWins = std::max(otherWins, g_roundWins[id]);
		}
	}
	float scale = height / 720.0f;
	char title[48], format[48], score[32];
	snprintf(title, sizeof title, g_roundBreak ? "ROUND %d OVER" : "ROUND %d", round);
	snprintf(format, sizeof format, bestOf ? "BEST OF %d" : "OF %d", total);
	ImFont* big = UiFont(true);
	ImFont* small = UiFont(false);
	if (!big || !small) {
		return;
	}
	ImVec2 t = big->CalcTextSizeA(big->FontSize, FLT_MAX, 0, title);
	ImVec2 f = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0, format);
	snprintf(score, sizeof score, "%d", ownWins);
	ImVec2 s1 = big->CalcTextSizeA(big->FontSize, FLT_MAX, 0, score);
	char score2[16];
	snprintf(score2, sizeof score2, "%d", otherWins);
	ImVec2 s2 = big->CalcTextSizeA(big->FontSize, FLT_MAX, 0, score2);
	float pad = 10 * scale, gap = 18 * scale;
	float midW = std::max(t.x, f.x);
	float w = s1.x + gap + midW + gap + s2.x + pad * 2, h = t.y + f.y + pad * 1.5f;
	float x = (width - w) / 2, y = 10 * scale;
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(8, 20, 32, 170), 6 * scale);
	dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(120, 200, 230, 120), 6 * scale, 0, 1.5f * scale);
	float cx = x + pad + s1.x + gap;
	dl->AddText(big, big->FontSize, ImVec2(cx + (midW - t.x) / 2, y + pad * 0.5f), IM_COL32(235, 248, 255, 255), title);
	dl->AddText(small, small->FontSize, ImVec2(cx + (midW - f.x) / 2, y + pad * 0.5f + t.y), IM_COL32(150, 210, 230, 255), format);
	float sy = y + (h - s1.y) / 2;
	dl->AddText(big, big->FontSize, ImVec2(x + pad, sy), IM_COL32(76, 156, 255, 255), score);
	dl->AddText(big, big->FontSize, ImVec2(x + w - pad - s2.x, sy), IM_COL32(255, 72, 72, 255), score2);
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

// Team Survival: the team kill counters (flash\cockpit\id762 mc_Kill, top left, "kills/target") show each
// team's players standing plus respawns left, over its players plus starting respawns. The cockpit's setters (thiscall(cockpit),
// setKillRed / setKillBlue(3 digits, 2 target digits, infinity flag) on the movie at cockpit +0x58) are
// detoured, and so is its per-frame update (which calls them only when the kills change), to show a new
// count as soon as a team loses a respawn.
struct CounterVas {
	uint32_t red, blue, update;
};
const CounterVas kCounterVas[kBuildCount] = {
	{0xecf520, 0xecf660, 0xedbb80}, // DX11
	{0xebf710, 0xebf850, 0xecbd70}, // DX9
	{0xecf8f0, 0xecfa30, 0xedbf50}, // DX11 1.1
	{0xebf7b0, 0xebf8f0, 0xecbe10}, // DX9 1.1
};
const uint32_t kCockpitMovie = 0x58;
ListDrawFn g_killRedOriginal = nullptr, g_killBlueOriginal = nullptr, g_cockpitUpdateOriginal = nullptr;
uint8_t* g_counterCockpit = nullptr;
int g_counterShown[2] = {-1, -1};

// Only the team kills rule shows mc_Kill. On Team Elimination (rule 4, as training bot matches are) that clip is off
// screen and the team bar is hidden too, so the respawns go in the list's text rows (TeamRows) instead.
bool CountersOurs()
{
	uint8_t* setup = g_matchSetup ? *g_matchSetup : nullptr;
	return g_survival && g_team && g_lives > 0 && setup && *(int32_t*)(setup + kSetupRule) == kRuleTeamKills &&
	       g_killRedOriginal && g_killBlueOriginal && g_cockpitUpdateOriginal && g_flashCall;
}

// Players standing plus respawns left, over the team's players plus the respawns it started with (1 life: 4/4 down to 0/4).
void CounterValues(int team, int* left, int* total)
{
	int players = 0;
	for (int slot = 0; slot < 16; slot++) {
		players += InPlay(slot) && SlotTeam(slot) == team;
	}
	*total = std::min(g_lives - 1 + players, 99);
	*left = std::min(std::max(LivesLeft(team, true), 0), *total);
}

void SetCounter(uint8_t* cockpit, int team)
{
	uint8_t* movie = *(uint8_t**)(cockpit + kCockpitMovie);
	if (!movie) {
		return;
	}
	int left, total;
	CounterValues(team, &left, &total);
	static std::atomic<int> counterLogged{-1};
	if (counterLogged.exchange(left * 2 + team) != left * 2 + team) {
		XLS_LOG_INFO("mods: rules: survival display: team %d counter = %d respawns (cockpit %p, movie %p).", team, left, cockpit, movie);
	}
	struct { uint8_t flag; uint32_t type; float number; } args[6] = {
		{0, 3, float(left / 100)}, {0, 3, float(left / 10 % 10)}, {0, 3, float(left % 10)},
		{0, 3, float(total / 10)}, {0, 3, float(total % 10)}, {0, 2, 0.0f}}; // 2: bool (false: no infinity)
	g_flashCall(movie, 0, team ? "_level0.mc_Kill.setKillBlue" : "_level0.mc_Kill.setKillRed", (FlashArg*)args, 6);
	g_counterShown[team] = left * 100 + total;
	g_counterCockpit = cockpit;
}

void __attribute__((thiscall)) KillRed(uint8_t* cockpit)
{
	CountersOurs() ? SetCounter(cockpit, 0) : g_killRedOriginal(cockpit);
}

void __attribute__((thiscall)) KillBlue(uint8_t* cockpit)
{
	CountersOurs() ? SetCounter(cockpit, 1) : g_killBlueOriginal(cockpit);
}

void __attribute__((thiscall)) CockpitUpdate(uint8_t* cockpit)
{
	g_cockpitUpdateOriginal(cockpit);
	if (!CountersOurs() || !*(uint8_t**)(cockpit + kCockpitMovie)) {
		g_counterShown[0] = g_counterShown[1] = -1;
		return;
	}
	if (cockpit != g_counterCockpit) {
		g_counterShown[0] = g_counterShown[1] = -1;
	}
	for (int team = 0; team < 2; team++) {
		int left, total;
	CounterValues(team, &left, &total);
		if (g_counterShown[team] != left * 100 + total) {
			SetCounter(cockpit, team);
		}
	}
}
uint8_t** g_input = nullptr;
const uint32_t kMenuState = 0x190, kMenuPhase = 0x194, kMenuRule = 0x198, kMenuMode = 0x1a0, kMenuRow = 0x1b0, kMenuFlags = 0x1d4;
const uint32_t kMenuMouseOn = 0x168, kMenuMouse = 0x154, kMenuWheel = 0x158, kMenuClick = 0x15c, kInputPressed = 0x330;
const int kPhaseBot = 2, kModeBusy = 0x3e5, kMouseBotRow = 0x22e, kMouseArrowL = 10, kMouseArrowR = 11;
const uint32_t kKeyUp = 0x40, kKeyDown = 0x80, kKeyLeft = 0x100, kKeyRight = 0x200, kFlagRow = 0x80;

bool BotScreen(uint8_t* menu)
{
	return *(int32_t*)(menu + kMenuState) == 1 && *(int32_t*)(menu + kMenuPhase) == kPhaseBot && *(int32_t*)(menu + kMenuMode) != kModeBusy;
}

// The rows by their item number (the game's Map 0 and Game Type 1, our Survival 2, Respawns 3,
// Bots 4, Difficulty 5, Brain 6, Personality 7, Taunts 8) in screen order, the shown ones only: Survival
// for the elimination rules, Respawns with Survival on.
const int kRowBots = 4, kRowDifficulty = 5, kRowBrain = 6, kRowPersona = 7, kRowTaunts = 8, kRowLast = 8;
const int kRowsMax = kRowLast + 1;
// Up to 9 rows: the screen shows a window of kRowsVisible of them that scrolls with the cursor (g_botScroll = the
// first shown row's index in screen order).
const int kRowsVisible = 5; // with 6 the last row could not be brought fully into view (user test)
int g_botScroll = 0;
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
	for (int r = kRowDifficulty; r <= kRowLast; r++) {
		rows[n++] = r;
	}
	return n;
}

void BotRowsDraw(uint8_t* menu)
{
	g_botRule = *(int32_t*)(menu + kMenuRule);
	int rows[kRowsMax], shown = BotRowsShown(menu, rows);
	std::string bots = std::to_string(g_botCount.load());
	struct { int n; const char* label; const char* value; } ours[] = {
		{2, "SURVIVAL", g_botSurvival ? "On" : "Off"}, {3, "RESPAWNS", kLivesNames[g_botLives % kLivesValues]},
		{kRowBots, "BOTS", bots.c_str()}, {kRowDifficulty, "DIFFICULTY", kDifficultyNames[g_botDifficulty & 3]},
		{kRowBrain, "BRAIN", kBrainNames[g_botBrain & 1]}, {kRowPersona, "PERSONALITY", kPersonaNames[g_botPersona % 6]},
		{kRowTaunts, "TAUNTS", kTauntNames[g_botTaunts & 3]}};
	bool ok = g_flashCall != nullptr;
	for (auto& r : ours) {
		std::string n = std::to_string(r.n);
		FlashArg args[3] = {{0, 4, n.c_str()}, {0, 4, r.label}, {0, 4, r.value}};
		ok = ok && g_flashCall(menu, 0, "lp2BotRow", args, 3);
	}
	g_botScroll = std::max(0, std::min(g_botScroll, shown - kRowsVisible));
	std::string order;
	for (int i = g_botScroll; i < shown && i < g_botScroll + kRowsVisible; i++) {
		order += (i > g_botScroll ? "," : "") + std::to_string(rows[i]);
	}
	FlashArg arg = {0, 4, order.c_str()};
	if (!ok || !g_flashCall(menu, 0, "lp2BotOrder", &arg, 1)) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			XLS_LOG_WARN("mods: rules: training: the menu Flash has no lp2BotRow / lp2BotOrder (rows not added).");
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
		g_botScroll = 0;
		*(uint32_t*)(menu + kMenuFlags) |= kFlagRow;
	}
	if (drawn != menu || drawnRule != *(int32_t*)(menu + kMenuRule)) { // shown, or the rule changed
		drawn = menu;
		drawnRule = *(int32_t*)(menu + kMenuRule);
		BotRowsDraw(menu);
	}
	uint32_t pressed = *(uint32_t*)(*g_input + kInputPressed);
	int rows[kRowsMax], shown = BotRowsShown(menu, rows), at = shown - 1; // a hidden row: the last shown
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
	int mouse = *(int32_t*)(menu + kMenuMouse) - kMouseBotRow;
	// The wheel scrolls the window (uPlanetFlash keeps the last wheel event; used up here). The cursor stays on its row
	// while that's visible. The row id under a still mouse is out of date after a scroll, so it's ignored until it changes.
	static int staleMouse = -1000;
	int32_t& wheel = *(int32_t*)(menu + kMenuWheel);
	if (wheel != 0 && shown > kRowsVisible) {
		g_botScroll = std::max(0, std::min(g_botScroll + (wheel > 0 ? 1 : -1), shown - kRowsVisible)); // the game's sign: positive = down
		at = std::max(g_botScroll, std::min(at, g_botScroll + kRowsVisible - 1));
		staleMouse = mouse;
		BotRowsDraw(menu);
	}
	wheel = 0;
	if (mouse != staleMouse) {
		staleMouse = -1000;
	}
	row = rows[at];
	for (int i = 0; menu[kMenuMouseOn] && staleMouse == -1000 && i < shown; i++) {
		if (rows[i] == mouse) {
			row = mouse;
		}
	}
	if (row != was || (pressed & (kKeyUp | kKeyDown))) {
		*(uint32_t*)(menu + kMenuFlags) |= kFlagRow;
	}
	for (int i = 0; i < shown; i++) { // keep the cursor's row in the window
		if (rows[i] == row && (i < g_botScroll || i >= g_botScroll + kRowsVisible)) {
			g_botScroll = i < g_botScroll ? i : i - kRowsVisible + 1;
			BotRowsDraw(menu);
		}
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
	// a list row: one step, stopping at its ends
	auto step = [dir](std::atomic<int>& v, int count) {
		int at = v + dir;
		if (at < 0 || at >= count) {
			return false;
		}
		v = at;
		return true;
	};
	int row = *(int32_t*)(menu + kMenuRow);
	if (row == kRowBots) {
		int at = g_botCount + dir;
		if (at < 1 || at > 15) {
			return;
		}
		g_botCount = at;
	} else if (row == kRowDifficulty || row == kRowBrain || row == kRowPersona || row == kRowTaunts) {
		bool changed = row == kRowDifficulty ? step(g_botDifficulty, 4) : row == kRowBrain ? step(g_botBrain, 2)
		             : row == kRowPersona ? step(g_botPersona, 6) : step(g_botTaunts, 4);
		if (!changed) {
			return;
		}
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
	XLS_LOG_INFO("mods: rules: training: %d bots, survival %s, %s respawns, %s, %s brain, %s, taunts %s.", g_botCount.load(),
	             g_botSurvival ? "on" : "off", kLivesNames[g_botLives % kLivesValues], kDifficultyNames[g_botDifficulty & 3],
	             kBrainNames[g_botBrain & 1], kPersonaNames[g_botPersona % 6], kTauntNames[g_botTaunts & 3]);
}

// The bottom bar text: AI Enemy Battle asks for MsgCinesco 0x106 + row, so our rows get ours
// (the Bots row's would be DUMMY).
using MenuHelpFn = void(__attribute__((thiscall)) *)(uint8_t*, int);
MenuHelpFn g_menuHelpOriginal = nullptr;
const int kHelpBotRow = 0x106;

extern "C" void __attribute__((thiscall)) MenuHelpBody(uint8_t* menu, int id)
{
	static const char* const kHelp[] = {"Limit respawns: the last player or team standing wins.", "How many times each player can respawn.",
	                                    "How many AI enemies join the battle (1 to 15).",
	                                    "How hard the bots fight: how aggressive, quick to dodge and keen on grenades they are.",
	                                    "Improved: weapon-aware bots with a brain per game type. Stock: the game's own.",
	                                    "Mixed gives each bot its own style; or make every bot one kind.",
	                                    "How often bots taunt after a kill."};
	int row = id - kHelpBotRow;
	if (!BotScreen(menu) || row < 2 || row > kRowLast) {
		g_menuHelpOriginal(menu, id);
		return;
	}
	if (menu[0x1d0]) { // as the original: only while the bar is up
		FlashArg arg = {0, 4, kHelp[row - 2]};
		g_flashCall(menu, 0, "_root.mc_CINESCO.cinescoMsg", &arg, 1);
	}
}

// Its callers keep the menu in ecx across the call, so the replacement keeps ecx and edx.
}

bool BotsInPlayerMatch()
{
	return g_botsInMatch;
}

BotOptions BotMenuOptions()
{
	return {g_botDifficulty & 3, g_botBrain != 0, g_botPersona % 6, g_botTaunts & 3};
}

namespace {

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

DWORD WINAPI SurvivalThread(LPVOID)
{
	std::string last;
	bool wasOut = false;
	char hudFile[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, hudFile, MAX_PATH);
	if (char* slash = strrchr(hudFile, '\\')) {
		strcpy(slash + 1, "team-hud.txt");
	}
	for (int tick = 0;; Sleep(200), tick++) {
		if (tick % 5 == 0) { // Team Survival rows' place, for lining them up while playing
			if (FILE* file = fopen(hudFile, "r")) {
				int x, y, gap;
				if (fscanf(file, "%d %d %d", &x, &y, &gap) == 3 && (x != g_teamRowsX || y != g_teamRowsY || gap != g_teamRowsGap)) {
					g_teamRowsX = x, g_teamRowsY = y, g_teamRowsGap = gap;
					XLS_LOG_INFO("mods: rules: team rows at %d, %d, gap %d (team-hud.txt).", x, y, gap);
				}
				fclose(file);
			}
		}
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
		// Team Survival: each new death in turn takes one of its team's respawns, or puts the player out
		static int32_t seen[16];
		bool restart = !have;
		for (int i = 0; i < 16 && have; i++) {
			restart = restart || diePerson[i] < seen[i];
		}
		if (restart) {
			memset(seen, 0, sizeof(seen));
			g_teamOut = 0;
		}
		if (have && g_survival && g_team && g_lives > 0 && setup) {
			int32_t teams[16], used[16] = {};
			for (int i = 0; i < 16; i++) {
				if (!ReadGame(setup + kSetupSlots + i * kSetupSlot, &teams[i]) || teams[i] < 0 || teams[i] >= 16) {
					teams[i] = -1;
				}
			}
			for (int i = 0; i < 16; i++) {
				used[teams[i] & 15] += teams[i] >= 0 ? seen[i] : 0;
			}
			for (int i = 0; i < 16; i++) {
				for (; teams[i] >= 0 && seen[i] < diePerson[i]; seen[i]++) {
					if (++used[teams[i]] >= g_lives && !((g_teamOut >> i) & 1)) {
						g_teamOut |= 1u << i;
						XLS_LOG_INFO("mods: rules: survival: player %d out (team %d, %d team deaths).", i, teams[i], used[teams[i]]);
					}
				}
			}
		}
		bool out = false;
		if (have && g_survival && g_lives > 0 && index >= 0 && index < 16) {
			out = g_team ? (g_teamOut >> index) & 1 : diePerson[index] >= g_lives;
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

uint8_t* g_respawnSecond = nullptr; // nullptr: not the expected bytes, left alone
const uint8_t kRespawnSecondCode[] = {0x8b, 0xbe, 0xf0, 0x00, 0x00, 0x00}, kRespawnSecondJump[] = {0xeb, 0x0e};

void RespawnSecondJump(bool on)
{
	DWORD old;
	if (g_respawnSecond && VirtualProtect(g_respawnSecond, 2, PAGE_EXECUTE_READWRITE, &old)) {
		memcpy(g_respawnSecond, on ? kRespawnSecondJump : kRespawnSecondCode, 2);
		VirtualProtect(g_respawnSecond, 2, old, &old);
		FlushInstructionCache(GetCurrentProcess(), g_respawnSecond, 2);
	}
}

// Rounds: each player's spawn record (game +0x28 + slot * 4; the spawn reads the weapon kinds at +0x10, eight of them, and
// with +0xb clear it loads the ammo kept at +0x64 and +0x74, two shorts each) carries a survivor's weapons and ammo into
// the next round, as the game's halftime does. Taken at the match's first spawn, put back at a later round's.
const uint32_t kRecordSize = 0xc8;
uint8_t g_loadout[16][kRecordSize];
bool g_loadoutHave[16] = {};
extern bool g_roundBreak;
extern DWORD g_roundBegan;

void RoundLoadout(int slot)
{
	uint8_t* game = g_game ? *g_game : nullptr;
	uint8_t* rec = game && slot >= 0 && slot < 16 ? *(uint8_t**)(game + 0x28 + slot * 4) : nullptr;
	if (!rec || !RoundsOn()) {
		return;
	}
	if (!g_roundBegan || GetTickCount() - g_roundBegan > 30000) { // the match's start (or no rounds behind it)
		memcpy(g_loadout[slot], rec, kRecordSize);
		g_loadoutHave[slot] = true;
		return;
	}
	if (!g_loadoutHave[slot]) {
		return;
	}
	const uint8_t* was = g_loadout[slot];
	std::string other;
	for (uint32_t i = 4; i < kRecordSize; i++) {
		bool ours = i == 0xb || (i >= 0x10 && i < 0x30) || (i >= 0x64 && i < 0x84);
		if (!ours && rec[i] != was[i]) {
			other += FormatA(" +%02x %02x->%02x", i, was[i], rec[i]);
		}
	}
	bool changed = rec[0xb] != was[0xb] || memcmp(rec + 0x10, was + 0x10, 0x20) || memcmp(rec + 0x64, was + 0x64, 0x20);
	rec[0xb] = was[0xb];
	memcpy(rec + 0x10, was + 0x10, 0x20);
	memcpy(rec + 0x64, was + 0x64, 0x20);
	XLS_LOG_INFO("mods: rules: rounds: player %d starts the round with the match's loadout (%s; other changes:%s).", slot,
	             changed ? "weapons/ammo put back" : "unchanged", other.empty() ? " none" : other.c_str());
}

uint32_t __attribute__((thiscall)) Spawn(uint8_t* spawner, uint32_t respawn, void* a, void* b)
{
	if (!spawner) {
		return g_spawnOriginal(spawner, respawn, a, b);
	}
	if (!(respawn & 0xff)) {
		RoundLoadout(spawner[kCtlSlot]);
	}
	PutSecondary(spawner[kCtlSlot], respawn & 0xff ? "respawns" : "spawns");
	bool second = (respawn & 0xff) && g_secondary && g_respawnSecond;
	if (second) {
		RespawnSecondJump(true);
	}
	g_inSpawn++;
	uint32_t r = g_spawnOriginal(spawner, respawn, a, b);
	g_inSpawn--;
	if ((respawn & 0xff) && b && g_magazine && g_grenadeCount != 0) { // the old grenade object carries over
		auto* m = (int16_t*)((uint8_t*)b + g_magazine);
		m[0] = m[1] = (int16_t)((g_grenadeCount - 1) * GrenadeCost(WeaponEntry((uint8_t*)b)));
	}
	int spawnSlot = spawner[kCtlSlot];
	if (spawnSlot < 16 && ((g_roundDead >> spawnSlot) & 1) && *(uint8_t**)(spawner + 0x40)) {
		// a new round's spawn of a player who was down at the last round's end: its old death view would carry on, count down
		// and respawn it again (a death)
		g_roundDead &= ~(1u << spawnSlot);
		XLS_LOG_INFO("mods: rules: rounds: player %d spawned for the new round, death view cleared (state %u -> 0).", spawnSlot, (unsigned)spawner[kCtlState]);
		spawner[kCtlState] = 0;
		((float*)(spawner + kCtlDeathTimers))[0] = 0.0f;
		((float*)(spawner + kCtlDeathTimers))[1] = 0.0f;
	}
	if (uint8_t* unit = *(uint8_t**)(spawner + 0x40)) {
		if (spawnSlot < 16 && g_unitTeam) {
			UnitTeam(spawnSlot); // its team while alive, for spectating once it is gone
			g_selfTeam[spawnSlot] = unit[g_unitTeam];
		}
		const float* at = (const float*)(unit + 0x30);
		XLS_LOG_INFO("mods: rules: player %u unit %p id %08x at %.0f %.0f %.0f.", spawner[kCtlSlot], unit, *(uint32_t*)(unit + 0x1c), at[0], at[1], at[2]);
	}
	else {
		XLS_LOG_WARN("mods: rules: player %u: no unit made.", spawner[kCtlSlot]);
	}
	if (respawn & 0xff) { // bug 14 (respawning with 0 grenades): the grenade object the controller handed over (+0xe0)
		auto* g = (uint8_t*)b;
		if (g && g_magazine) {
			auto* m = (int16_t*)(g + g_magazine);
			XLS_LOG_INFO("mods: rules: player %u respawn: weapon %p, grenades %p (kind 0x%02x, %d loaded, %d total).", spawner[kCtlSlot], a, b, g[0x1e], m[0], m[1]);
		}
		else {
			XLS_LOG_INFO("mods: rules: player %u respawn: weapon %p, no grenade object (the grenade slot is skipped).", spawner[kCtlSlot], a);
		}
	}
	if (second) {
		RespawnSecondJump(false);
		XLS_LOG_INFO("mods: rules: player %u respawned with secondary kind 0x%02x.", spawner[kCtlSlot], g_secondary.load());
	}
	return r;
}

// The game setup screen: the create-match screen (DX11 FUN_00e90180) without its four saved slots and three tabs. The map
// circle and ten rows stay. Rows show one page at a time (Setup below), scrolled when there are more than ten. The game
// still steps value rows, draws them and starts the match; the row table (read only by this screen) and the Flash's row
// list (flash/setup.as) hold the visible rows, and a nav row is a row of ours with no rule behind it.
const int kRuleRespawn = 0x2a, kRespawnOn = 1;
const uint32_t kMsgSettingsTab = 375; // MSG_SETTINGS_01-03: tab 1 is the page's title, the others are blank
uint8_t* g_rowTable = nullptr;
uint8_t g_stockRows[16 * 0x23]; // says which rules a category has
const int kCategories = 16, kSetupRows = 10;
const uint8_t kNavOffset = 0x37; // nothing reads it; left and right never reach the game on a nav row
const int kNavItem = 520; // flash/setup.as: 520 + row
std::string g_setupTitle;
const char* g_tabText[3] = {"", "", ""};

bool StockRow(int c, uint8_t offset)
{
	const uint8_t* cat = g_stockRows + c * 0x23;
	for (int page = 0; page < 3; page++) {
		const uint8_t* rows = cat + 2 + page * 0xb;
		for (int r = 0; r < rows[0] && r < 10; r++) {
			if (rows[1 + r] == offset) {
				return true;
			}
		}
	}
	return false;
}

int RowCategory(const uint8_t* rules)
{
	for (int c = 0; c < kCategories; c++) {
		const uint8_t* cat = g_stockRows + c * 0x23;
		if (cat[0] == rules[kRuleMain] && (rules[kRuleMain] >= 3 || cat[1] == rules[kRuleSub])) {
			return c;
		}
	}
	return 0;
}

// OnlineRule's item of a rule: its label and value texts.
int RuleItem(uint8_t offset, const uint8_t* rules)
{
	static const uint8_t kItems[][2] = {{0x0e, 4}, {0x09, 5}, {0x0a, 6}, {0x0c, 8}, {0x0b, 10}, {0x1e, 11}, {0x12, 12}, {0x13, 13},
		{0x1b, 14}, {0x03, 15}, {0x01, 16}, {0x08, 17}, {0x0f, 18}, {0x27, 19}, {0x28, 20}, {0x04, 21}, {0x05, 22}, {0x06, 23},
		{0x07, 24}, {0x29, 25}, {0x15, 26}, {0x2a, 27}, {0x20, 28}, {0x02, 29}, {0x26, 30}, {0x10, 31}, {0x11, 32}};
	if (offset == 0x0d) {
		return rules[kRuleMain] == 3 ? 9 : 7; // the egg target, or VS Annihilator's
	}
	for (const auto& pair : kItems) {
		if (pair[0] == offset) {
			return pair[1];
		}
	}
	for (const Row& r : kRowDefs) {
		if (r.offset == offset) {
			return r.item;
		}
	}
	return 12;
}

struct Section {
	const char* name;
	std::vector<uint8_t> offsets;
};
const std::vector<Section> kSections = {
	{"General", {0x12, 0x15, 0x13, 0x2a, 0x20, 0x08, 0x0f, 0x02, 0x10, 0x11, 0x29}},
	{"Rounds", {0x26}},
	{"Player", {0x03, 0x2e, 0x04, 0x34, 0x27, 0x28}}, // Bullet Stagger (0x36) is on the Stagger page
	{"Weapons", {0x05, 0x06, 0x2f, 0x31}},
	{"VS", {0x07}},
};
const size_t kSectionRounds = 1, kSectionPlayer = 2, kSectionWeapons = 3;
const uint8_t kRuleHalftime = 0x26; // the stock Halftime row: our Rounds rows stand in for it
std::string RoundsName(int index)
{
	return index < 10 ? std::to_string(index + 1) : "Best of " + std::to_string(3 + (index - 10) * 2);
} // these have a row of ours, so they're always there
const uint8_t kScoreRows[] = {0x01, 0x0e, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x1e};

enum SetupPage { kSetupTop, kSetupOptions, kSetupSection, kSetupTypes, kSetupEntries, kSetupName, kSetupBots, kSetupRadar, kSetupAnchor, kSetupStagger };
enum SetupKind { kKindValue, kKindNav, kKindHeader, kKindOwn };
const int kOwnBulletStagger = 100; // a kKindOwn row's arg for the rules byte 0x36 (not an extras key)
enum SetupAction { kActNone, kActTypes, kActType, kActOptions, kActCreate, kActSection, kActDefault, kActProfile, kActBots, kActRadar, kActAnchor, kActStagger };
const char* const kBotRuleKeys[] = {"bots", "botDifficulty", "botBrain", "botPersona", "botTaunts", "radar", "radarPlayers", "radarVs", "anchorRange", "anchorSpeed", "fallStagger", "grenadeCount", "grenadePickup", "quickResupply", "rounds", "switchSides", "radarTeam",
	"explosionStagger", "anchorStagger", "meleeStagger", "massiveShot", "massiveThreshold", "knockdownPush"};
const int kBotRuleCounts[] = {2, 4, 2, 6, 4, 3, 4, 4, 13, 12, kThresholdValues, 12, 11, 2, 14, 11, 2, kStaggerValues, kStaggerValues, kStaggerValues, kMassiveValues, kThresholdValues, kPushValues};
const char* const kRadarNames[] = {"Always On", "Near Data Post", "Hidden"};
const char* const kDetectNames[] = {"Always Show", "Moving & Shooting", "Running & Shooting", "Shooting"};
struct SetupRow {
	SetupKind kind;
	uint8_t offset;
	SetupAction action;
	int arg;
	std::string label, value;
};
struct SetupAt {
	int page, arg, cursor, top;
};
std::vector<SetupAt> g_setupStack;
std::vector<SetupRow> g_setupRows;
std::vector<GameProfile> g_setupProfiles;
std::string g_setupProfile; // "" = Default
int g_matchDesired = 0; // Players Desired, kept for the lobby: the create screen's rules are gone by then
std::wstring g_setupName;
bool g_setupEdited = false, g_setupOpen = false, g_setupLeaving = false;
std::atomic<bool> g_nextIsBotMatch{false};
bool g_setupBotMatch = false;
int g_botsBefore = -1;
int g_setupMouse = -1;

using CreateTickFn = void(__attribute__((thiscall)) *)(uint8_t*);
using CreateRefreshFn = void(__attribute__((thiscall)) *)(uint8_t*, int, char);
CreateTickFn g_createTickOriginal = nullptr;
CreateRefreshFn g_createRefresh = nullptr; // the screen's row states (Private greyed)
CreateTickFn g_createHelp = nullptr, g_createArrows = nullptr; // the help line's text, the value arrows
const uint32_t kCreateTick[kBuildCount] = {0xe90180, 0xe80030, 0xe90590, 0xe80550};
const uint32_t kCreateRefresh[kBuildCount] = {0xe8f7a0, 0xe7f650, 0xe8fbb0, 0xe7fb70};
const uint32_t kCreateHelp[kBuildCount] = {0xe8f4c0, 0xe7f370, 0xe8f8d0, 0xe7f890};
const uint32_t kCreateArrows[kBuildCount] = {0xe8f510, 0xe7f3c0, 0xe8f920, 0xe7f8e0};
const uint8_t kCreateTickPrologue[] = {0x83, 0xec, 0x28, 0x55, 0x8b, 0xe9}; // sub esp,0x28; push ebp; mov ebp,ecx
const uint32_t kCreateReady = 0x3cd, kCreateState = 0x3b8, kCreateCursor = 0x3b0, kCreateSlot = 0x418, kCreateSlots = 0x3d0, kCreateTab = 0x3c0;
const uint32_t kCreateHelpId = 0x3ca, kCreateHelpFlag = 0x3cc;
const uint32_t kInputButtons = 800, kButtonConfirm = 1, kButtonBack = 2, kButtonSave = 4;
const uint32_t kKeyStepUp = 0x40, kKeyStepDown = 0x80, kKeysStep = 0x100 | 0x200;
const int kMouseRow0 = 0x1fe, kMouseSave = 1, kMouseConfirm = 2, kMouseBack = 3, kMouseValueL = 10, kMouseValueR = 11;
const int kSoundMove = 7, kSoundConfirm = 8, kSoundBack = 9;

void __attribute__((thiscall)) PushRules(void* screen, uint8_t* rules);

std::string GameTypeName(int type)
{
	const GameType& t = GameTypes()[type];
	std::string main = MapsOnlineText(72 + t.main);
	if (main.empty()) {
		main = t.id; // the message file isn't loaded yet
	}
	if (t.main >= 3) {
		return main;
	}
	std::string sub = t.survival ? kSurvivalName : MapsOnlineText((t.main == 0 ? 79 : t.main == 1 ? 83 : 88) + t.sub);
	return sub.empty() ? main : main + " - " + sub;
}

void SetupBuild(const uint8_t* rules)
{
	g_setupRows.clear();
	g_matchDesired = rules[0x15];
	const SetupAt& at = g_setupStack.back();
	int c = RowCategory(rules);
	auto valueRow = [&](uint8_t offset) { g_setupRows.push_back({kKindValue, offset, kActNone, 0, offset == 0x01 ? "Round Time" : "", ""}); };
	auto navRow = [&](SetupKind kind, SetupAction action, int arg, const std::string& label, const std::string& value) {
		g_setupRows.push_back({kind, kNavOffset, action, arg, label, value});
	};
	auto hidden = [&](uint8_t offset) {
		return (offset == kRuleRespawn && SurvivalOn(rules) && rules[kRuleSub] == kSubKills) || (offset == 0x01 && rules[kRuleMain] == 4) ||
		       (offset == 0x31 && (rules[0x2f] & kInfiniteAmmo)); // infinite ammo: no supply to set
	};
	switch (at.page) {
	case kSetupTop: {
		g_setupTitle = "Game Setup";
		navRow(kKindNav, kActTypes, 0, "Game Mode", GameTypeName(GameTypeOf(rules)));
		navRow(kKindNav, kActType, GameTypeOf(rules), "Rules", (g_setupProfile.empty() ? "Default" : g_setupProfile) + (g_setupEdited ? " *" : ""));
		valueRow(0x1b);
		navRow(kKindNav, kActOptions, 0, "Game Options", ">");
		navRow(kKindNav, kActCreate, 0, "Begin Match", "");
		break;
	}
	case kSetupOptions:
		g_setupTitle = "Game Options";
		for (uint8_t offset : kScoreRows) {
			if (StockRow(c, offset) && !hidden(offset)) {
				valueRow(offset);
			}
		}
		for (size_t s = 0; s < kSections.size(); s++) {
			if (s == kSectionPlayer || s == kSectionWeapons) {
				navRow(kKindNav, kActSection, (int)s, kSections[s].name, ">");
				continue;
			}
			for (uint8_t offset : kSections[s].offsets) {
				if (StockRow(c, offset) && !hidden(offset)) {
					navRow(kKindNav, kActSection, (int)s, kSections[s].name, ">");
					break;
				}
			}
		}
		navRow(kKindNav, kActBots, 0, "Bots", g_matchBots ? "On" : "Off");
		navRow(kKindNav, kActRadar, 0, "Radar", kRadarNames[g_radarMode % 3]);
		navRow(kKindNav, kActAnchor, 0, "Anchor", ">");
		navRow(kKindNav, kActCreate, 0, "Begin Match", "");
		break;
	case kSetupSection:
		g_setupTitle = kSections[at.arg].name;
		for (uint8_t offset : kSections[at.arg].offsets) {
			if (StockRow(c, offset) && !hidden(offset) && offset != kRuleHalftime) {
				valueRow(offset);
			}
		}
		if ((size_t)at.arg == kSectionRounds) {
			int rounds = GameRulesExtras()["rounds"], every = GameRulesExtras()["switchSides"];
			navRow(kKindOwn, kActNone, 14, "Rounds", RoundsName(rounds));
			navRow(kKindOwn, kActNone, 15, "Switch Side Every # Rounds", every ? std::to_string(every) : "Never");
		}
		if ((size_t)at.arg == kSectionPlayer) {
			navRow(kKindNav, kActStagger, 0, "Stagger", ">");
		}
		if ((size_t)at.arg == kSectionPlayer && !(rules[0x2f] & kInfiniteGrenades)) {
			int count = GameRulesExtras()["grenadeCount"];
			navRow(kKindOwn, kActNone, 11, "Grenades at Spawn", count ? std::to_string(count - 1) : "Default");
		}
		if ((size_t)at.arg == kSectionWeapons && !(rules[0x2f] & kInfiniteGrenades)) {
			int pickup = GameRulesExtras()["grenadePickup"];
			navRow(kKindOwn, kActNone, 12, "Grenade Pickups", pickup ? std::to_string(pickup) : "Default");
		}
		if ((size_t)at.arg == kSectionWeapons) {
			navRow(kKindOwn, kActNone, 13, "Quick Resupply", GameRulesExtras()["quickResupply"] ? "On" : "Off");
		}
		break;
	case kSetupTypes:
		g_setupTitle = "Game Mode";
		for (int t = 0; t < (int)GameTypes().size(); t++) {
			navRow(kKindNav, kActType, t, GameTypeName(t), ">");
		}
		break;
	case kSetupEntries:
		g_setupTitle = GameTypeName(at.arg);
		g_setupProfiles = GameProfiles();
		navRow(kKindNav, kActDefault, at.arg, "Default", "");
		for (size_t p = 0; p < g_setupProfiles.size(); p++) {
			if (g_setupProfiles[p].type == at.arg) {
				navRow(kKindNav, kActProfile, (int)p, g_setupProfiles[p].name, "");
			}
		}
		break;
	case kSetupBots: {
		g_setupTitle = "Bots";
		std::map<std::string, int>& x = GameRulesExtras();
		std::string fill = g_matchBots ? "Fill to " + std::to_string((int)rules[0x15]) + " Players" : "Off";
		navRow(kKindOwn, kActNone, 0, "Bots", fill);
		navRow(kKindOwn, kActNone, 1, "Difficulty", kDifficultyNames[x[kBotRuleKeys[1]] & 3]);
		navRow(kKindOwn, kActNone, 2, "Brain", kBrainNames[x[kBotRuleKeys[2]] & 1]);
		navRow(kKindOwn, kActNone, 3, "Personality", kPersonaNames[x[kBotRuleKeys[3]] % 6]);
		navRow(kKindOwn, kActNone, 4, "Taunts", kTauntNames[x[kBotRuleKeys[4]] & 3]);
		break;
	}
	case kSetupRadar: {
		g_setupTitle = "Radar";
		std::map<std::string, int>& x = GameRulesExtras();
		navRow(kKindOwn, kActNone, 5, "Radar", kRadarNames[x["radar"] % 3]);
		navRow(kKindOwn, kActNone, 6, "Players Show", kDetectNames[x["radarPlayers"] % 4]);
		navRow(kKindOwn, kActNone, 7, "VS Show", kDetectNames[x["radarVs"] % 4]);
		navRow(kKindOwn, kActNone, 16, "Team Colours", x["radarTeam"] ? "On" : "Off");
		break;
	}
	case kSetupStagger: {
		g_setupTitle = "Stagger";
		std::map<std::string, int>& x = GameRulesExtras();
		// Bullet Stagger (rules byte 0x36, any hit not told apart): our own row, the game's stepper only knows its own pages
		navRow(kKindOwn, kActNone, kOwnBulletStagger, "Bullet Stagger", kStaggerNames[rules[0x36] % kStaggerValues]);
		navRow(kKindOwn, kActNone, 17, "Explosion Stagger", kStaggerNames[x["explosionStagger"] % kStaggerValues]);
		navRow(kKindOwn, kActNone, 18, "Anchor Stagger", kStaggerNames[x["anchorStagger"] % kStaggerValues]);
		navRow(kKindOwn, kActNone, 19, "Melee Stagger", kStaggerNames[x["meleeStagger"] % kStaggerValues]);
		navRow(kKindOwn, kActNone, 20, "Massive Shot", kMassiveNames[x["massiveShot"] % kMassiveValues]);
		navRow(kKindOwn, kActNone, 21, "Massive Shot Threshold", kThresholdNames[x["massiveThreshold"] % kThresholdValues]);
		navRow(kKindOwn, kActNone, 22, "Knockdown Force", kPushNames[x["knockdownPush"] % kPushValues]);
		navRow(kKindOwn, kActNone, 10, "Fall Stagger Threshold", kThresholdNames[x["fallStagger"] % kThresholdValues]);
		break;
	}
	case kSetupAnchor: {
		g_setupTitle = "Anchor";
		std::map<std::string, int>& x = GameRulesExtras();
		navRow(kKindOwn, kActNone, 8, "Range", kGrappleNames[x["anchorRange"] % kGrappleValues]);
		navRow(kKindOwn, kActNone, 9, "Speed", kGrappleNames[(x["anchorSpeed"] + 1) % kGrappleValues]);
		break;
	}
	case kSetupName:
		g_setupTitle = "Save Profile";
		navRow(kKindNav, kActNone, 0, "Profile Name", WideToUtf8(g_setupName.c_str()) + "_");
		break;
	}
}

// The row table: every category the same, as the rows are already filtered for the current one.
using CreateCheckFn = char(__cdecl*)();
CreateCheckFn g_createCheck = nullptr;

void SetupWriteTable()
{
	const SetupAt& at = g_setupStack.back();
	DWORD old = 0;
	if (!VirtualProtect(g_rowTable, sizeof(g_stockRows), PAGE_READWRITE, &old)) {
		return;
	}
	int count = std::min((int)g_setupRows.size() - at.top, kSetupRows);
	for (int c = 0; c < kCategories; c++) {
		uint8_t* page = g_rowTable + c * 0x23 + 2;
		memset(page, 0, 0xb);
		page[0] = (uint8_t)std::max(count, 0);
		for (int r = 0; r < count; r++) {
			page[1 + r] = g_setupRows[at.top + r].offset;
		}
	}
	VirtualProtect(g_rowTable, sizeof(g_stockRows), old, &old);
}

// The help line (stock, thiscall(screen)): the cursor row's rule offset indexes a short table of ticker messages
// (DX9 0x17d9780), which ends at offset 0x30, so our rows past it (Bullet Stagger 0x36 and others) read the row table
// after it as an index (Gato 2026-11-09: 0x2827). The ticker (DX9 FUN_00e7cb20) then indexes a 35-entry local array
// with it: a crash on DX9, garbage on DX11. Any index outside 0..34 becomes 0 (no message).
CreateTickFn g_createHelpOriginal = nullptr;
const uint8_t kCreateHelpPrologue[] = {0x56, 0x8b, 0xf1, 0x8b, 0x86, 0x18, 0x04, 0x00, 0x00}; // push esi; mov esi,ecx; mov eax,[esi+0x418]

void __attribute__((thiscall)) CreateHelp(uint8_t* screen)
{
	g_createHelpOriginal(screen);
	int16_t& id = *(int16_t*)(screen + kCreateHelpId);
	if (id < 0 || id > 34) {
		static LONG logged = 0;
		if (InterlockedIncrement(&logged) <= 5) {
			XLS_LOG_INFO("mods: rules: create screen help message %d out of range (row offset past the game's table), none shown.", id);
		}
		id = 0;
	}
}

void SetupSelect(uint8_t* screen)
{
	SetupAt& at = g_setupStack.back();
	*(int32_t*)(screen + kCreateCursor) = at.cursor - at.top;
	float row = (float)(at.cursor - at.top);
	FlashArg arg = {0, 3, nullptr};
	memcpy(&arg.text, &row, sizeof(row));
	g_flashCall(screen, 0, "_root.selectItem", &arg, 1);
	if (g_createArrows && g_createHelp) {
		g_createArrows(screen);
		g_createHelp(screen);
		if (g_setupRows[at.cursor].kind != kKindValue) {
			*(int16_t*)(screen + kCreateHelpId) = 0;
		}
		screen[kCreateHelpFlag] = 1;
	}
}

// Rows, title, row states and cursor of the current page.
void SetupShow(uint8_t* screen, const uint8_t* rules)
{
	SetupBuild(rules);
	SetupAt& at = g_setupStack.back();
	at.cursor = std::max(0, std::min(at.cursor, (int)g_setupRows.size() - 1));
	while (at.cursor < (int)g_setupRows.size() - 1 && g_setupRows[at.cursor].kind == kKindHeader) {
		at.cursor++;
	}
	at.top = std::max(0, std::min(at.top, at.cursor));
	if (at.cursor >= at.top + kSetupRows) {
		at.top = at.cursor - kSetupRows + 1;
	}
	SetupWriteTable();
	std::string items, labels, values, kinds;
	int count = std::min((int)g_setupRows.size() - at.top, kSetupRows);
	for (int r = 0; r < count; r++) {
		const SetupRow& row = g_setupRows[at.top + r];
		items += (r ? "|" : "") + std::to_string(row.kind == kKindValue ? RuleItem(row.offset, rules) : kNavItem + r);
		labels += (r ? "|" : "") + row.label;
		values += (r ? "|" : "") + row.value;
		int kind = 0; // for the Flash: 1 a value row of ours, 2 can go left, 4 can go right
		if (row.kind == kKindOwn) {
			bool bullet = row.arg == kOwnBulletStagger;
			int value = bullet ? rules[0x36] % kStaggerValues : GameRulesExtras()[kBotRuleKeys[row.arg]];
			kind = 1 | (value > 0 ? 2 : 0) | (value < (bullet ? kStaggerValues : kBotRuleCounts[row.arg]) - 1 ? 4 : 0);
			if (row.arg == 0 && g_setupBotMatch) {
				kind = 1; // a Bot Match has bots: the row shows it, no arrows
			}
		}
		kinds += (r ? "|" : "") + std::to_string(kind);
	}
	FlashArg args[4] = {{0, 4, items.c_str()}, {0, 4, labels.c_str()}, {0, 4, values.c_str()}, {0, 4, kinds.c_str()}};
	g_flashCall(screen, 0, "lp2SetupRows", args, 4);
	g_tabText[0] = g_setupTitle.c_str();
	FlashArg title = {0, 4, g_setupTitle.c_str()};
	g_flashCall(screen, 0, "lp2SetupTitle", &title, 1);
	if (g_createRefresh) {
		g_createRefresh(screen, 0, 1);
	}
	SetupSelect(screen);
}

void SetupOpenPage(uint8_t* screen, const uint8_t* rules, int page, int arg, int cursor)
{
	g_setupStack.push_back({page, arg, cursor, 0});
	SetupShow(screen, rules);
}

// Up or down one selectable row, wrapping; the window follows the cursor, and shows a header above its first row.
void SetupMove(uint8_t* screen, const uint8_t* rules, int step)
{
	SetupAt& at = g_setupStack.back();
	int count = (int)g_setupRows.size(), next = at.cursor;
	do {
		next = (next + step + count) % count;
	} while (g_setupRows[next].kind == kKindHeader && next != at.cursor);
	if (next == at.cursor) {
		return;
	}
	at.cursor = next;
	int top = at.top;
	if (next - 1 >= 0 && g_setupRows[next - 1].kind == kKindHeader && next - 1 < top) {
		top = next - 1;
	}
	top = std::min(top, next);
	if (next >= top + kSetupRows) {
		top = next - kSetupRows + 1;
	}
	MenuSound(screen, kSoundMove);
	if (top != at.top) {
		at.top = top;
		SetupShow(screen, rules);
	}
	else {
		SetupSelect(screen);
	}
}

// The profile name is typed into its row: the game's window hands us its characters while the page is open. Enter and
// Backspace also reach the game's own input as confirm and back, so those count only from a pad (the key isn't down); if a
// key's character never arrives (nothing typed so far), the button still acts after a moment, so the page can't trap anyone.
const size_t kNameMax = 32;
const int kNameButtonWait = 6;
HWND g_nameWindow = nullptr;
WNDPROC g_nameOldProc = nullptr;
std::mutex g_nameMutex;
std::wstring g_nameTyped;
bool g_nameCharSeen = false;
int g_nameWait = 0, g_setupIdleTicks = 0;
uint32_t g_nameWaiting = 0;

LRESULT CALLBACK NameWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_CHAR) {
		std::lock_guard<std::mutex> lock(g_nameMutex);
		g_nameTyped += (wchar_t)wParam;
		return 0;
	}
	return CallWindowProcW(g_nameOldProc, window, message, wParam, lParam);
}

void NameCaptureEnd()
{
	if (g_nameWindow) {
		SetWindowLongPtrW(g_nameWindow, GWLP_WNDPROC, (LONG_PTR)g_nameOldProc);
		g_nameWindow = nullptr;
	}
}

void NameCaptureStart()
{
	NameCaptureEnd();
	HWND window = GetForegroundWindow();
	DWORD process = 0;
	GetWindowThreadProcessId(window, &process);
	if (!window || process != GetCurrentProcessId()) {
		XLS_LOG_WARN("mods: game rules: the game's window isn't in front, no typing for the profile name.");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_nameMutex);
		g_nameTyped.clear();
	}
	g_nameOldProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)&NameWindowProc);
	g_nameWindow = g_nameOldProc ? window : nullptr;
	g_nameCharSeen = false;
	g_nameWaiting = 0;
}

void SetupNameOpen(uint8_t* screen, const uint8_t* rules)
{
	g_setupName = Utf8ToWide(g_setupProfile.c_str());
	NameCaptureStart();
	MenuSound(screen, kSoundConfirm);
	SetupOpenPage(screen, rules, kSetupName, 0, 0);
}

void SetupNameClose(uint8_t* screen, uint8_t* rules, bool save)
{
	NameCaptureEnd();
	g_setupIdleTicks = 4; // the key that closed it is still down
	std::string name = WideToUtf8(g_setupName.c_str());
	for (char& c : name) {
		c = c == '|' ? '/' : c;
	}
	bool saving = save && !name.empty() && GameProfileSave(rules, name);
	if (saving) {
		XLS_LOG_INFO("mods: game rules: saved profile %s.", name.c_str());
		g_setupProfile = name;
		g_setupEdited = false;
		GameRulesSaveCurrent(rules, g_setupProfile, g_setupEdited);
	}
	MenuSound(screen, save ? kSoundConfirm : kSoundBack);
	g_setupStack.pop_back();
	SetupShow(screen, rules);
}

// One tick of the name page.
void SetupNameTick(uint8_t* screen, uint8_t* rules, uint32_t buttons)
{
	std::wstring typed;
	{
		std::lock_guard<std::mutex> lock(g_nameMutex);
		typed.swap(g_nameTyped);
	}
	bool changed = false;
	for (wchar_t c : typed) {
		g_nameCharSeen = true;
		g_nameWaiting = 0;
		if (c == L'\r' || c == L'\n') {
			if (!g_setupName.empty()) {
				SetupNameClose(screen, rules, true);
				return;
			}
		}
		else if (c == 27) {
			SetupNameClose(screen, rules, false);
			return;
		}
		else if (c == 8) {
			if (!g_setupName.empty()) {
				g_setupName.pop_back();
				changed = true;
			}
		}
		else if (c >= 32 && g_setupName.size() < kNameMax) {
			g_setupName += c;
			changed = true;
		}
	}
	if (changed) {
		MenuSound(screen, kSoundMove);
		SetupShow(screen, rules);
	}
	uint32_t button = buttons & (kButtonConfirm | kButtonBack);
	if (button && !g_nameWaiting) {
		bool key = (GetAsyncKeyState(button & kButtonConfirm ? VK_RETURN : VK_BACK) & 0x8000) != 0;
		if (key && g_nameCharSeen) {
			button = 0; // typing, already handled above
		}
		else if (key) {
			g_nameWaiting = button; // its character may be on the way
			g_nameWait = kNameButtonWait;
			button = 0;
		}
	}
	else if (!button && g_nameWaiting && --g_nameWait <= 0) {
		button = g_nameWaiting;
		g_nameWaiting = 0;
	}
	if (button & kButtonBack) {
		SetupNameClose(screen, rules, false);
	}
	else if (button & kButtonConfirm) {
		if (g_setupName.empty()) { // a pad can't type: the next free number
			std::vector<GameProfile> profiles = GameProfiles();
			for (int n = 1; n < 1000 && g_setupName.empty(); n++) {
				std::string name = "Profile " + std::to_string(n);
				bool used = false;
				for (const GameProfile& p : profiles) {
					used |= p.name == name;
				}
				if (!used) {
					g_setupName = Utf8ToWide(name.c_str());
				}
			}
		}
		SetupNameClose(screen, rules, true);
	}
}

// Runs one game tick with only these inputs, so the screen does what it would for them and nothing else.
void SetupRunGame(uint8_t* screen, uint32_t pressed, uint32_t buttons, uint32_t clicked, int32_t mouseId)
{
	uint32_t& inPressed = *(uint32_t*)(*g_input + kInputPressed);
	uint32_t& inButtons = *(uint32_t*)(*g_input + kInputButtons);
	uint32_t& inClick = *(uint32_t*)(screen + kMenuClick);
	int32_t& inMouse = *(int32_t*)(screen + kMenuMouse);
	uint32_t heldPressed = inPressed, heldButtons = inButtons, heldClick = inClick;
	int32_t heldMouse = inMouse;
	inPressed = pressed;
	inButtons = buttons;
	inClick = clicked;
	inMouse = mouseId;
	g_createTickOriginal(screen);
	inPressed = heldPressed;
	inButtons = heldButtons;
	inClick = heldClick;
	inMouse = heldMouse;
}

// The bot rules from the rule file to where the game's bot code reads them.
void BotRulesSync()
{
	std::map<std::string, int>& x = GameRulesExtras();
	if (g_setupBotMatch) {
		x["bots"] = 1; // loading a rules profile can't turn them off either
	}
	g_matchBots = x["bots"] != 0;
	g_botDifficulty = x["botDifficulty"];
	g_botBrain = x["botBrain"];
	g_botPersona = x["botPersona"];
	g_botTaunts = x["botTaunts"];
	g_radarMode = x["radar"];
	g_radarPlayers = x["radarPlayers"];
	g_radarVs = x["radarVs"];
	g_radarTeamRule = x["radarTeam"];
	g_anchorRangeRule = x["anchorRange"];
	g_anchorSpeedRule = x["anchorSpeed"];
	g_fallRule = x["fallStagger"];
	g_massiveRule = x["massiveShot"];
	g_massiveThresholdRule = x["massiveThreshold"];
	g_pushRule = x["knockdownPush"];
	g_grenadeCountRule = x["grenadeCount"];
	g_grenadePickupRule = x["grenadePickup"];
	g_quickResupplyRule = x["quickResupply"];
	g_roundsRule = x["rounds"];
	g_switchRule = x["switchSides"];
	g_staggerExplosionRule = x["explosionStagger"];
	g_staggerAnchorRule = x["anchorStagger"];
	g_staggerMeleeRule = x["meleeStagger"];
}

// Keep what carries between types, take the entry's rules, tell the game.
void SetupPick(uint8_t* screen, uint8_t* rules, const SetupRow& row)
{
	if (row.action == kActDefault) {
		GameRulesDefaults(row.arg, rules);
		g_setupProfile.clear();
	}
	else {
		GameProfileApply(g_setupProfiles[row.arg], rules);
		g_setupProfile = g_setupProfiles[row.arg].name;
	}
	g_setupEdited = false;
	BotRulesSync();
	PushRules(screen, rules);
	GameRulesSaveCurrent(rules, g_setupProfile, g_setupEdited);
	g_setupStack.resize(1);
	SetupShow(screen, rules);
}

const uint32_t kCreateDialog = 0x480, kDialogResult = 0x4e8;

// Begin Match without the window: what the game does when its "create match?" dialog says yes (FUN_00e90180 state 6). State 7 waits, and with
// +0x19 = 1 the screen's FUN_00e8f5e0 makes the session. The check (FUN_00e8cb60) puts up its own error when it says no.
void SetupBegin(uint8_t* screen)
{
	bool ok = g_createCheck ? g_createCheck() != 0 : true;
	XLS_LOG_INFO("mods: game rules: begin match, check %s.", ok ? "passed" : "refused");
	if (!ok) {
		return;
	}
	*(int32_t*)(screen + 0x3bc) = *(int32_t*)(screen + kCreateState); // where a failure comes back to
	screen[0x19] = 1;
	*(int32_t*)(screen + kCreateState) = 7;
	*(float*)(screen + 0x39c) = 300.0f; // the game's: 5 seconds' worth of frames
	screen[kCreateHelpFlag] = 1;
}

void SetupConfirm(uint8_t* screen, uint8_t* rules)
{
	const SetupRow row = g_setupRows[g_setupStack.back().cursor];
	switch (row.action) {
	case kActTypes:
		MenuSound(screen, kSoundConfirm);
		SetupOpenPage(screen, rules, kSetupTypes, 0, GameTypeOf(rules)); // starts on what's in use
		break;
	case kActType: {
		int cursor = 0;
		g_setupProfiles = GameProfiles();
		if (row.arg == GameTypeOf(rules) && !g_setupProfile.empty()) {
			int n = 0;
			for (const GameProfile& p : g_setupProfiles) {
				if (p.type == row.arg) {
					n++;
					cursor = p.name == g_setupProfile ? n : cursor;
				}
			}
		}
		MenuSound(screen, kSoundConfirm);
		SetupOpenPage(screen, rules, kSetupEntries, row.arg, cursor);
		break;
	}
	case kActOptions:
		MenuSound(screen, kSoundConfirm);
		SetupOpenPage(screen, rules, kSetupOptions, 0, 0);
		break;
	case kActBots:
		MenuSound(screen, kSoundConfirm);
		SetupOpenPage(screen, rules, kSetupBots, 0, 0);
		break;
	case kActAnchor:
		MenuSound(screen, kSoundConfirm);
		SetupOpenPage(screen, rules, kSetupAnchor, 0, 0);
		break;
	case kActStagger:
		MenuSound(screen, kSoundConfirm);
		SetupOpenPage(screen, rules, kSetupStagger, 0, 0);
		break;
	case kActRadar:
		MenuSound(screen, kSoundConfirm);
		SetupOpenPage(screen, rules, kSetupRadar, 0, 0);
		break;
	case kActSection:
		MenuSound(screen, kSoundConfirm);
		SetupOpenPage(screen, rules, kSetupSection, row.arg, 0);
		break;
	case kActCreate:
		GameRulesSaveCurrent(rules, g_setupProfile, g_setupEdited);
		{
			// The game's own "create match?" yes / no (FUN_00efa8e0), whose yes starts it. The window opens only if it's idle (its
			// +0x18); if it isn't, the screen would sit waiting for an answer nobody can give, so go on as a yes would.
			uint8_t* dialog = *(uint8_t**)(screen + kCreateDialog);
			if (g_setupBotMatch) {
				rules[0x13] = rules[0x12]; // every seat private: nobody finds it on the network
			}
			XLS_LOG_INFO("mods: game rules: begin match with rule %d, sub %d, survival byte %d, map %d (menu survival %d).", rules[kRuleMain], rules[kRuleSub], rules[kRuleSurvival], rules[0x1b], (int)g_menuSurvival.load());
			XLS_LOG_INFO("mods: game rules: begin match, dialog state %d, result %d.", dialog ? dialog[0x18] : -1, dialog ? *(int32_t*)(dialog + kDialogResult) : -1);
			if (dialog && dialog[0x18] != 0) {
				MenuSound(screen, kSoundConfirm);
				SetupBegin(screen);
				break;
			}
			SetupRunGame(screen, 0, kButtonConfirm, 0, 0);
		}
		break;
	case kActDefault:
	case kActProfile:
		MenuSound(screen, kSoundConfirm);
		SetupPick(screen, rules, row);
		break;
	default:
		break;
	}
}

void __attribute__((thiscall)) CreateTick(uint8_t* screen)
{
	if (!screen[kCreateReady]) {
		g_setupOpen = false;
		g_setupMouse = -1;
		NameCaptureEnd();
		g_createTickOriginal(screen);
		return;
	}
	int32_t state = *(int32_t*)(screen + kCreateState);
	uint8_t* rules = *(uint8_t**)(screen + kCreateSlots);
	static int32_t lastState = -1;
	// Back left the screen, which doesn't always tick while closed (the reset above): if it hasn't ticked for a while,
	// this is a new visit, not the same one leaving. Else the setup stayed "leaving" and the stock rows came back.
	static DWORD lastTick = 0;
	DWORD tickNow = GetTickCount();
	if (g_setupLeaving && tickNow - lastTick > 500) {
		XLS_LOG_INFO("mods: game rules: create screen back after %lu ms: setup reset.", (unsigned long)(tickNow - lastTick));
		g_setupOpen = false;
		g_setupLeaving = false;
		g_setupMouse = -1;
		lastState = -1;
	}
	lastTick = tickNow;
	if (state != lastState) {
		uint8_t* dialog = *(uint8_t**)(screen + kCreateDialog);
		XLS_LOG_INFO("mods: game rules: create screen state %d -> %d (phase %d, dialog state %d, result %d).", lastState, state, screen[0x19], dialog ? dialog[0x18] : -1, dialog ? *(int32_t*)(dialog + kDialogResult) : -1);
		lastState = state;
	}
	if (!g_setupOpen || (state == 0 && !g_setupLeaving)) { // a closed screen doesn't always tick before it's back
		if (state != 0 || !rules || !*g_input) {
			g_createTickOriginal(screen);
			return;
		}
		// Open on the rows: load the file, then let the game take the first slot as if it were picked.
		g_setupOpen = true;
		g_setupLeaving = false;
		g_setupStack.clear();
		g_setupStack.push_back({kSetupTop, 0, 0, 0});
		g_setupProfile.clear();
		g_setupEdited = false;
		bool loaded = GameRulesLoadCurrent(rules, &g_setupProfile, &g_setupEdited);
		XLS_LOG_INFO("mods: game rules: setup opened, %s.", loaded ? "current.json loaded" : "no rule file yet");
		{
			std::map<std::string, int>& x = GameRulesExtras();
			g_setupBotMatch = g_nextIsBotMatch;
			g_nextIsBotMatch = false;
			if (g_setupBotMatch) { // from the Multiplayer screen's Bot Match: bots on, this time only
				g_botsBefore = x["bots"];
				x["bots"] = 1;
			}
			else if (g_botsBefore >= 0) {
				x["bots"] = g_botsBefore;
				g_botsBefore = -1;
			}
		}
		BotRulesSync();
		g_botsInMatch = false;
		*(int32_t*)(screen + kCreateCursor) = 0;
		*(int32_t*)(screen + kCreateTab) = 1;
		PushRules(screen, rules);
		SetupBuild(rules);
		SetupWriteTable();
		SetupRunGame(screen, 0, kButtonConfirm, 0, 0);
		SetupShow(screen, rules);
		return;
	}
	if (state != 1 || !rules || !*g_input) {
		g_createTickOriginal(screen);
		return;
	}
	*(int32_t*)(screen + kCreateSlot) = 0;
	SetupAt& at = g_setupStack.back();
	uint32_t pressed = *(uint32_t*)(*g_input + kInputPressed);
	uint32_t buttons = *(uint32_t*)(*g_input + kInputButtons);
	uint32_t click = *(uint32_t*)(screen + kMenuClick);
	int32_t mouse = *(int32_t*)(screen + kMenuMouse);
	int32_t& wheel = *(int32_t*)(screen + kMenuWheel);
	bool clicked = (click & 1) != 0;
	int mouseRow = mouse >= kMouseRow0 && mouse < kMouseRow0 + kSetupRows ? mouse - kMouseRow0 : -1;
	if (at.page == kSetupName) {
		SetupNameTick(screen, rules, buttons | ((click & 2) || (clicked && mouse == kMouseBack) ? kButtonBack : 0) | (clicked && mouse == kMouseConfirm ? kButtonConfirm : 0));
		return;
	}
	if (g_setupIdleTicks > 0) {
		g_setupIdleTicks--;
		SetupRunGame(screen, 0, 0, 0, 0);
		return;
	}
	bool back = (buttons & kButtonBack) || (click & 2) || (clicked && mouse == kMouseBack);
	bool confirm = (buttons & kButtonConfirm) || (clicked && mouse == kMouseConfirm);

	if ((buttons & kButtonSave) || (clicked && mouse == kMouseSave)) {
		SetupNameOpen(screen, rules);
		return;
	}
	if (back) {
		if (g_setupStack.size() > 1) {
			MenuSound(screen, kSoundBack);
			g_setupStack.pop_back();
			SetupShow(screen, rules);
		}
		else {
			g_setupLeaving = true;
			*(int32_t*)(screen + kCreateState) = 0; // the slot list's back leaves the screen
			SetupRunGame(screen, 0, kButtonBack, 0, 0);
		}
		return;
	}
	// A row under the mouse takes the cursor when the mouse moves to it, not when a scroll moves the rows under it.
	if (mouseRow >= 0 && mouse != g_setupMouse && at.top + mouseRow < (int)g_setupRows.size() && g_setupRows[at.top + mouseRow].kind != kKindHeader && at.top + mouseRow != at.cursor) {
		at.cursor = at.top + mouseRow;
		MenuSound(screen, kSoundMove);
		SetupSelect(screen);
	}
	g_setupMouse = mouse;
	const SetupRow& now = g_setupRows[at.cursor];
	if (confirm || (clicked && mouseRow >= 0 && now.kind == kKindNav)) {
		if (now.kind == kKindNav) {
			SetupConfirm(screen, rules);
		}
		return;
	}
	if (pressed & kKeyStepUp) {
		SetupMove(screen, rules, -1);
		return;
	}
	if (pressed & kKeyStepDown) {
		SetupMove(screen, rules, 1);
		return;
	}
	if (wheel != 0) {
		int step = wheel > 0 ? 1 : -1;
		wheel = 0;
		SetupMove(screen, rules, step);
		return;
	}
	if (now.kind == kKindOwn) {
		int dir = (pressed & kKeyRight) || (clicked && mouse == kMouseValueR) ? 1 : (pressed & kKeyLeft) || (clicked && mouse == kMouseValueL) ? -1 : 0;
		if (dir && now.arg == kOwnBulletStagger) {
			int at = rules[0x36] % kStaggerValues + dir;
			if (at >= 0 && at < kStaggerValues) {
				rules[0x36] = (uint8_t)at;
				g_menu[kRowStagger] = at;
				g_menuSeen = true;
				g_setupEdited = true;
				GameRulesSaveCurrent(rules, g_setupProfile, g_setupEdited);
				MenuSound(screen, kSoundMove);
				SetupShow(screen, rules);
			}
		}
		else if (dir && !(now.arg == 0 && g_setupBotMatch)) {
			int& value = GameRulesExtras()[kBotRuleKeys[now.arg]];
			int at = value + dir;
			if (at >= 0 && at < kBotRuleCounts[now.arg]) {
				value = at;
				BotRulesSync();
				g_setupEdited = true;
				GameRulesSaveCurrent(rules, g_setupProfile, g_setupEdited);
				MenuSound(screen, kSoundMove);
				SetupShow(screen, rules);
			}
		}
		SetupRunGame(screen, 0, 0, 0, 0);
		return;
	}
	if (now.kind != kKindValue) {
		SetupRunGame(screen, 0, 0, 0, 0);
		return;
	}
	// A value row: left and right go to the game, which steps it through the row table and pushes the rules.
	uint8_t before[0x38];
	memcpy(before, rules, sizeof(before));
	bool arrow = clicked && (mouse == kMouseValueL || mouse == kMouseValueR);
	SetupRunGame(screen, pressed & kKeysStep, 0, arrow ? 1 : 0, arrow ? mouse : 0);
	if (memcmp(before, rules, sizeof(before)) != 0) {
		g_setupEdited |= now.offset != 0x1b && now.offset != 0x12 && now.offset != 0x13 && now.offset != 0x15;
		GameRulesSaveCurrent(rules, g_setupProfile, g_setupEdited);
		if (now.offset == 0x2f) {
			SetupShow(screen, rules); // infinite ammo / grenades hide rows; they're all below this one, so the cursor stays on it
		}
	}
}

using IsHostFn = char(__attribute__((thiscall)) *)(void*);
IsHostFn g_isHost = nullptr;
uint8_t** g_lobbySession = nullptr;

// Lobby Match Settings: the host changes the match's rules from the lobby, on the overlay, without backing out to Create
// Match. F1 opens and closes it (the pad's Y is the lobby's gamer card); up / down pick a row, left / right change it,
// Back closes it. While it's open the lobby gets no input. Nothing changes until it closes.
//   The game's own rules: the lobby's rule block (match setup +0x5e, 0x38 bytes, the create screen's layout; every
//   machine has one). The host's goes to the others as member data "srules" and each one writes it into its own block
//   while in the lobby, so nothing rests on how the game sends it. The game's push (FUN_00e8c270) fills in the bytes it
//   derives (category, random weapon / grenade / layout flags, map size). Mode, players and private slots stay as made.
//   Map: the stock maps the host has and our packages; a package is picked (and published, so players without it
//   download it; the start waits for them) only when the panel closes, so scrolling past maps doesn't start downloads.
//   Our rules: extras and the g_menu rows, sent as they always are (SyncThread, member data "rules").
// On close the rules go to current.json and the next Create Match starts from them.
const uint32_t kSetupBlock = 0x5e;
const int kBlockSize = 0x38;
void LobbyBlockDerive(uint8_t* block); // after the push hook

enum LobbyKind { kLobbyOwn, kLobbyMenu, kLobbyStock, kLobbyMap };
struct LobbyRow {
	const char* section;
	const char* label;
	int kind;
	int arg; // extras index, g_menu row or rules offset
};
const LobbyRow kLobbyOwnRows[] = {
	{"Rounds", "Rounds", kLobbyOwn, 14},
	{"Rounds", "Switch Side Every # Rounds", kLobbyOwn, 15},
	{"Player", "Default Secondary", kLobbyMenu, kRowSecondary},
	{"Player", "Harmonizer", kLobbyMenu, kRowHarmonizer},
	{"Player", "Grenades at Spawn", kLobbyOwn, 11},
	{"Stagger", "Bullet Stagger", kLobbyMenu, kRowStagger},
	{"Stagger", "Explosion Stagger", kLobbyOwn, 17},
	{"Stagger", "Anchor Stagger", kLobbyOwn, 18},
	{"Stagger", "Melee Stagger", kLobbyOwn, 19},
	{"Stagger", "Massive Shot", kLobbyOwn, 20},
	{"Stagger", "Massive Shot Threshold", kLobbyOwn, 21},
	{"Stagger", "Knockdown Force", kLobbyOwn, 22},
	{"Stagger", "Fall Stagger Threshold", kLobbyOwn, 10},
	{"Weapons", "Infinite Ammo", kLobbyMenu, kRowInfinite},
	{"Weapons", "Ammo Supply", kLobbyMenu, kRowSupply},
	{"Weapons", "Grenade Pickups", kLobbyOwn, 12},
	{"Weapons", "Quick Resupply", kLobbyOwn, 13},
	{"Anchor", "Range", kLobbyOwn, 8},
	{"Anchor", "Speed", kLobbyOwn, 9},
	{"Radar", "Radar", kLobbyOwn, 5},
	{"Radar", "Players Show", kLobbyOwn, 6},
	{"Radar", "VS Show", kLobbyOwn, 7},
	{"Radar", "Team Colours", kLobbyOwn, 16},
	{"Bots", "Bots", kLobbyOwn, 0},
	{"Bots", "Difficulty", kLobbyOwn, 1},
	{"Bots", "Brain", kLobbyOwn, 2},
	{"Bots", "Personality", kLobbyOwn, 3},
	{"Bots", "Taunts", kLobbyOwn, 4},
};

// A stock rule's texts and range, by its OnlineRule item (RuleItem): MsgOnline value base (-1: the number itself),
// first and last stored value, and the stored -> shown remap some rows have (FUN_00e8c270).
struct StockRange {
	int base, lo, hi;
};
StockRange StockRangeOf(int item)
{
	switch (item) {
	case 4: return {91, 0, 4};    // Battle Gauge
	case 5: return {96, 0, 5};    // Kills
	case 6: return {102, 0, 5};   // Points
	case 7: return {108, 0, 2};   // VS Leader
	case 8: return {111, 0, 3};   // Control Time
	case 9: return {115, 0, 3};   // Goals
	case 10: return {119, 0, 2};  // Fugitive Selection
	case 11: return {122, 2, 5};  // Rounds (Battle Series): shown as value - 2
	case 15: return {149, 0, 5};  // Default Weapons
	case 16: return {155, 0, 5};  // Time Limit
	case 17: return {430, 0, 4};  // Layout
	case 18: return {165, 0, 2};  // Friendly Fire (remapped)
	case 21: return {177, 0, 6};  // Default Grenades (remapped)
	case 22: return {188, 0, 3};  // Set Weapons
	case 23: return {192, 0, 3};  // Set Grenades
	case 24: return {196, 0, 3};  // Set VSs
	case 26: return {-1, 2, 16};  // Players Desired
	case 31: return {168, 0, 3};  // Teamkill Penalty 1
	case 32: return {172, 0, 4};  // Teamkill Penalty 2
	default: return {kMsgOffOn, 0, 1};
	}
}

std::string StockValueName(uint8_t offset, const uint8_t* block)
{
	int item = RuleItem(offset, block), v = (int8_t)block[offset];
	StockRange r = StockRangeOf(item);
	if (r.base < 0) {
		return std::to_string(v);
	}
	int shown = item == 11 ? v - 2 : item == 18 ? (v >= 0 && v < 3 ? "\x01\x02\x00"[v] : v) : item == 21 ? (v >= 0 && v < 7 ? "\x06\x00\x01\x02\x03\x04\x05"[v] : v) : v;
	std::string text = MapsOnlineText(r.base + shown);
	return text.empty() ? std::to_string(v) : text;
}

struct LobbyLine {
	std::string section, label, value;
	int kind, arg;
};
std::mutex g_lobbyPanelMutex;
std::vector<LobbyLine> g_lobbyLines; // the draw's copy
int g_lobbyCursorShown = 0;
std::atomic<bool> g_lobbyPanelOpen{false}, g_lobbyHost{false};
std::atomic<DWORD> g_lobbySeenAt{0};
int g_lobbyCursor = 0;
bool g_lobbyChanged = false, g_lobbyStockChanged = false, g_lobbyCarry = false;
uint8_t g_lobbyBlock[kBlockSize];      // the panel's copy of the rule block while it's open
uint8_t g_lobbyCarryBlock[kBlockSize]; // the last closed panel's rules, for the next create screen
std::vector<LobbyMap> g_lobbyMaps;
int g_lobbyMapAt = 0;
bool g_lobbyKeyWas = false;
uint8_t* g_lobbyScreen = nullptr; // for the menu sounds
std::string g_lobbyPublished;
std::mutex g_hostBlockMutex;
std::string g_hostBlock; // the lobby owner's "srules" (hex)
uint64_t g_hostBlockFrom = 0;
std::string g_hostBlockApplied;

uint8_t* LobbySlots()
{
	return g_ruleSlots && *g_ruleSlots ? *g_ruleSlots : nullptr;
}

std::string Hex(const uint8_t* bytes, int n)
{
	std::string out;
	for (int i = 0; i < n; i++) {
		out += FormatA("%02x", bytes[i]);
	}
	return out;
}

bool Unhex(const std::string& text, uint8_t* out, int n)
{
	if ((int)text.size() != n * 2) {
		return false;
	}
	for (int i = 0; i < n; i++) {
		unsigned v = 0;
		if (sscanf(text.c_str() + i * 2, "%2x", &v) != 1) {
			return false;
		}
		out[i] = (uint8_t)v;
	}
	return true;
}

std::string OwnValueName(int own, int v)
{
	switch (own) {
	case 0: return v ? "Fill to " + std::to_string(g_matchDesired) + " Players" : "Off";
	case 1: return kDifficultyNames[v & 3];
	case 2: return kBrainNames[v & 1];
	case 3: return kPersonaNames[v % 6];
	case 4: return kTauntNames[v & 3];
	case 5: return kRadarNames[v % 3];
	case 6: case 7: return kDetectNames[v % 4];
	case 8: return kGrappleNames[v % kGrappleValues];
	case 9: return kGrappleNames[(v + 1) % kGrappleValues];
	case 10: case 21: return kThresholdNames[v % kThresholdValues];
	case 20: return kMassiveNames[v % kMassiveValues];
	case 22: return kPushNames[v % kPushValues];
	case 11: return v ? std::to_string(v - 1) : "Default";
	case 12: return v ? std::to_string(v) : "Default";
	case 13: case 16: return v ? "On" : "Off";
	case 14: return RoundsName(v);
	case 15: return v ? std::to_string(v) : "Never";
	default: return kStaggerNames[v % kStaggerValues];
	}
}

std::string MenuValueName(int row, int v)
{
	switch (row) {
	case kRowSecondary: return kSecondaryNames[v % kSecondaryValues];
	case kRowInfinite: return kInfiniteNames[v % kInfiniteValues];
	case kRowSupply: return kSupplyNames[v % kSupplyValues];
	case kRowStagger: return kStaggerNames[v % kStaggerValues];
	default: return v ? "On" : "Off";
	}
}

// The stock rows the block's mode has (the create screen's row table), less the ones we don't change here.
bool LobbyStockShown(uint8_t offset, const uint8_t* b, int c)
{
	if (offset == 0x12 || offset == 0x13 || offset == kRuleHalftime || offset == 0x1b) {
		return false;
	}
	for (const Row& r : kRowDefs) {
		if (r.offset == offset) {
			return false; // ours: a g_menu row
		}
	}
	if ((offset == kRuleRespawn && SurvivalOn(b) && b[kRuleSub] == kSubKills) || (offset == 0x01 && b[kRuleMain] == 4)) {
		return false;
	}
	return offset == 0x01 || StockRow(c, offset);
}

std::vector<LobbyLine> LobbyBuild()
{
	std::map<std::string, int>& x = GameRulesExtras();
	const uint8_t* b = g_lobbyBlock;
	int c = RowCategory(b);
	std::vector<LobbyLine> lines;
	if (!g_lobbyMaps.empty()) {
		lines.push_back({"Match", MapsOnlineText(38 + 14).empty() ? "Map" : MapsOnlineText(38 + 14), g_lobbyMaps[g_lobbyMapAt].name, kLobbyMap, 0});
	}
	auto stock = [&](const char* section, uint8_t offset) {
		if (LobbyStockShown(offset, b, c)) {
			std::string label = MapsOnlineText(38 + RuleItem(offset, b));
			lines.push_back({section, label.empty() ? FormatA("Rule %02x", offset) : label, StockValueName(offset, b), kLobbyStock, offset});
		}
	};
	for (uint8_t offset : kScoreRows) {
		stock("Match", offset);
	}
	for (const Section& sec : kSections) {
		for (uint8_t offset : sec.offsets) {
			stock(sec.name, offset);
		}
		for (const LobbyRow& r : kLobbyOwnRows) {
			if (strcmp(r.section, sec.name) == 0) {
				int v = r.kind == kLobbyOwn ? x[kBotRuleKeys[r.arg]] : (int)g_menu[r.arg];
				lines.push_back({r.section, r.label, r.kind == kLobbyOwn ? OwnValueName(r.arg, v) : MenuValueName(r.arg, v), r.kind, r.arg});
			}
		}
	}
	for (const LobbyRow& r : kLobbyOwnRows) { // our sections the stock list doesn't have
		bool known = false;
		for (const Section& sec : kSections) {
			known |= strcmp(r.section, sec.name) == 0;
		}
		if (!known) {
			int v = r.kind == kLobbyOwn ? x[kBotRuleKeys[r.arg]] : (int)g_menu[r.arg];
			lines.push_back({r.section, r.label, r.kind == kLobbyOwn ? OwnValueName(r.arg, v) : MenuValueName(r.arg, v), r.kind, r.arg});
		}
	}
	return lines;
}

void LobbyPanelSnapshot()
{
	std::vector<LobbyLine> lines = LobbyBuild();
	g_lobbyCursor = std::clamp(g_lobbyCursor, 0, std::max(0, (int)lines.size() - 1));
	std::lock_guard<std::mutex> lock(g_lobbyPanelMutex);
	g_lobbyLines.swap(lines);
	g_lobbyCursorShown = g_lobbyCursor;
}

void LobbyPanelStep(const LobbyLine& row, int dir)
{
	bool changed = false;
	if (row.kind == kLobbyOwn) {
		int& value = GameRulesExtras()[kBotRuleKeys[row.arg]];
		int at = value + dir;
		if (at >= 0 && at < kBotRuleCounts[row.arg]) {
			value = at;
			BotRulesSync();
			changed = true;
		}
	}
	else if (row.kind == kLobbyMenu) {
		int n = kRowDefs[row.arg].values, was = g_menu[row.arg] % n, at = was + dir;
		at = row.arg == kRowSupply ? (at < 0 || at >= n ? was : at) : (at + n) % n; // supply: least to most, no wrap
		g_menu[row.arg] = at;
		g_menuSeen = true;
		changed = at != was;
	}
	else if (row.kind == kLobbyStock) {
		uint8_t offset = (uint8_t)row.arg;
		StockRange r = StockRangeOf(RuleItem(offset, g_lobbyBlock));
		int hi = r.base < 0 ? std::max<int>(r.lo, std::min<int>(r.hi, g_lobbyBlock[0x12])) : r.hi; // desired: up to max players
		int v = (int8_t)g_lobbyBlock[offset] + dir;
		if (v >= r.lo && v <= hi) {
			g_lobbyBlock[offset] = (uint8_t)v;
			g_lobbyStockChanged = changed = true;
		}
	}
	else if (row.kind == kLobbyMap && !g_lobbyMaps.empty()) {
		int n = (int)g_lobbyMaps.size();
		g_lobbyMapAt = (g_lobbyMapAt + dir + n) % n;
		g_lobbyBlock[0x1b] = g_lobbyMaps[g_lobbyMapAt].map;
		g_lobbyStockChanged = changed = true;
	}
	if (changed) {
		g_lobbyChanged = true;
		MenuSound(g_lobbyScreen, kSoundMove);
	}
}

void LobbyPanelOpen()
{
	uint8_t* slots = LobbySlots();
	if (slots) {
		memcpy(g_lobbyBlock, slots + kSetupBlock, kBlockSize);
	}
	g_lobbyMaps = slots ? MapsLobbyChoices(g_lobbyBlock) : std::vector<LobbyMap>();
	g_lobbyMapAt = 0;
	int picked = PackagesPicked();
	for (int i = 0; i < (int)g_lobbyMaps.size(); i++) {
		const LobbyMap& m = g_lobbyMaps[i];
		if (m.map == g_lobbyBlock[0x1b] && (m.package < 0 || m.package == picked)) {
			g_lobbyMapAt = i;
			break;
		}
	}
	g_lobbyStockChanged = false;
	g_lobbyPanelOpen = true;
	LobbyPanelSnapshot();
	XLS_LOG_INFO("mods: lobby settings: opened (%d maps to pick from).", (int)g_lobbyMaps.size());
}

// Close: the stock rules into the lobby's block, the map picked, everything saved to current.json and kept for Create Match.
void LobbyPanelClose()
{
	g_lobbyPanelOpen = false;
	uint8_t* slots = LobbySlots();
	if (g_lobbyChanged && slots) {
		for (int i = 0; i < kRows; i++) {
			g_lobbyBlock[kRowDefs[i].offset] = (uint8_t)g_menu[i];
		}
		if (g_lobbyStockChanged) {
			const LobbyMap* map = g_lobbyMaps.empty() ? nullptr : &g_lobbyMaps[g_lobbyMapAt];
			if (map && map->package >= 0 && map->package != PackagesPicked()) {
				PackagesPickIndex(map->package);
				XLS_LOG_INFO("mods: lobby settings: map package \"%s\" picked.", map->name.c_str());
			}
			LobbyBlockDerive(g_lobbyBlock);
		}
		memcpy(slots + kSetupBlock, g_lobbyBlock, kBlockSize);
		memcpy(g_lobbyCarryBlock, g_lobbyBlock, kBlockSize);
		g_lobbyCarry = true;
		g_setupEdited = true;
		GameRulesSaveCurrent(g_lobbyBlock, g_setupProfile, true);
		XLS_LOG_INFO("mods: lobby settings: rules changed (%s), saved to current.json.", Hex(g_lobbyBlock, kBlockSize).c_str());
	}
	g_lobbyChanged = g_lobbyStockChanged = false;
	XLS_LOG_INFO("mods: lobby settings: closed.");
}

// A client: the lobby owner's rule block into ours, while in the lobby.
void LobbyBlockFollow(uint8_t* slots)
{
	std::string want;
	{
		std::lock_guard<std::mutex> lock(g_hostBlockMutex);
		CSteamID lobby = SessionPresenceLobby();
		uint64_t host = lobby.IsValid() && SteamReady() && SteamMatchmaking() ? LobbyOwner(SteamMatchmaking(), lobby).ConvertToUint64() : 0;
		if (!host || host != g_hostBlockFrom) {
			return;
		}
		want = g_hostBlock;
	}
	uint8_t block[kBlockSize];
	if (want == g_hostBlockApplied || !Unhex(want, block, kBlockSize)) {
		return;
	}
	g_hostBlockApplied = want;
	if (memcmp(slots + kSetupBlock, block, kBlockSize) != 0) {
		memcpy(slots + kSetupBlock, block, kBlockSize);
		XLS_LOG_INFO("mods: lobby settings: the host's rules (%s) taken.", want.c_str());
	}
}

// From the lobby's input, each tick. True while the panel is open (the lobby then gets no input).
bool LobbyPanelTick(uint8_t* screen)
{
	g_lobbyScreen = screen;
	g_lobbySeenAt = GetTickCount();
	bool host = g_isHost && g_lobbySession && *g_lobbySession && g_isHost(*g_lobbySession) == 1;
	g_lobbyHost = host;
	uint8_t* slots = LobbySlots();
	if (slots && !host) {
		LobbyBlockFollow(slots);
	}
	if (slots && host) {
		std::string now = Hex(slots + kSetupBlock, kBlockSize);
		if (now != g_lobbyPublished) {
			g_lobbyPublished = now;
			MemberDataSet("srules", now);
		}
	}
	if (!host || !g_input || !*g_input) {
		if (g_lobbyPanelOpen) {
			LobbyPanelClose();
		}
		return false;
	}
	uint32_t pressed = *(uint32_t*)(*g_input + kInputPressed);
	uint32_t buttons = *(uint32_t*)(*g_input + kInputButtons);
	HWND front = GetForegroundWindow();
	DWORD process = 0;
	GetWindowThreadProcessId(front, &process);
	bool key = process == GetCurrentProcessId() && (GetAsyncKeyState(VK_F1) & 0x8000);
	bool toggle = key && !g_lobbyKeyWas;
	g_lobbyKeyWas = key;
	if (!g_lobbyPanelOpen) {
		if (toggle) {
			LobbyPanelOpen();
			return true;
		}
		return false;
	}
	if (toggle || (buttons & kButtonBack)) {
		LobbyPanelClose();
		return true;
	}
	int count = 0;
	LobbyLine row{};
	{
		std::lock_guard<std::mutex> lock(g_lobbyPanelMutex);
		count = (int)g_lobbyLines.size();
		if (g_lobbyCursor < count) {
			row = g_lobbyLines[g_lobbyCursor];
		}
	}
	if (count && (pressed & kKeyStepUp)) {
		g_lobbyCursor = (g_lobbyCursor + count - 1) % count;
		MenuSound(g_lobbyScreen, kSoundMove);
	}
	else if (count && (pressed & kKeyStepDown)) {
		g_lobbyCursor = (g_lobbyCursor + 1) % count;
		MenuSound(g_lobbyScreen, kSoundMove);
	}
	else if (count && (pressed & (kKeyLeft | kKeyRight))) {
		LobbyPanelStep(row, pressed & kKeyRight ? 1 : -1);
	}
	LobbyPanelSnapshot();
	return true;
}

void LobbyPanelDraw(float width, float height)
{
	if (!g_lobbyHost || GetTickCount() - g_lobbySeenAt > 500) {
		return;
	}
	ImFont* font = UiFont(false);
	ImFont* big = UiFont(true);
	if (!font || !big) {
		return;
	}
	float scale = height / 720.0f;
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	const ImU32 back = IM_COL32(8, 20, 32, 215), edge = IM_COL32(120, 200, 230, 140), text = IM_COL32(235, 248, 255, 255),
	            dim = IM_COL32(150, 180, 200, 255), head = IM_COL32(120, 200, 230, 255), pick = IM_COL32(40, 110, 160, 200);
	if (!g_lobbyPanelOpen) {
		const char* hint = "F1  Match Settings";
		ImVec2 t = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, hint);
		float pad = 8 * scale, x = width - t.x - pad * 2 - 16 * scale, y = 16 * scale;
		dl->AddRectFilled(ImVec2(x, y), ImVec2(x + t.x + pad * 2, y + t.y + pad), back, 5 * scale);
		dl->AddText(font, font->FontSize, ImVec2(x + pad, y + pad / 2), text, hint);
		return;
	}
	std::vector<LobbyLine> lines;
	int cursor = 0;
	{
		std::lock_guard<std::mutex> lock(g_lobbyPanelMutex);
		lines = g_lobbyLines;
		cursor = g_lobbyCursorShown;
	}
	if (lines.empty()) {
		return;
	}
	// rows and their section headers as one list, a window of it around the cursor
	struct Item {
		bool header;
		int row;
	};
	std::vector<Item> items;
	int cursorItem = 0;
	for (int i = 0; i < (int)lines.size(); i++) {
		if (i == 0 || lines[i].section != lines[i - 1].section) {
			items.push_back({true, i});
		}
		if (i == cursor) {
			cursorItem = (int)items.size();
		}
		items.push_back({false, i});
	}
	float line = font->FontSize * 1.35f, pad = 14 * scale, w = 520 * scale;
	int visible = std::max(6, (int)((height * 0.7f - big->FontSize - line * 2) / line));
	int first = std::clamp(cursorItem - visible / 2, 0, std::max(0, (int)items.size() - visible));
	int shown = std::min(visible, (int)items.size() - first);
	float h = pad * 2 + big->FontSize + line * 0.5f + shown * line + line * 1.5f;
	float x = (width - w) / 2, y = (height - h) / 2;
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), back, 8 * scale);
	dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), edge, 8 * scale, 0, 1.5f * scale);
	dl->AddText(big, big->FontSize, ImVec2(x + pad, y + pad), text, "Match Settings");
	float cy = y + pad + big->FontSize + line * 0.5f;
	for (int k = first; k < first + shown; k++, cy += line) {
		const Item& it = items[k];
		const LobbyLine& l = lines[it.row];
		if (it.header) {
			dl->AddText(font, font->FontSize, ImVec2(x + pad, cy + (line - font->FontSize) / 2), head, l.section.c_str());
			continue;
		}
		bool on = it.row == cursor;
		if (on) {
			dl->AddRectFilled(ImVec2(x + pad / 2, cy), ImVec2(x + w - pad / 2, cy + line), pick, 4 * scale);
		}
		float ty = cy + (line - font->FontSize) / 2;
		dl->AddText(font, font->FontSize, ImVec2(x + pad + 16 * scale, ty), on ? text : dim, l.label.c_str());
		std::string value = on ? "<  " + l.value + "  >" : l.value;
		ImVec2 v = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, value.c_str());
		dl->AddText(font, font->FontSize, ImVec2(x + w - pad - v.x, ty), on ? text : dim, value.c_str());
	}
	const char* help = "Up/Down select   Left/Right change   Back / F1 close";
	ImVec2 hs = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, help);
	dl->AddText(font, font->FontSize, ImVec2(x + (w - hs.x) / 2, y + h - pad - font->FontSize), dim, help);
}

// Bots in player matches (Public and System Link; Ranked has its own lobby screen, uIdBattleRankLobby). Campaign co-op already
// does this: its host marks free slots AI (FUN_00828ca0), sends the mask (message 7, FUN_01011df0) and every client marks the
// same slots (FUN_01012710 case 7). Ours does the same just before the host's match start (FUN_00e97130).
// The lobby wants more than one player for Start Game: its input and prompt (FUN_00e969f0, FUN_00e95470) compare the member
// count at screen +0x5b0, recounted each tick, so with bots on they see one more than they need while they run.
using LobbyFn = void(__attribute__((thiscall)) *)(uint8_t*, int);
using LobbyTickFn = void(__attribute__((thiscall)) *)(uint8_t*);
using SlotAiFn = void(__attribute__((thiscall)) *)(uint8_t*, uint32_t);
using NetMakeFn = uint8_t*(__cdecl*)(uint32_t);
using NetSendFn = void(__cdecl*)(uint8_t*);
using NetFillFn = void(__attribute__((thiscall)) *)(uint8_t*, uint32_t);
LobbyFn g_lobbyInputOriginal = nullptr, g_lobbyPromptOriginal = nullptr;
LobbyTickFn g_lobbyStartOriginal = nullptr, g_lobbyLeaveOriginal = nullptr;
SlotAiFn g_slotAi = nullptr;
SlotAiFn g_slotAiOriginal = nullptr;
using FactionPartFn = uint32_t (*)(int, int, int, int, int);
FactionPartFn g_factionPart = nullptr;

// In versus the game dresses an AI slot in the local player's set (DX11 FUN_00828ca0 -> FUN_0081f600), so bots wear yours, and on
// a client they'd wear that client's. Its campaign table (FUN_00f88a30: faction, 1, member, part, 0) has whole outfits: each bot
// gets one by a hash of the slot and lobby, the same on every machine. Codes are at slot record +0xc + part * 4.
void __attribute__((thiscall)) SlotAiLook(uint8_t* slots, uint32_t slot)
{
	g_slotAiOriginal(slots, slot);
	uint32_t s = slot & 0xff;
	if (!g_factionPart || !g_game || !*g_game || !(*g_game)[0x256] || !(*g_game)[599] || s >= 16) {
		return;
	}
	CSteamID lobby = SessionPresenceLobby();
	uint32_t h = (s + 1) * 2654435761u ^ (lobby.IsValid() ? (uint32_t)lobby.ConvertToUint64() : 0);
	h ^= h >> 13;
	h *= 0x5bd1e995;
	h ^= h >> 15;
	int faction = 1 + (int)(h % 6), member = (int)((h >> 8) & 3);
	auto* codes = (uint32_t*)(slots + 0x148 + s * 0x7c + 0xc);
	for (int part = 0; part < 4; part++) {
		codes[part] = g_factionPart(faction, 1, member, part, 0);
	}
	XLS_LOG_INFO("mods: bots: slot %u wears faction %d, member %d (%08x %08x %08x %08x).", s, faction, member, codes[0], codes[1], codes[2], codes[3]);
}
NetMakeFn g_netMake = nullptr;
NetSendFn g_netSend = nullptr;
NetFillFn g_netFill = nullptr;
uint8_t** g_lobbySlots = nullptr;
const uint32_t kLobbyMembers = 0x5b0, kLobbyPhase = 0x1a, kLobbyStep = 0x1b, kSlotsMax = 0x70, kSlotsTeams = 0x87, kSlotOccupied = 0x1ac, kSlotAi = 0x1b1, kLobbyLeaveState = 0x19;
const uint32_t kNetMessage = 0x42000002;
const int kNetAiMask = 7;

bool LobbyWantsBots()
{
	return g_matchBots && g_isHost && g_lobbySession && *g_lobbySession && g_isHost(*g_lobbySession) == 1;
}

void __attribute__((thiscall)) LobbyInput(uint8_t* screen, int mode);
void __attribute__((thiscall)) LobbyPrompt(uint8_t* screen, int mode);

void LobbyBoosted(uint8_t* screen, int mode, LobbyFn original)
{
	int32_t& members = *(int32_t*)(screen + kLobbyMembers);
	uint8_t* slots = g_lobbySlots ? *g_lobbySlots : nullptr;
	int need = slots && slots[kSlotsTeams] ? 3 : 2;
	if (!slots || members >= need || !LobbyWantsBots()) {
		original(screen, mode);
		return;
	}
	int32_t held = members;
	members = need;
	original(screen, mode);
	members = held;
}

void __attribute__((thiscall)) LobbyInput(uint8_t* screen, int mode)
{
	if (LobbyPanelTick(screen) && g_input && *g_input) {
		// the panel has the input: the lobby runs on with none
		uint32_t& pressed = *(uint32_t*)(*g_input + kInputPressed);
		uint32_t& buttons = *(uint32_t*)(*g_input + kInputButtons);
		uint32_t heldPressed = pressed, heldButtons = buttons;
		pressed = 0;
		buttons = 0;
		LobbyBoosted(screen, mode, g_lobbyInputOriginal);
		pressed = heldPressed;
		buttons = heldButtons;
		return;
	}
	LobbyBoosted(screen, mode, g_lobbyInputOriginal);
}

void __attribute__((thiscall)) LobbyPrompt(uint8_t* screen, int mode)
{
	if (g_lobbyPanelOpen && g_input && *g_input) {
		uint32_t& pressed = *(uint32_t*)(*g_input + kInputPressed);
		uint32_t& buttons = *(uint32_t*)(*g_input + kInputButtons);
		uint32_t heldPressed = pressed, heldButtons = buttons;
		pressed = 0;
		buttons = 0;
		LobbyBoosted(screen, mode, g_lobbyPromptOriginal);
		pressed = heldPressed;
		buttons = heldButtons;
		return;
	}
	LobbyBoosted(screen, mode, g_lobbyPromptOriginal);
}

// The free slots up to Players Desired become bots, on every machine. FUN_00828ca0 leaves a slot's occupied flag clear and sets
// its AI flag, so the two counts stay apart. Starting again from the same lobby finds its bots still there.
void BotsFill(uint8_t* slots, int desired)
{
	int max = slots[kSlotsMax], humans = 0, ai = 0, added = 0;
	if (desired < 2 || desired > max) {
		desired = max;
	}
	for (int i = 0; i < max; i++) {
		humans += slots[kSlotOccupied + i * 0x7c] == 1 ? 1 : 0;
		ai += slots[kSlotAi + i * 0x7c] ? 1 : 0;
	}
	for (int i = 0; i < max && humans + ai + added < desired; i++) {
		if (slots[kSlotOccupied + i * 0x7c] != 1 && !slots[kSlotAi + i * 0x7c]) {
			g_slotAi(slots, (uint32_t)i);
			added++;
		}
	}
	ai += added;
	XLS_LOG_INFO("mods: bots: host start: %d humans, %d bots (%d new), want %d of %d slots.", humans, ai, added, desired, max);
	if (!ai) {
		return;
	}
	g_botsInMatch = true;
	if (uint8_t* message = g_netMake(kNetMessage)) {
		memset(message + 0x1b4, 0, 0x10);
		memset(message + 0x1d0, 0, 0x20);
		g_netFill(message, kNetAiMask);
		g_netSend(message);
	}
}

void __attribute__((thiscall)) LobbyStart(uint8_t* screen)
{
	uint8_t* slots = g_lobbySlots ? *g_lobbySlots : nullptr;
	if (slots && screen[kLobbyPhase] == 0 && screen[kLobbyStep] == 0 && LobbyWantsBots()) {
		BotsFill(slots, g_matchDesired);
	}
	g_lobbyStartOriginal(screen);
}

// Leaving the lobby (state 0 of FUN_00e98f00) the game sets the player count (game +0x254) from the members alone. A match with
// AI counts them too, as campaign does (FUN_0089e840: humans + game +0x50188). Message 7 marked the same slots on clients.
void __attribute__((thiscall)) LobbyLeave(uint8_t* screen)
{
	bool entering = screen[kLobbyLeaveState] == 0;
	g_lobbyLeaveOriginal(screen);
	uint8_t* slots = g_lobbySlots ? *g_lobbySlots : nullptr;
	if (!entering || !slots || !g_game || !*g_game) {
		return;
	}
	int ai = 0, humans = 0;
	for (int i = 0; i < 16; i++) {
		ai += slots[kSlotAi + i * 0x7c] ? 1 : 0;
		humans += slots[kSlotOccupied + i * 0x7c] == 1 ? 1 : 0;
	}
	if (ai) {
		(*g_game)[0x254] = (uint8_t)(humans + ai);
		*(int32_t*)(*g_game + 0x50188) = ai;
		XLS_LOG_INFO("mods: bots: match starting with %d humans and %d bots.", humans, ai);
	}
}

// An AI slot's character (FUN_007e3320, read by the slot setup and the player create): campaign partners uHm60 on, one per slot
// number, unless game +0x8174 is 0 or 0xd, which gets uHm27, the AI Enemy Battle's bot. Versus sets +0x8174 to the game kind, so
// a player match asked for the partners. Bots get the bot there too.
// Its callers count on it leaving every register but eax alone (FUN_007f2ba0 keeps "this" in edx across the call), so this is a thunk.
extern "C" void* g_botClassOriginal = nullptr;

extern "C" int BotClassVersus()
{
	return g_game && *g_game && (*g_game)[0x256] != 0;
}

extern "C" void __attribute__((naked)) BotClass()
{
	asm("push %ecx\n"
	    "push %edx\n"
	    "call _BotClassVersus\n"
	    "pop %edx\n"
	    "pop %ecx\n"
	    "test %eax, %eax\n"
	    "jz 1f\n"
	    "mov $0x27, %eax\n"
	    "ret $4\n"
	    "1:\n"
	    "jmp *_g_botClassOriginal\n");
}

void BotsLobbyInstall(uint8_t* base, int build)
{
	static const uint32_t kBotClass[kBuildCount] = {0x7e3320, 0x7d4460, 0x7e3440, 0x7d45c0};
	static const uint8_t kBotClassPrologue[] = {0x0f, 0xb6, 0x44, 0x24, 0x04};
	g_botClassOriginal = Detour(base + (kBotClass[build] - 0x400000), kBotClassPrologue, sizeof(kBotClassPrologue), (void*)&BotClass);
	if (!g_botClassOriginal) {
		XLS_LOG_WARN("mods: bots: character type code not recognised.");
	}
	static const uint32_t kInput[kBuildCount] = {0xe969f0, 0xe86920, 0xe96e40, 0xe86e00};
	static const uint32_t kPrompt[kBuildCount] = {0xe95470, 0xe853a0, 0xe958c0, 0xe85880};
	static const uint32_t kStart[kBuildCount] = {0xe97130, 0xe87060, 0xe97580, 0xe87540};
	static const uint32_t kLeave[kBuildCount] = {0xe98f00, 0xe88e30, 0xe991b0, 0xe89310};
	static const uint32_t kSlotAiVa[kBuildCount] = {0x828ca0, 0x819ec0, 0x828ec0, 0x819f60};
	static const uint32_t kMake[kBuildCount] = {0x1002d70, 0xff29d0, 0x10030a0, 0xff2ce0};
	static const uint32_t kFill[kBuildCount] = {0x1011df0, 0x1001a30, 0x10120f0, 0x1001d40};
	static const uint32_t kSend[kBuildCount] = {0x1002d50, 0xff29a0, 0x1003080, 0xff2cc0};
	static const uint32_t kHost[kBuildCount] = {0x802d10, 0x7f3e60, 0x802e30, 0x7f3fb0};
	static const uint32_t kSession[kBuildCount] = {0x1852144, 0x18420a4, 0x1853144, 0x18420a4};
	static const uint32_t kSlotsAt[kBuildCount] = {0x1852850, 0x18427b0, 0x1853850, 0x18427b0};
	static const uint8_t kInputPrologue[] = {0x55, 0x8b, 0x6c, 0x24, 0x08, 0x56, 0x8b, 0xf1};
	static const uint8_t kPromptPrologue[] = {0x83, 0xec, 0x64, 0x0f, 0x57, 0xc0, 0x53, 0x55, 0x56};
	static const uint8_t kStartPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
	static const uint8_t kLeavePrologue[] = {0x56, 0x8b, 0xf1, 0x0f, 0xb6, 0x46, 0x19};
	g_slotAi = (SlotAiFn)(base + (kSlotAiVa[build] - 0x400000));
	static const uint32_t kFactionPart[kBuildCount] = {0xf88a30, 0xf78a90, 0xf88eb0, 0xf78dd0};
	static const uint8_t kSlotAiPrologue[] = {0x81, 0xec, 0x84, 0x00, 0x00, 0x00};
	g_factionPart = (FactionPartFn)(base + (kFactionPart[build] - 0x400000));
	g_slotAiOriginal = (SlotAiFn)Detour((uint8_t*)g_slotAi, kSlotAiPrologue, sizeof(kSlotAiPrologue), (void*)&SlotAiLook);
	if (!g_slotAiOriginal) {
		XLS_LOG_WARN("mods: bots: AI slot setup not recognised, bots wear the host's set.");
		g_factionPart = nullptr;
		g_slotAiOriginal = g_slotAi;
	}
	g_netMake = (NetMakeFn)(base + (kMake[build] - 0x400000));
	g_netFill = (NetFillFn)(base + (kFill[build] - 0x400000));
	g_netSend = (NetSendFn)(base + (kSend[build] - 0x400000));
	g_isHost = (IsHostFn)(base + (kHost[build] - 0x400000));
	g_lobbySession = (uint8_t**)(base + (kSession[build] - 0x400000));
	g_lobbySlots = (uint8_t**)(base + (kSlotsAt[build] - 0x400000));
	g_ruleSlots = g_lobbySlots;
	g_lobbyInputOriginal = (LobbyFn)Detour(base + (kInput[build] - 0x400000), kInputPrologue, sizeof(kInputPrologue), (void*)&LobbyInput);
	g_lobbyPromptOriginal = (LobbyFn)Detour(base + (kPrompt[build] - 0x400000), kPromptPrologue, sizeof(kPromptPrologue), (void*)&LobbyPrompt);
	g_lobbyStartOriginal = (LobbyTickFn)Detour(base + (kStart[build] - 0x400000), kStartPrologue, sizeof(kStartPrologue), (void*)&LobbyStart);
	g_lobbyLeaveOriginal = (LobbyTickFn)Detour(base + (kLeave[build] - 0x400000), kLeavePrologue, sizeof(kLeavePrologue), (void*)&LobbyLeave);
	if (g_lobbyInputOriginal && g_input) {
		UiAddLayer("lobby settings", LobbyPanelDraw);
	}
	if (!g_lobbyInputOriginal || !g_lobbyPromptOriginal || !g_lobbyStartOriginal || !g_lobbyLeaveOriginal) {
		XLS_LOG_WARN("mods: bots: lobby code not recognised: input %d, prompt %d, start %d, leave %d.", g_lobbyInputOriginal != nullptr,
		             g_lobbyPromptOriginal != nullptr, g_lobbyStartOriginal != nullptr, g_lobbyLeaveOriginal != nullptr);
	}
}

using PushRulesFn = void(__attribute__((thiscall)) *)(void*, uint8_t*);
using SetNumberFn = void(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t);
PushRulesFn g_pushRulesOriginal = nullptr;

// The bytes the game derives from the rules (category, the random flags, map size): its own push works them out
// (and sets the create screen's Flash numbers, which nothing shows outside that screen).
void LobbyBlockDerive(uint8_t* block)
{
	if (g_pushRulesOriginal) {
		g_pushRulesOriginal(nullptr, block);
	}
}
SetNumberFn g_setNumber = nullptr;

const uint8_t kPushRulesPrologue[] = {0x83, 0xec, 0x20, 0x53, 0x55, 0x56}; // sub esp,0x20; push ebx; push ebp; push esi

void __attribute__((thiscall)) PushRules(void* screen, uint8_t* rules)
{
	g_pushRulesOriginal(screen, rules);
	if (!rules) {
		return;
	}
	if (g_lobbyCarry) { // the rules last set in the lobby's Match Settings: the next create screen starts from them
		g_lobbyCarry = false;
		memcpy(rules, g_lobbyCarryBlock, kBlockSize);
	}
	if (rules[kRuleOldGrenades] == 1) { // saved with the separate Unlimited Grenades row
		rules[kRowDefs[kRowInfinite].offset] = (rules[kRowDefs[kRowInfinite].offset] & kInfiniteAmmo) | kInfiniteGrenades;
	}
	rules[kRuleOldGrenades] = 0;
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
		rules[kRuleRespawn] = kRespawnOn;
	}
	g_menuLives = kLives[rules[kRuleLives]];
	g_setNumber(screen, kDataNoKillShown, SurvivalOn(rules) ? kKillTargets + rules[kRuleLives] : rules[kRuleKillTarget]);
	int sub = rules[kRuleSub];
	g_setNumber(screen, kDataNoSubShown, SurvivalOn(rules) ? 2 : sub + (sub >= kSubPoints ? 1 : 0));
	g_menuSeen = true;
}

bool PatchRowTable(uint8_t* table)
{
	g_rowTable = table;
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
	memcpy(g_stockRows, table, sizeof(g_stockRows));
	VirtualProtect(table, 16 * 0x23, old, &old);
	XLS_LOG_INFO("mods: rules: %d create-match categories got the rule rows.", patched);
	return patched > 0;
}

using SetupFn = void(__attribute__((thiscall)) *)(uint8_t*, uint32_t);
SetupFn g_setupOriginal = nullptr;
uintptr_t g_transfer = 0;
const uint8_t kSetupPrologue[] = {0x56, 0x8b, 0xf1, 0x8b, 0x0d}; // push esi; mov esi,ecx; mov ecx,[game] (9 bytes copied)
const size_t kSetupCopy = 9;
const int kDataEntries = 3;
const size_t kDataEntry = 0x54;
std::mutex g_supplyMutex;
std::map<uint8_t*, int16_t> g_startOriginal; // data entry -> its original start total
std::map<uint8_t*, std::pair<int, int>> g_pickupScale; // weapon -> new / original start total

// Grenade counts. A grenade's data entries hold its start total (+0x30), magazine (+0x32) and cap (+0x34). The refill at a
// respawn (DX11 FUN_00c3fa20) reads the start; a pickup is refused at the cap (uWp1b::vf124) and clamped to it. So the entries
// keep the spawn count and the 10 cap while either rule is on, and a weapon set up outside a spawn (a grenade of another kind
// picked up) starts with the pickup count just for that one setup. The originals come back when the rules are off.
struct GrenadeEntry {
	int16_t start, magazine, cap;
};
std::map<uint8_t*, GrenadeEntry> g_grenadeOriginal;
const int16_t kGrenadeMost = 10;

void GrenadeSetup(uint8_t* weapon, uint32_t arg)
{
	int count = g_grenadeCount, pickup = g_grenadePickup;
	uint8_t* data = *(uint8_t**)(weapon + g_dataAt);
	if (!data || (!count && !pickup && g_grenadeOriginal.empty())) {
		g_setupOriginal(weapon, arg);
		return;
	}
	std::lock_guard<std::mutex> lock(g_supplyMutex);
	bool spawn = g_inSpawn != 0, on = count || pickup;
	int16_t want[kDataEntries], keep[kDataEntries];
	for (int k = 0; k < kDataEntries; k++) {
		uint8_t* e = data + k * kDataEntry;
		auto* fields = (int16_t*)(e + 0x30);
		auto it = g_grenadeOriginal.find(e);
		if (it == g_grenadeOriginal.end()) {
			it = g_grenadeOriginal.emplace(e, GrenadeEntry{fields[0], fields[1], fields[2]}).first;
		}
		const GrenadeEntry& o = it->second;
		int cost = GrenadeCost(e);
		keep[k] = count ? (int16_t)((count - 1) * cost) : o.start;
		want[k] = !spawn && pickup ? (int16_t)(pickup * cost) : !spawn ? o.start : keep[k];
		fields[0] = want[k];
		fields[1] = on ? (int16_t)(kGrenadeMost * cost) : o.magazine;
		fields[2] = on ? (int16_t)(kGrenadeMost * cost) : o.cap;
	}
	g_setupOriginal(weapon, arg);
	int16_t total = *(int16_t*)(weapon + g_magazine + 2);
	const uint8_t* entry = WeaponEntry(weapon);
	int took = entry >= data && entry < data + kDataEntries * kDataEntry ? (int)((entry - data) / kDataEntry) : 0;
	static int logged = 0;
	if (on && logged < 24) {
		logged++;
		const GrenadeEntry& o = g_grenadeOriginal[data + took * kDataEntry];
		XLS_LOG_INFO("mods: rules: grenade setup: kind 0x%02x, spawn %d, entry %d, cost %d, stock start %d magazine %d cap %d, want %d, took %d.", weapon[0x1e], spawn, took, GrenadeCost(data + took * kDataEntry), o.start, o.magazine, o.cap, want[took], total);
	}
	// Only a spawn's weapon is forced. The game sets a weapon up again after its spawn (on another thread, so not "in a spawn"),
	// which leaves its total alone; a new weapon outside a spawn already took want[] from its entry.
	if (on && spawn && count != 0) {
		auto* m = (int16_t*)(weapon + g_magazine);
		m[0] = m[1] = want[took];
	}
	for (int k = 0; k < kDataEntries; k++) {
		*(int16_t*)(data + k * kDataEntry + 0x30) = keep[k];
	}
}

void __attribute__((thiscall)) Setup(uint8_t* weapon, uint32_t arg)
{
	if (!weapon) {
		g_setupOriginal(weapon, arg);
		return;
	}
	if (Grenade(weapon)) {
		GrenadeSetup(weapon, arg);
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
	if (delta > 0 && weapon && g_grenadePickup != 0 && Grenade(weapon) && (uintptr_t)__builtin_return_address(0) != g_transfer) {
		delta = (int16_t)(g_grenadePickup * GrenadeCost(WeaponEntry(weapon)));
	}
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

// Score log (while ModCache\scorelog.on exists): every change of a slot's Battle Gauge / kills / points,
// a team's gauge, and the three things that stop the game's death scoring (DX9 FUN_00f6b050: the
// victim loses gauge on every death, a killer that isn't a teammate gains), read every 20 ms:
//   game status +0x1467c bit 0x200000: the death isn't scored at all
//   players +0x256 == 0 (offline-style rules) with the victim's slot record +0x1b1 set: both amounts 0
// and what the match is scored by (players +0x8174: 1 gauge, 2 kills, 3 points).
uint8_t** g_status = nullptr;
const uint32_t kResultsPoints = 0x19bc, kResultsTeamGauge = 0x107c;
const uint32_t kStatusFlags = 0x1467c, kStatusNoScore = 0x200000, kPlayersOnline = 0x256, kSlotNoScore = 0x69;

DWORD WINAPI ScoreThread(LPVOID)
{
	struct Snap {
		int32_t gauge[16], kills[16], points[16], team[2], mode;
		uint32_t flags;
		uint8_t online, noScore[16];
	};
	Snap last{};
	bool have = false, on = false;
	DWORD checked = 0;
	for (;; Sleep(20)) {
		if (GetTickCount() - checked > 2000) {
			checked = GetTickCount();
			bool now = GetFileAttributesW((Cfg().modCacheDir + L"scorelog.on").c_str()) != INVALID_FILE_ATTRIBUTES;
			if (now != on) {
				on = now;
				have = false;
				XLS_LOG_INFO("mods: rules: score log %s.", on ? "on" : "off");
			}
		}
		if (!on) {
			continue;
		}
		uint8_t *results = nullptr, *status = nullptr, *players = nullptr, *setup = nullptr;
		ReadGame(g_results, &results);
		ReadGame(g_status, &status);
		ReadGame(g_players, &players);
		ReadGame(g_matchSetup, &setup);
		Snap s{};
		if (!results || !ReadGame(results + kResultsGauge, &s.gauge) || !ReadGame(results + kResultsKills, &s.kills) ||
		    !ReadGame(results + kResultsPoints, &s.points) || !ReadGame(results + kResultsTeamGauge, &s.team)) {
			have = false;
			continue;
		}
		ReadGame(status + kStatusFlags, &s.flags);
		ReadGame(players + kPlayersOnline, &s.online);
		ReadGame(players + kPlayersListMode, &s.mode);
		for (int i = 0; i < 16; i++) {
			ReadGame(setup + kSetupSlots + i * kSetupSlot + kSlotNoScore, &s.noScore[i]);
		}
		if (!have || s.mode != last.mode || s.online != last.online || (s.flags & kStatusNoScore) != (last.flags & kStatusNoScore)) {
			XLS_LOG_INFO("mods: rules: score: scored by %s (mode %d), players+0x256 = %u, no-score flag %s.",
			             s.mode == kListGauge ? "Battle Gauge" : s.mode == kListKills ? "kills" : s.mode == kListPoints ? "points" : "?",
			             s.mode, s.online, s.flags & kStatusNoScore ? "SET (deaths not scored)" : "clear");
		}
		for (int i = 0; i < 16; i++) {
			bool changed = !have || s.gauge[i] != last.gauge[i] || s.kills[i] != last.kills[i] || s.points[i] != last.points[i] ||
			               s.noScore[i] != last.noScore[i];
			bool used = s.gauge[i] > 0 || s.kills[i] > 0 || s.points[i] > 0 || (have && (last.gauge[i] > 0 || last.kills[i] > 0 || last.points[i] > 0));
			if (changed && (used || have)) {
				XLS_LOG_INFO("mods: rules: score: slot %d gauge %d -> %d, kills %d -> %d, points %d -> %d (slot +0x1b1 = %u).", i,
				             have ? last.gauge[i] : s.gauge[i], s.gauge[i], have ? last.kills[i] : s.kills[i], s.kills[i],
				             have ? last.points[i] : s.points[i], s.points[i], s.noScore[i]);
			}
		}
		for (int t = 0; t < 2; t++) {
			if (!have || s.team[t] != last.team[t]) {
				XLS_LOG_INFO("mods: rules: score: team %d gauge %d -> %d.", t, have ? last.team[t] : s.team[t], s.team[t]);
			}
		}
		last = s;
		have = true;
	}
}

}

void RulesInstall(unsigned char* base)
{
	LivesInstall();
	int build = -1;
	for (int b = 0; b < kBuildCount && build < 0; b++) {
		if (GameBuild() >= 0 && b != GameBuild()) {
			continue;
		}
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
	g_battleFormatOriginal = (BattleFormatFn)Detour(base + (kBattleFormat[build] - 0x400000), kBattleFormatPrologue, sizeof(kBattleFormatPrologue), (void*)&BattleFormat);
	if (!g_battleFormatOriginal) {
		XLS_LOG_WARN("mods: rules: battle message code not recognised, no Eliminated messages.");
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
	if (uint8_t* check = base + (kHarmonizerStart[build] - 0x400000) + 0x19; memcmp(check, kHarmonizerCheck, sizeof(kHarmonizerCheck)) == 0) {
		g_harmonizerJump = check + 7;
	}
	else {
		XLS_LOG_WARN("mods: rules: Harmonizer start check not found, the Harmonizer row has no effect.");
	}
	GrappleInstall(base, build);
	StaggerInstall(base, build);
	RadarInstall(base, build);
	ResupplyInstall(base, build);
	g_pushRulesOriginal = (PushRulesFn)Detour(base + (kRulesVas[build].pushRules - 0x400000), kPushRulesPrologue, sizeof(kPushRulesPrologue), (void*)&PushRules);
	if (g_pushRulesOriginal) {
		PatchRowTable(base + (kRulesVas[build].rowTable - 0x400000));
		g_createTickOriginal = (CreateTickFn)Detour(base + (kCreateTick[build] - 0x400000), kCreateTickPrologue, sizeof(kCreateTickPrologue), (void*)&CreateTick);
		g_createRefresh = (CreateRefreshFn)(base + (kCreateRefresh[build] - 0x400000));
		g_createHelp = (CreateTickFn)(base + (kCreateHelp[build] - 0x400000));
		g_createHelpOriginal = (CreateTickFn)Detour((uint8_t*)g_createHelp, kCreateHelpPrologue, sizeof(kCreateHelpPrologue), (void*)&CreateHelp);
		if (!g_createHelpOriginal) {
			XLS_LOG_WARN("mods: rules: create screen help line not guarded (code not recognised).");
		}
		g_createArrows = (CreateTickFn)(base + (kCreateArrows[build] - 0x400000));
		static const uint32_t kCreateCheck[kBuildCount] = {0xe8cb60, 0xe7cad0, 0xe9c950, 0xe8cab0};
		g_createCheck = (CreateCheckFn)(base + (kCreateCheck[build] - 0x400000));
		if (!g_createTickOriginal) {
			XLS_LOG_WARN("mods: rules: create-match screen code not recognised, the stock screen stays.");
		}
		BotsLobbyInstall(base, build);
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
	g_respawnSecond = base + (kRulesVas[build].respawnSecond - 0x400000);
	if (memcmp(g_respawnSecond, kRespawnSecondCode, sizeof(kRespawnSecondCode)) != 0) {
		g_respawnSecond = nullptr;
		XLS_LOG_WARN("mods: rules: respawn slot 7 code not as expected, no secondary after a respawn.");
	}
	uint8_t* spawn = base + (kRulesVas[build].spawn - 0x400000);
	g_game = *(uint8_t***)(spawn + kSpawnGameAt);
	g_spawnOriginal = (SpawnFn)Detour(spawn, kSpawnPrologue, sizeof(kSpawnPrologue), (void*)&Spawn);
	if (!g_addAmmoOriginal || !g_reserveOriginal || !g_counterOriginal || !g_spawnOriginal) {
		XLS_LOG_WARN("mods: rules: add-ammo hook failed.");
		return;
	}
	MemberDataListen("srules", [](uint64_t steamId, XUID, const std::string& value) {
		std::lock_guard<std::mutex> lock(g_hostBlockMutex);
		CSteamID lobby = SessionPresenceLobby();
		uint64_t host = lobby.IsValid() && SteamReady() && SteamMatchmaking() ? LobbyOwner(SteamMatchmaking(), lobby).ConvertToUint64() : 0;
		if (host && steamId == host) {
			g_hostBlock = value;
			g_hostBlockFrom = steamId;
		}
	});
	MemberDataListen("rules", [](uint64_t steamId, XUID, const std::string& value) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_memberRules[steamId] = Parse(value);
	});
	// After a fresh start: the rules of the last match created (ModCache\gamerules\current.json, saved by Create match),
	// so what we publish before the setup screen opens is that ruleset; edits left without creating a match don't count.
	uint8_t saved[0x38] = {};
	std::string savedProfile;
	bool savedEdited = false;
	if (GameRulesLoadCurrent(saved, &savedProfile, &savedEdited)) {
		for (int i = 0; i < kRows; i++) {
			uint8_t v = saved[kRowDefs[i].offset];
			g_menu[i] = v < kRowDefs[i].values ? v : 0;
		}
		g_menuSurvival = SurvivalOn(saved);
		g_menuTeam = saved[kRuleMain] == 1;
		g_menuLives = kLives[saved[kRuleLives] < kLivesValues ? saved[kRuleLives] : 0];
		BotRulesSync();
		g_menuSeen = true;
		XLS_LOG_INFO("mods: rules: last match's rules loaded (current.json): %s.", Format(Own()).c_str());
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
	g_status = (uint8_t**)(base + (kRulesVas[build].status - 0x400000));
	g_follow = (FollowFn)(base + (kRulesVas[build].follow - 0x400000));
	g_cameraSnap = (CameraSnapFn)(base + (kCameraSnap[build] - 0x400000));
	g_unitTeam = kUnitTeam[build];
	g_followArg = (const float*)(base + (kRulesVas[build].followArg - 0x400000));
	g_flashCall = (FlashCallFn)(base + (kRulesVas[build].flashCall - 0x400000));
	g_input = (uint8_t**)(base + (kRulesVas[build].input - 0x400000));
	g_results = (uint8_t**)(base + (kRulesVas[build].results - 0x400000));
	g_gaugeFormat = (const char*)(base + (kRulesVas[build].gaugeFormat - 0x400000));
	uint8_t* listCall = base + (kRulesVas[build].listValue - 0x400000);
	uint8_t* sprintfFn = base + (kRulesVas[build].sprintfFn - 0x400000);
	bool listed = memcmp(g_gaugeFormat, "%d", 3) == 0 && RedirectCall(listCall, sprintfFn, (void*)&ListValueThunk);
	listed = RedirectCall(base + (kRulesVas[build].listValue2 - 0x400000), sprintfFn, (void*)&ListValueThunk) && listed;
	uint8_t* rankOfCall = base + (kRulesVas[build].listRankOf - 0x400000);
	uint8_t* rankOf = rankOfCall + 5 + *(int32_t*)(rankOfCall + 1); // the accessors, as the list calls them
	uint8_t* slotAt = base + (kRulesVas[build].listSlotAt[0] - 0x400000);
	slotAt += 5 + *(int32_t*)(slotAt + 1);
	for (uint32_t va : kRulesVas[build].listSlotAt) {
		listed = RedirectCall(base + (va - 0x400000), slotAt, (void*)&ListSlotAtThunk) && listed;
	}
	listed = RedirectCall(rankOfCall, rankOf, (void*)&ListRankOfThunk) && listed;
	if (!listed) {
		XLS_LOG_WARN("mods: rules: battle HUD player list not (fully) patched, Survival shows the Battle Gauge order there.");
	}
	uint8_t* listDraw = base + (kRulesVas[build].listDraw - 0x400000);
	const uint8_t kListDrawPrologue[] = {0x81, 0xec, 0xa4, 0x01, 0x00, 0x00}; // sub esp,0x1a4
	if (listDraw[kListStyleAt - 1] == 0xbe && listDraw[kListFontAt - 2] == 0x66 && listDraw[kListCtxAt - 2] == 0x8b && listDraw[kListTextCall] == 0xe8) {
		g_textStyle = *(const uint32_t**)(listDraw + kListStyleAt);
		g_textFont = *(const uint16_t**)(listDraw + kListFontAt);
		g_textCtx = *(void***)(listDraw + kListCtxAt);
		g_text = (TextFn)(listDraw + kListTextCall + 5 + *(int32_t*)(listDraw + kListTextCall + 1));
		g_listDrawOriginal = (ListDrawFn)Detour(listDraw, kListDrawPrologue, sizeof(kListDrawPrologue), (void*)&ListDraw);
	}
	g_flashGet = (FlashGetFn)(base + (kRulesVas[build].flashGet - 0x400000));
	g_flashSet = (FlashGetFn)(base + (kRulesVas[build].flashSet - 0x400000));
	g_flashFree = (FlashFreeFn)(base + (kRulesVas[build].flashFree - 0x400000));
	const uint8_t kHudUpdatePrologue[] = {0x83, 0xec, 0x20, 0x53, 0x56}; // sub esp,0x20; push ebx; push esi
	g_hudUpdateOriginal = (ListDrawFn)Detour(base + (kRulesVas[build].hudUpdate - 0x400000), kHudUpdatePrologue, sizeof(kHudUpdatePrologue), (void*)&HudUpdate);
	if (!g_hudUpdateOriginal) {
		XLS_LOG_WARN("mods: rules: HUD update not found, Survival spectators get no HUD once their body is gone.");
	}
	const uint8_t kTeamBarPrologue[] = {0x83, 0xec, 0x24, 0x53, 0x56}; // sub esp,0x24; push ebx; push esi
	g_teamBarOriginal = (ListDrawFn)Detour(base + (kRulesVas[build].teamBar - 0x400000), kTeamBarPrologue, sizeof(kTeamBarPrologue), (void*)&TeamBar);
	if (!g_teamBarOriginal) {
		XLS_LOG_WARN("mods: rules: team gauge bar update not found, Team Survival shows it.");
	}
	const uint8_t kKillSetPrologue[] = {0x83, 0xec, 0x4c, 0x8b, 0xd1, 0x8b, 0x42, 0x58};       // sub esp,0x4c; mov edx,ecx; mov eax,[edx+0x58]
	const uint8_t kCockpitUpdatePrologue[] = {0x83, 0xec, 0x14, 0x56, 0x8b, 0xf1}; // sub esp,0x14; push esi; mov esi,ecx
	g_killRedOriginal = (ListDrawFn)Detour(base + (kCounterVas[build].red - 0x400000), kKillSetPrologue, sizeof(kKillSetPrologue), (void*)&KillRed);
	g_killBlueOriginal = (ListDrawFn)Detour(base + (kCounterVas[build].blue - 0x400000), kKillSetPrologue, sizeof(kKillSetPrologue), (void*)&KillBlue);
	g_cockpitUpdateOriginal = (ListDrawFn)Detour(base + (kCounterVas[build].update - 0x400000), kCockpitUpdatePrologue, sizeof(kCockpitUpdatePrologue), (void*)&CockpitUpdate);
	if (!g_killRedOriginal || !g_killBlueOriginal || !g_cockpitUpdateOriginal) {
		XLS_LOG_WARN("mods: rules: team kill counters not hooked (red %d, blue %d, update %d), Team Survival shows its respawns as text rows.",
		             g_killRedOriginal != nullptr, g_killBlueOriginal != nullptr, g_cockpitUpdateOriginal != nullptr);
	}
	if (!g_listDrawOriginal) {
		g_text = nullptr;
		XLS_LOG_WARN("mods: rules: battle HUD list draw not found, Team Survival shows no team rows.");
	}
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
	{
		// FUN_01007110 (round end): push ecx; mov ecx,[global]. FUN_01006fe0 (round time): mov eax,[global]. FUN_010068e0: push esi;
		// mov esi,ecx; cmp byte [esi+0x1c],0
		const uint32_t kRoundEnd[kBuildCount] = {0x1007110, 0xff6d30, 0x1007400, 0xff7040};
		const uint32_t kRoundTime[kBuildCount] = {0x1006fe0, 0xff6c00, 0x10072d0, 0xff6f10};
		const uint32_t kNextRound[kBuildCount] = {0x10068e0, 0xff6500, 0x1006bd0, 0xff6810};
		const uint32_t kRoundGlobal[kBuildCount] = {0x1852144, 0x18420a4, 0x1853144, 0x18420a4};
		uint8_t endPro[7] = {0x51, 0x8b, 0x0d}, timePro[5] = {0xa1};
		memcpy(endPro + 3, &kRoundGlobal[build], 4);
		memcpy(timePro + 1, &kRoundGlobal[build], 4);
		const uint8_t kNextRoundPrologue[] = {0x56, 0x8b, 0xf1, 0x80, 0x7e, 0x1c, 0x00};
		g_roundEndOriginal = (VoidFn)Detour(base + (kRoundEnd[build] - 0x400000), endPro, sizeof(endPro), (void*)&RoundEnd);
		g_roundTimeOriginal = g_roundEndOriginal ? (VoidFn)Detour(base + (kRoundTime[build] - 0x400000), timePro, sizeof(timePro), (void*)&RoundTime) : nullptr;
		g_nextRoundOriginal = g_roundTimeOriginal ? (SlotsFn)Detour(base + (kNextRound[build] - 0x400000), kNextRoundPrologue, sizeof(kNextRoundPrologue), (void*)&NextRound) : nullptr;
		if (!g_nextRoundOriginal) {
			XLS_LOG_WARN("mods: rules: rounds not hooked, the Rounds rows have no effect.");
		}
		else {
			UiAddLayer("round counter", RoundCounter);
		}
	}
	g_ctlUpdateOriginal = (ControlUpdateFn)Detour(base + (kRulesVas[build].ctlUpdate - 0x400000), kCtlUpdatePrologue, sizeof(kCtlUpdatePrologue), (void*)&ControlUpdate);
	if (!g_ctlUpdateOriginal) {
		XLS_LOG_WARN("mods: rules: controller update not hooked, survival can't hold respawns.");
	}
	HANDLE survival = CreateThread(nullptr, 0, SurvivalThread, nullptr, 0, nullptr);
	if (survival) {
		CloseHandle(survival);
	}
	if (HANDLE score = CreateThread(nullptr, 0, ScoreThread, nullptr, 0, nullptr)) {
		CloseHandle(score);
	}
	XLS_LOG_INFO("mods: rules: hooked (%s).", BuildName(build));
}


int RulesStep(int rule, int key, uint8_t* value, const std::function<int()>& original)
{
	if (rule == kRuleSub && (key == 10 || key == 11) && value[kRuleMain - kRuleSub] <= 1) {
		uint8_t& survival = value[kRuleSurvival - kRuleSub];
		bool on = *value == kSubKills && survival == 1;
		// Survival is a game mode now (Modes\Survival.lp2mode), not a sub-type: the steps pass it by
		(void)on;
		survival = 0;
		int changed = original() ? 2 : 0; // 2: the game's step (it made the sound)
		bool was = g_menuSurvival;
		g_menuSurvival = SurvivalOn(value - kRuleSub);
		g_menuTeam = value[kRuleMain - kRuleSub] == 1;
		// Survival forces the kill target (infinite) and respawns (on); leaving it puts back what was there
		static uint8_t savedTarget = 0, savedRespawn = 0;
		static bool saved = false;
		if (g_menuSurvival && !was) {
			savedTarget = value[kRuleKillTarget - kRuleSub];
			savedRespawn = value[kRuleRespawn - kRuleSub];
			saved = true;
		}
		else if (!g_menuSurvival && was && saved) {
			value[kRuleKillTarget - kRuleSub] = savedTarget;
			value[kRuleRespawn - kRuleSub] = savedRespawn;
			saved = false;
		}
		if (g_menuSurvival) {
			value[kRuleKillTarget - kRuleSub] = kKillTargetInfinite;
			value[kRuleRespawn - kRuleSub] = kRespawnOn;
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

unsigned char* RulesBody(int slot)
{
	return slot >= 0 && slot < 16 ? Body(slot) : nullptr;
}

int RulesSlotOf(const uint8_t* character)
{
	if (!g_controllers || !character) {
		return -1;
	}
	for (int slot = 0; slot < 16; slot++) {
		if (Body(slot) == character) {
			return slot;
		}
	}
	return -1;
}

unsigned char* RulesController(int slot)
{
	return g_controllers && slot >= 0 && slot < 16 ? g_controllers[slot] : nullptr;
}

int RulesLocalSlot()
{
	return g_localIndex;
}

bool RulesCamera(float eye[3], float target[3], float up[3], float* fov)
{
	// the local entry's camera (a uCamera: mCameraPos +0x40, mCameraUp +0x50, mTargetPos +0x60, mFov +0x30)
	int self = g_localIndex;
	uint8_t* entry = g_controllers && self >= 0 && self < 16 ? g_controllers[self] : nullptr;
	uint8_t* cam = entry ? *(uint8_t**)(entry + kCtlCamera) : nullptr;
	if (!cam) {
		return false;
	}
	memcpy(eye, cam + 0x40, 12);
	memcpy(up, cam + 0x50, 12);
	memcpy(target, cam + 0x60, 12);
	memcpy(fov, cam + 0x30, 4);
	return true;
}

const char* RulesBattleMessage(uint32_t id)
{
	int self = g_localIndex;
	// the death only counts once its countdown ends, so one in the death view is added here
	int pending = self >= 0 && self < 16 && g_slotState[self] != 0 ? 1 : 0; // any of the death view's states
	bool noneLeft = g_survival && g_lives > 0 && self >= 0 && self < 16 &&
	                (g_out || (g_team ? TeamDeaths(SlotTeam(self)) + pending >= g_lives : SlotDeaths(self) + pending >= g_lives)); // the thread's flag lags
	if ((id == 21 || id == 22) && noneLeft) { // "Respawn in <#RespornTime> seconds" / "second": no respawn coming
		return " "; // the overlay says Eliminated (lives.cpp)
	}
	return g_eliminatedText && (int)id == g_eliminatedId ? g_eliminatedText : nullptr;
}

void RulesNextCreateIsBotMatch()
{
	g_nextIsBotMatch = true;
}

const char* RulesMessage(uint32_t id)
{
	if (id == 0x105 && g_setupOpen) { // state 1's prompt, in order: 0x1a3 (W), 0x105 (Enter), 0x101 (Back)
		static bool logged = false;
		if (!logged) {
			logged = true;
			XLS_LOG_INFO("mods: game rules: prompt 0x105 was \"%s\", 0x1a3 is \"%s\".", MapsOnlineText(0x105).c_str(), MapsOnlineText(0x1a3).c_str());
		}
		return "<PD_DECIDE>Select"; // Enter opens the row; Begin Match starts the match. The tag is the button icon
	}
	if (id >= kMsgSettingsTab && id < kMsgSettingsTab + 3 && g_createTickOriginal) {
		return g_tabText[id - kMsgSettingsTab]; // the tab clips read their labels again on each frame
	}
	if (id >= (uint32_t)kMsgSecondary && id < (uint32_t)(kMsgSecondary + kSecondaryValues)) {
		return kSecondaryNames[id - kMsgSecondary];
	}
	if (id >= (uint32_t)kMsgInfinite && id < (uint32_t)(kMsgInfinite + kInfiniteValues)) {
		return kInfiniteNames[id - kMsgInfinite];
	}
	if (id >= (uint32_t)kMsgSupply && id < (uint32_t)(kMsgSupply + kSupplyValues)) {
		return kSupplyNames[id - kMsgSupply];
	}
	if (id >= (uint32_t)kMsgGrapple && id < (uint32_t)(kMsgGrapple + 6)) {
		return kGrappleNames[id - kMsgGrapple];
	}
	if (id >= (uint32_t)kMsgStagger && id < (uint32_t)(kMsgStagger + kStaggerValues)) {
		return kStaggerNames[id - kMsgStagger];
	}
	if (id == (uint32_t)(kMsgItemName + kItemKillRow) && g_menuSurvival) {
		return "Respawns"; // the Kills row's label while Survival is picked
	}
	if (id == (uint32_t)kMsgSubElim + 2 || id == (uint32_t)kMsgSubTeam + 2) {
		return kSurvivalName;
	}
	// in a Survival lobby: its match is Elimination / Team scored by kills, so the lobby's rules say "Kills"
	if (!g_offline && g_survival && (id == (uint32_t)kSubElimGameMsg[kSubKills] || id == (uint32_t)kSubTeamGameMsg[kSubKills])) {
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

bool FlashAddRootScript(std::vector<uint8_t>* gfx, const std::vector<uint8_t>& script, const char* what)
{
	return InsertRootScript(gfx, Bytes(script.begin(), script.end()), what);
}

bool RulesPatchMenuFlash(std::vector<uint8_t>* gfx)
{
	return InsertRootScript(gfx, RowsScript(), "create-match rows") &&
	       InsertRootScript(gfx, Bytes(std::begin(kSetupScript), std::end(kSetupScript)), "game setup");
}

bool RulesPatchTrainingFlash(std::vector<uint8_t>* gfx)
{
	return InsertRootScript(gfx, Bytes(std::begin(kBotRowsScript), std::end(kBotRowsScript)), "AI Enemy Battle rows");
}

float RulesGrappleRange()
{
	return kGrappleStock * kGrappleScales[g_grapple % kGrappleValues];
}

bool RulesInPlay(int slot)
{
	return slot >= 0 && slot < 16 && InPlay(slot);
}

int RulesTeamOf(int slot)
{
	return SlotTeam(slot);
}

bool RulesTeams()
{
	return TeamGame();
}

}
