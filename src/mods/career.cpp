#include "mods/lp2.h"
#include "mods/parts.h"

#include "core/log.h"

#include <stdint.h>

// Career points from bot matches. The match-end results (DX11 FUN_0100b6b0, thiscall(results)) add the local player's
// match points to the career total (game data DAT_01852238 +0x5060, per mode index FUN_0081fd80(-1), capped 9,999,999)
// only in online sessions: session (DAT_01852144) +0xcc64 == 1 (set when it isn't System Link) and +0x1136db == 1. Bot
// matches run as System Link, so they never counted. The match's points themselves are only worked out when session
// flag 0x4 is set (ranked / player match) or game +0x257 is 0; a bot match has neither (bot match 2026-11-13: flags 0x6a),
// and that block also adds Fraction points, the matches-played count and their achievements. So the figure is worked
// out here the way that block does: the local player's score (slots +0x1154 [player], 0 if negative, read before the
// call: the function rewrites that table at its end), plus 100 per round won (slots +0x1ad4 [player]) when the rounds
// are all played (+0xab >= +0x7c >= 2), times each boost the character has on (FUN_009aa5b0 abilities 0x25-0x29, the
// game's factors, truncated). That goes on the career total only; ranking stats, leaderboards, Fraction points and
// achievements stay online-only.

namespace xls::mods {

namespace {

struct Vas {
	uint32_t results, modeIndex, gameData, game, session, slots;
	uint32_t localPlayer, hasAbility, characters, boost[5]; // the points block's (ability 0x25 + i -> factor boost[i])
};
const Vas kVas[kBuildCount] = {
	{0x100b6b0, 0x81fd80, 0x1852238, 0x18520c0, 0x1852144, 0x1852850, 0x7e27e0, 0x9aa5b0, 0x1852028, {0x185162c, 0x18515e8, 0x185187c, 0x18518e0, 0x1851734}}, // DX11
	{0xffb2d0, 0x810fa0, 0x1842198, 0x1842020, 0x18420a4, 0x18427b0, 0x7d3920, 0x99add0, 0x1841f88, {0x184158c, 0x1841548, 0x18417dc, 0x1841840, 0x1841694}},   // DX9
	{0x100b9a0, 0x81ffa0, 0x1853238, 0x18530c0, 0x1853144, 0x1853850, 0x7e2900, 0x9aa8b0, 0x1853028, {0x185262c, 0x18525e8, 0x185287c, 0x18528e0, 0x1852734}}, // DX11 1.1
	{0xffb5e0, 0x811040, 0x1842198, 0x1842020, 0x18420a4, 0x18427b0, 0x7d3a80, 0x99af90, 0x1841f88, {0x184158c, 0x1841548, 0x18417dc, 0x1841840, 0x1841694}},   // DX9 1.1
};
const uint8_t kPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
const uint32_t kStage = 0x138, kCareer = 0x5060, kOnline = 0xcc64, kMatchType = 0x1136db;
const uint32_t kSlotAi = 0x1b1, kSlotStride = 0x7c, kScores = 0x1154, kRoundWins = 0x1ad4, kRoundsTotal = 0x7c, kRoundsPlayed = 0xab;
const int32_t kCareerMax = 9999999;

using ResultsFn = void(__attribute__((thiscall)) *)(uint8_t*);
using ModeIndexFn = int(__attribute__((thiscall)) *)(uint8_t*, int);
using LocalPlayerFn = uint32_t (*)();
using HasAbilityFn = bool(__attribute__((thiscall)) *)(uint8_t*, int);

ResultsFn g_original = nullptr;
ModeIndexFn g_modeIndex = nullptr;
uint8_t** g_gameData = nullptr;
uint8_t** g_game = nullptr;
uint8_t** g_session = nullptr;
uint8_t** g_slots = nullptr;
LocalPlayerFn g_localPlayer = nullptr;
HasAbilityFn g_hasAbility = nullptr;
uint8_t** g_characters = nullptr;
const float* g_boost[5] = {};

bool BotInMatch(const uint8_t* slots)
{
	for (int s = 0; s < 16; s++) {
		if (slots[kSlotAi + s * kSlotStride]) {
			return true;
		}
	}
	return false;
}

void __attribute__((thiscall)) Results(uint8_t* results)
{
	uint8_t* data = *g_gameData;
	uint8_t* game = *g_game;
	uint8_t* session = *g_session;
	uint8_t* slots = *g_slots;
	bool final = results && *(int32_t*)(results + kStage) == 3;
	bool online = session && *(int32_t*)(session + kOnline) == 1 && session[kMatchType] == 1;
	bool ours = final && data && game && session && slots && !online && BotInMatch(slots);
	uint32_t player = ours ? g_localPlayer() : 0xffffffff;
	ours = ours && player < 16;
	int32_t score = ours ? *(int32_t*)(slots + kScores + player * 4) : 0;
	g_original(results);
	if (!ours) {
		return;
	}
	int32_t points = score < 0 ? 0 : score;
	uint8_t rounds = slots[kRoundsTotal];
	int32_t roundBonus = rounds >= 2 && slots[kRoundsPlayed] >= rounds ? slots[kRoundWins + player] * 100 : 0;
	points += roundBonus;
	uint8_t* character = g_characters[player];
	int boosts = 0;
	for (int i = 0; i < 5 && character; i++) {
		if (g_hasAbility(character, 0x25 + i)) {
			points = (int32_t)((double)points * *g_boost[i]);
			boosts |= 1 << i;
		}
	}
	int mode = g_modeIndex(slots, -1);
	if (mode < 0 || mode > 2 || points <= 0) {
		XLS_LOG_INFO("mods: career: bot match points %d (score %d, rounds +%d), mode index %d: nothing added.", points, score, roundBonus, mode);
		return;
	}
	int32_t* total = (int32_t*)(data + kCareer + mode * 4);
	int32_t was = *total;
	int64_t now = (int64_t)was + points;
	*total = now > kCareerMax ? kCareerMax : (int32_t)now;
	XLS_LOG_INFO("mods: career: bot match: +%d career points (score %d, rounds +%d, boosts 0x%x; mode %d: %d -> %d).", points, score, roundBonus, boosts, mode, was, *total);
}

}

void CareerInstall(uint8_t* base, int build)
{
	const Vas& v = kVas[build];
	auto at = [&](uint32_t va) { return base + (va - 0x400000); };
	g_modeIndex = (ModeIndexFn)at(v.modeIndex);
	g_gameData = (uint8_t**)at(v.gameData);
	g_game = (uint8_t**)at(v.game);
	g_session = (uint8_t**)at(v.session);
	g_slots = (uint8_t**)at(v.slots);
	g_localPlayer = (LocalPlayerFn)at(v.localPlayer);
	g_hasAbility = (HasAbilityFn)at(v.hasAbility);
	g_characters = (uint8_t**)at(v.characters);
	for (int i = 0; i < 5; i++) {
		g_boost[i] = (const float*)at(v.boost[i]);
	}
	g_original = (ResultsFn)Detour(at(v.results), kPrologue, sizeof(kPrologue), (void*)&Results);
	if (!g_original) {
		XLS_LOG_WARN("mods: career: match results not hooked, bot matches give no career points.");
	}
}

}
