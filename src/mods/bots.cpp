// Bot brains (lp2-tools/notes/bot-match-plan.md). Battle bots (uHm26 and its variants) bind their plan, the
// rThinkPlanAI "tool\AI\Think\HumanBotBt" (autoArc\uHm6d.arc), as each one spawns: uHm26::vf41 -> the
// character spec's think setter (DX9 FUN_00f7f0f0, thiscall(spec, path): resource manager get + spec->unit
// +0xfc). The DLL adds our brains to that archive as the game opens it (BotsBuildBrains, from brains.h; an overlay of uHm6d.arc in Mods that has its own wins, lp2-tools lp2brain.py builds one) and we hand it the brain for the
// match's game type instead: HumanBotBt_<mode>, mode from the game's kind byte (game +0x8174, the same
// value the game picks its InitPos / ObjSet "-<Mode>-" files by):
//   1-3 elim (Elimination), 4-7 and 9 team (team battles, VS, Counter), 8 post (Post Grab),
//   10 fox (Fugitive), 11 egg (Akrid Egg), anything else elim.
// If the game refuses the variant (not in the loaded archive) the stock path is used.
//
// cAIPlanFilterWeapon (vf5, thiscall(filter, character)) gets a mode the game doesn't have: mWpMode 3 tests
// the character's other weapon (+0x1694 DX9 / +0x16a4 DX11) instead of the held one (+0x1678 / +0x1688),
// so a brain can switch weapons by range only when the other weapon suits the range. Done by a mode-0 call on
// private copies with the other weapon as the held one. AI runs on worker threads: nothing shared is written here,
// and the personality table takes g_personalityLock.
//
// Personality (bot-match areas only: area category byte game +599 == 0, the aArea_TUTOREAL ones). Every human
// AI has a cAIParam at +0x2418 (DX9) / +0x2428 (DX11): bytes the brain reads through
// cAIPlanEvaluateAIParam (vf5, thiscall(eval, unit) -> float, byte x 0.01, mCheckParamMode at +8). Stock
// HumanBotBt uses: 1 quickness (Avoid, Avoid-G: dodging), 2 calm (Wait), 3 attacker (Strike, ShotTaregt,
// WaitShotP, Reload), 4 field +8 (WaitAI-P, WaitCoverP: holding cover), 7 sekkachi "impatient" (running for
// goals), 9 follow (FriendFollow), 10 granade (ThrowG), 11 appeal (Appeal1: the taunt after a kill, pad
// action 26). The first time a bot is evaluated it gets an archetype (scales on its stock values + jitter),
// kept by unit pointer. Taunts are gated so they're occasional: per bot and 4 s window, a roll against the
// archetype's taunt chance; a failed roll scores appeal 0. The AI Enemy Battle screen's rows (BotMenuOptions) pick
// the brain (Improved = ours, Stock), one archetype for every bot or a mix, a difficulty that scales the traits
// (attacker, quickness, grenade, impatient up; calm down) and how often taunts may come.
#include "mods/lp2.h"
#include "mods/brains.h"
#include "mods/package.h"
#include "mods/parts.h"

#include "core/config.h"
#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

#include <atomic>
#include <math.h>

#include <stdint.h>
#include <string.h>

#include <mutex>
#include <string>
#include <unordered_map>

namespace xls::mods {

namespace {

struct BotVas {
	uint32_t thinkSet;    // the spec's think setter (VA)
	uint32_t filterWeapon; // cAIPlanFilterWeapon::vf5 (VA)
	uint32_t game;        // global -> game (kind byte at +0x8174)
	uint32_t held, other; // character: held / other weapon
	uint32_t evalParam;   // cAIPlanEvaluateAIParam::vf5 (VA)
	uint32_t param;       // character: cAIParam
	uint32_t think;       // cThinkPlayer's per-frame body, thiscall(think, unit), ret 4 (DX11 FUN_0093b1b0)
	uint32_t grudgeTarget; // target the think's Nth biggest grudge, thiscall(think, unit, n), ret 8 (DX11 FUN_0090ffb0)
	uint32_t slots;       // global -> player slot records (0x7c each at +0x148, AI flag at +0x1b1)
	uint32_t target, recentDamage, team; // character: current target (0x1194), recent damage (0x18b4, decays), team byte
	uint32_t lifeRatio, charFlags; // fastcall(character): life / max life (FUN_00900880); character flags (0xd84), bit 20 = has life
	uint32_t resolve;     // plan target -> position, thiscall(?, unit, plan, ctx, target type, float* out, info), ret 0x18 (DX11 FUN_0091bb40)
};
const BotVas kBotVas[kBuildCount] = {
	{0xf8f060, 0x98fd90, 0x18520c0, 0x1688, 0x16a4, 0x992600, 0x2428, 0x93b1b0, 0x90ffb0, 0x1852850, 0x1194, 0x18b4, 0xce2, 0x900880, 0xd84, 0x91bb40}, // DX11
	{0xf7f0f0, 0x980410, 0x1842020, 0x1678, 0x1694, 0x982c80, 0x2418, 0x92b820, 0x900640, 0x18427b0, 0x1184, 0x18a4, 0xcd2, 0x8f0f20, 0xd74, 0x90c1b0}, // DX9
	{0xf8f4f0, 0x98ff70, 0x18530c0, 0x1688, 0x16a4, 0x9927e0, 0x2428, 0x93b420, 0x910280, 0x1853850, 0x1194, 0x18b4, 0xce2, 0x900b60, 0xd84, 0x91bdd0}, // DX11 1.1
	{0xf7f400, 0x9806e0, 0x1842020, 0x1678, 0x1694, 0x982f50, 0x2418, 0x92b9e0, 0x900800, 0x18427b0, 0x1184, 0x18a4, 0xcd2, 0x8f10d0, 0xd74, 0x90c370}, // DX9 1.1
};
// mov edx,[esp+4]; cmp byte [edx+0x1f],1
const uint8_t kEvalPrologue[] = {0x8b, 0x54, 0x24, 0x04, 0x80, 0x7a, 0x1f, 0x01};
// mov edx,[esp+4]; push esi; mov esi,ecx
const uint8_t kThinkPrologue[] = {0x8b, 0x54, 0x24, 0x04, 0x56, 0x8b, 0xf1};
// sub esp,0xc; push ebx; push ebp; push esi
const uint8_t kFilterPrologue[] = {0x83, 0xec, 0x0c, 0x53, 0x55, 0x56};

const uint32_t kGameKind = 0x8174;
const uint32_t kFilterInverse = 0x4, kFilterMode = 0x18;
const uint8_t kModeOther = 3;
const char kStockBrain[] = "tool\\AI\\Think\\HumanBotBt";

typedef uint32_t(__attribute__((thiscall)) * ThinkSetFn)(void* spec, const char* path);
// Both take three stack arguments (ret 0xc: unit, the think, a third; only the unit is used), and a hook that returns
// by itself must clean all three up or the caller's stack is 8 bytes off and it "returns" into its data.
typedef uint32_t(__attribute__((thiscall)) * FilterFn)(uint8_t* filter, uint8_t* character, void* b, void* c);
typedef float(__attribute__((thiscall)) * EvalFn)(uint8_t* eval, uint8_t* unit, void* b, void* c);

// cAIParam byte offsets
enum : uint32_t { kQuickness = 5, kCalm = 6, kAttacker = 7, kHold = 8, kImpatient = 0xb, kFollow = 0xe,
                  kGrenade = 0xf, kAppeal = 0x12 };
const uint32_t kEvalMode = 0x8, kModeAppeal = 11, kAreaCategory = 599;
struct Archetype {
	const char* name;
	float attacker, calm, quickness, hold, impatient, grenade, appeal;
	int taunt; // % chance per 4 s window that a taunt may be picked
};
const Archetype kArchetypes[] = {
	{"aggressor", 1.6f, 0.5f, 0.8f, 0.5f, 1.5f, 1.0f, 1.5f, 40},
	{"tactician", 1.0f, 1.3f, 1.3f, 1.3f, 1.0f, 1.4f, 1.0f, 15},
	{"cautious", 0.6f, 1.2f, 1.6f, 1.8f, 0.7f, 1.0f, 0.5f, 5},
	{"show-off", 1.3f, 0.8f, 1.0f, 0.8f, 1.2f, 0.8f, 3.0f, 70},
	{"grenadier", 1.1f, 1.0f, 1.1f, 1.0f, 1.0f, 2.2f, 1.2f, 25},
};
const int kArchetypeCount = (int)(sizeof(kArchetypes) / sizeof(kArchetypes[0]));

const BotVas* g_vas = nullptr;
uint8_t* g_base = nullptr;
ThinkSetFn g_thinkOriginal = nullptr;
FilterFn g_filterOriginal = nullptr;
EvalFn g_evalOriginal = nullptr;
std::unordered_map<uint8_t*, int> g_personality; // unit -> archetype
std::mutex g_personalityLock; // also covers Rand's seed
const size_t kFilterCharacterSize = 0x16b0; // past the other weapon slot on every build
uint32_t g_seed = 0;
bool g_brains = false; // uHm6d.arc is overlaid (our brains are in it)
int g_lastKind = -1;

uint8_t* At(uint32_t va)
{
	return g_base + (va - 0x400000);
}

const char* ModeName(int kind)
{
	switch (kind) {
	case 4: case 5: case 6: case 7: case 9:
		return "team";
	case 8:
		return "post";
	case 10:
		return "fox";
	case 11:
		return "egg";
	default:
		return "elim";
	}
}

int GameKind()
{
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	return game ? game[kGameKind] : -1;
}

uint32_t BrainCrc(const Bytes& data)
{
	uint32_t crc = 0xffffffffu;
	for (uint8_t b : data) {
		crc ^= b;
		for (int i = 0; i < 8; i++) {
			crc = crc >> 1 ^ (0xedb88320u & (0u - (crc & 1)));
		}
	}
	return ~crc;
}

// The embedded brains are edits against the player's own stock file (lp2brain.py --header): varint (length << 1 | copy),
// then a varint offset into the stock file, or that many new bytes.
bool BrainApply(const Bytes& stock, const EmbeddedBrain& brain, Bytes* out)
{
	const uint8_t* p = brain.ops;
	const uint8_t* end = p + brain.opsSize;
	auto varint = [&](uint32_t* v) {
		*v = 0;
		for (int shift = 0; p < end && shift < 35; shift += 7) {
			uint8_t b = *p++;
			*v |= (uint32_t)(b & 0x7f) << shift;
			if (b < 0x80) {
				return true;
			}
		}
		return false;
	};
	out->clear();
	out->reserve(brain.size);
	while (p < end) {
		uint32_t n, at;
		if (!varint(&n)) {
			return false;
		}
		const uint32_t length = n >> 1;
		if (n & 1) {
			if (!varint(&at) || at > stock.size() || length > stock.size() - at) {
				return false;
			}
			out->insert(out->end(), stock.begin() + at, stock.begin() + at + length);
		}
		else {
			if (length > (size_t)(end - p)) {
				return false;
			}
			out->insert(out->end(), p, p + length);
			p += length;
		}
	}
	return out->size() == brain.size && BrainCrc(*out) == brain.crc;
}

// The AI Enemy Battle areas (category 0; campaign episodes are 1-5), or a versus match this PC hosts with bots.
bool BotWorld(const uint8_t* game)
{
	return game && (game[kAreaCategory] == 0 || (game[0x256] && BotsInPlayerMatch()));
}

const char* const kPartnerBrain = "tool\\AI\\Think\\HumanBotCp";

uint32_t __attribute__((thiscall)) ThinkSet(void* spec, const char* path)
{
	// A player match's bots are built as campaign partners (FUN_00828ca0's co-op route), so they ask for HumanBotCp.
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	bool partner = path && strcmp(path, kPartnerBrain) == 0, battle = path && strcmp(path, kStockBrain) == 0;
	// Only in a bot match or a player match with bots: campaign and co-op keep the game's own brains, whichever they ask for.
	if (g_brains && BotMenuOptions().improved && (partner ? game && game[0x256] && BotsInPlayerMatch() : battle && BotWorld(game))) {
		int kind = GameKind();
		std::string mine = std::string(kStockBrain) + "_" + ModeName(kind);
		uint32_t ok = g_thinkOriginal(spec, mine.c_str());
		if (kind != g_lastKind) {
			g_lastKind = kind;
			XLS_LOG_INFO("mods: bots: game kind %d -> brain %s (%s).", kind, mine.c_str(), (ok & 0xff) ? "loaded" : "missing, stock used");
		}
		if (ok & 0xff) {
			return ok;
		}
	}
	return g_thinkOriginal(spec, path);
}

uint32_t __attribute__((thiscall)) FilterWeapon(uint8_t* filter, uint8_t* character, void* b, void* c)
{
	if (filter[kFilterMode] != kModeOther || !character) {
		return g_filterOriginal(filter, character, b, c);
	}
	uint8_t* other = *(uint8_t**)(character + g_vas->other);
	if (!other) {
		return filter[kFilterInverse] ? 1 : 0; // no other weapon: it matches nothing
	}
	// AI evaluates on worker threads and brains are shared between bots, so neither the character nor the filter may
	// be changed, even briefly. The game's filter only reads plain fields of both (no calls), so it gets private copies.
	uint8_t filterCopy[kFilterMode + 1];
	uint8_t characterCopy[kFilterCharacterSize];
	memcpy(filterCopy, filter, sizeof(filterCopy));
	memcpy(characterCopy, character, g_vas->other + sizeof(uint8_t*));
	filterCopy[kFilterMode] = 0;
	*(uint8_t**)(characterCopy + g_vas->held) = other;
	return g_filterOriginal(filterCopy, characterCopy, b, c);
}

uint32_t Rand()
{
	g_seed = g_seed * 1103515245u + 12345u;
	return g_seed >> 8;
}

uint32_t Mix(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	return x ^ (x >> 16);
}

void Scale(uint8_t* p, uint32_t at, float m)
{
	float jitter = 0.8f + (Rand() % 41) / 100.0f; // 0.8 .. 1.2
	float base = p[at] < 20 && m > 1.0f ? 20.0f : (float)p[at]; // a boosted trait the stock bot lacks
	float v = base * m * jitter;
	p[at] = (uint8_t)(v < 0 ? 0 : v > 200 ? 200 : v);
}

int Personality(uint8_t* unit)
{
	std::lock_guard<std::mutex> lock(g_personalityLock); // bots first evaluate on several worker threads at once
	auto it = g_personality.find(unit);
	if (it != g_personality.end()) {
		return it->second;
	}
	if (g_personality.size() > 256) {
		g_personality.clear(); // stale units from earlier matches
	}
	BotOptions o = BotMenuOptions();
	int a = o.personality > 0 ? (o.personality - 1) % kArchetypeCount : (int)(Rand() % kArchetypeCount);
	g_personality[unit] = a;
	uint8_t* p = unit + g_vas->param;
	const Archetype& k = kArchetypes[a];
	// difficulty on top of the archetype: fight, dodge, grenades and urgency up, waiting down (and the reverse)
	const float kDifficulty[] = {0.6f, 1.0f, 1.3f, 2.0f};
	float d = kDifficulty[o.difficulty & 3];
	uint8_t before[3] = {p[kAttacker], p[kQuickness], p[kAppeal]};
	Scale(p, kAttacker, k.attacker * d);
	Scale(p, kCalm, k.calm / d);
	Scale(p, kQuickness, k.quickness * d);
	Scale(p, kHold, k.hold);
	Scale(p, kImpatient, k.impatient * d);
	Scale(p, kGrenade, k.grenade * d);
	Scale(p, kAppeal, k.appeal);
	XLS_LOG_INFO("mods: bots: %p is a %s, difficulty x%.1f (attacker %u -> %u, quickness %u -> %u, appeal %u -> %u).", unit,
	             k.name, d, before[0], p[kAttacker], before[1], p[kQuickness], before[2], p[kAppeal]);
	return a;
}

// What the aim code (FUN_00903f10) reads, logged once a match starts so its effect can be judged: the game's difficulty
// index, then per difficulty the lead ratio and the aim offset. Read only. Order: Normal, Hard, Extreme, Single.
struct AimVas {
	uint32_t owner;
	uint32_t ratio[4], offset[4];
};
const AimVas kAimVas[kBuildCount] = {
	{0x1852238, {0x185167c, 0x185176c, 0x1851640, 0x18517f8}, {0x185180c, 0x1851838, 0x1851788, 0x1851898}}, // DX11
	{0x1842198, {0x18415dc, 0x18416cc, 0x18415a0, 0x1841758}, {0x184176c, 0x1841798, 0x18416e8, 0x18417f8}}, // DX9
	{0x1853238, {0x185267c, 0x185276c, 0x1852640, 0x18527f8}, {0x185280c, 0x1852838, 0x1852788, 0x1852898}}, // DX11 1.1
	{0x1842198, {0x18415dc, 0x18416cc, 0x18415a0, 0x1841758}, {0x184176c, 0x1841798, 0x18416e8, 0x18417f8}}, // DX9 1.1
};

void LogAimValues()
{
	static std::atomic<bool> logged{false};
	int build = GameBuild();
	if (build < 0 || logged.exchange(true)) {
		return;
	}
	const AimVas& v = kAimVas[build];
	uint8_t* owner = *(uint8_t**)At(v.owner);
	auto value = [](uint32_t va) { return *(float*)At(va); };
	XLS_LOG_INFO("mods: bots: aim: difficulty index %d, lead ratio %.3f %.3f %.3f %.3f, offset %.3f %.3f %.3f %.3f.", owner ? owner[0x5e0] : -1,
	             value(v.ratio[0]), value(v.ratio[1]), value(v.ratio[2]), value(v.ratio[3]),
	             value(v.offset[0]), value(v.offset[1]), value(v.offset[2]), value(v.offset[3]));
}

// Mode 15, ours (the game's modes stop at 14 and score 0 past it): 100 while the bot's target is reloading or has an empty
// magazine. Weapon (character's held one) +0x210c reload timer, above 0 while reloading (the game's FUN_0091f640 waits on
// it); +0x20fc rounds loaded, +0x20fe in reserve. DX9 weapons are 0x10 lower.
bool BotLogOn();
const uint32_t kModeTargetReload = 15;
bool TargetReloading(uint8_t* unit)
{
	uint8_t* target = *(uint8_t**)(unit + g_vas->target);
	if (!target || target[0x1f] != 1) {
		return false;
	}
	uint8_t* weapon = *(uint8_t**)(target + g_vas->held);
	if (!weapon) {
		return false;
	}
	const uint32_t d = g_vas->held == 0x1688 ? 0 : 0x10;
	float timer = *(float*)(weapon + 0x210c - d);
	int16_t loaded = *(int16_t*)(weapon + 0x20fc - d), reserve = *(int16_t*)(weapon + 0x20fe - d);
	return timer > 0.0f || (loaded == 0 && reserve > 0);
}

float __attribute__((thiscall)) EvalParam(uint8_t* eval, uint8_t* unit, void* b, void* c)
{
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	if (!unit || unit[0x1f] != 1 || !BotWorld(game)) {
		return g_evalOriginal(eval, unit, b, c);
	}
	LogAimValues();
	if (*(uint32_t*)(eval + kEvalMode) == kModeTargetReload) {
		bool r = TargetReloading(unit);
		static DWORD logged = 0;
		if (r && BotLogOn() && GetTickCount() - logged > 3000) {
			logged = GetTickCount();
			XLS_LOG_INFO("mods: bots: %p sees its target reloading.", unit);
		}
		return r ? 1.0f : 0.0f;
	}
	int a = Personality(unit);
	if (*(uint32_t*)(eval + kEvalMode) == kModeAppeal) {
		uint32_t roll = Mix((uint32_t)(uintptr_t)unit ^ (GetTickCount() / 4000) * 0x9e3779b9u) % 100;
		const float kTauntScale[] = {0.0f, 0.4f, 1.0f, 2.0f};
		if ((float)roll >= kArchetypes[a].taunt * kTauntScale[BotMenuOptions().taunts & 3]) {
			return 0.0f;
		}
	}
	return g_evalOriginal(eval, unit, b, c);
}

// Being shot: a bot learns who did it and turns on them. The game keeps the think's attackers in mDmgGrudge[16] (+4 fighter,
// +8 grudge, +0xa hits since their last one; FUN_00908a30), but only retargets at grudge > 0x60 and on a 5 s timer, so a
// rifle at range went unanswered. Improved brain, bot slots in bot-match areas: rising recent damage = a hit, and after a
// reaction delay (by difficulty) the newest attacker becomes the target, through the game's own grudge targeting, which
// also gives the bot their position. A switch costs 2 s before the next one, so crossfire doesn't make it flip every hit.
typedef uint32_t(__attribute__((thiscall)) * ThinkFn)(uint8_t*, uint8_t*);
typedef void(__attribute__((thiscall)) * GrudgeTargetFn)(uint8_t*, uint8_t*, int);
ThinkFn g_thinkBodyOriginal = nullptr;
struct Reaction {
	float damage = 0.0f;
	uint8_t* pending = nullptr;
	DWORD due = 0, nextSwitch = 0;
};
Reaction g_reactions[16]; // by slot; each slot's think runs on one thread at a time
const int kThinkSlot = 0x84, kGrudgeCount = 16, kGrudgeStride = 8, kGrudgeFighter = 4, kGrudgeValue = 8, kGrudgeAge = 0xa;
const uint16_t kGrudgeMax = 0x80;

bool Alive(uint8_t* unit)
{
	const uint32_t state = *(uint32_t*)(unit + 4) & 7;
	return state == 1 || state == 2;
}

uint8_t* NewestAttacker(uint8_t* think)
{
	uint8_t* best = nullptr;
	int bestAge = 0x10000;
	for (int i = 0; i < kGrudgeCount; i++) {
		uint8_t* entry = think + i * kGrudgeStride;
		uint8_t* fighter = *(uint8_t**)(entry + kGrudgeFighter);
		const int age = *(uint16_t*)(entry + kGrudgeAge);
		if (fighter && age < bestAge) {
			best = fighter;
			bestAge = age;
		}
	}
	return best;
}

void TargetAttacker(uint8_t* think, uint8_t* unit, uint8_t* attacker)
{
	for (int i = 0; i < kGrudgeCount; i++) { // the game targets by biggest grudge: make that this attacker
		uint8_t* entry = think + i * kGrudgeStride;
		uint16_t& value = *(uint16_t*)(entry + kGrudgeValue);
		if (*(uint8_t**)(entry + kGrudgeFighter) == attacker) {
			value = kGrudgeMax;
		}
		else if (value >= kGrudgeMax) {
			value = kGrudgeMax - 1;
		}
	}
	((GrudgeTargetFn)At(g_vas->grudgeTarget))(think, unit, 0);
}

// Brains can't read health, only recent damage. Two AI param bytes nothing in the game reads except EvaluateAIParam /
// FilterAIParam (modes 13 and 14) carry it instead, as a percentage: the bot's own and its target's. Not synced: bots
// think on one PC. A target without a life value (a VS, nothing) reads as 100, so nothing chases it as "nearly dead".
const uint32_t kOwnLife = 0x13, kTargetLife = 0x11;
// Mode 12: 100 when the plan has a data post to take (cThinkPlanAI +0x890, the nearest this bot can capture).
const uint32_t kPostFree = 0x15, kThinkPlan = 0x1a4, kPlanPost = 0x890;
uint8_t LifePercent(uint8_t* character)
{
	typedef float(__attribute__((fastcall)) * LifeRatioFn)(uint8_t*);
	if (!character || character[0x1f] != 1 || !((*(uint32_t*)(character + g_vas->charFlags) >> 20) & 1)) {
		return 100;
	}
	float ratio = ((LifeRatioFn)At(g_vas->lifeRatio))(character);
	return (uint8_t)(ratio <= 0.0f ? 0 : ratio >= 1.0f ? 100 : ratio * 100.0f);
}

// Debugging, off unless ModCache\\bot-log.txt exists (read at the first bot think): a line each time a bot's running plan
// item changes, for judging brains from a match's log. cThinkPlanAI mpItemNow (+0x710) -> cItemProcessor, +0x30 the item,
// its mName at +4 (MtString: chars at +8).
bool BotLogOn()
{
	static const bool enabled = [] {
		Bytes unused;
		return ReadWholeFile(Cfg().modCacheDir + L"bot-log.txt", &unused);
	}();
	return enabled;
}

void LogPlan(uint8_t slot, uint8_t* plan, uint8_t* unit, const uint8_t* p)
{
	static void* last[16];
	if (!BotLogOn() || !plan) {
		return;
	}
	void* now = *(void**)(plan + 0x710);
	if (now == last[slot]) {
		return;
	}
	last[slot] = now;
	uint8_t* item = now ? *(uint8_t**)((uint8_t*)now + 0x30) : nullptr;
	uint8_t* name = item ? *(uint8_t**)(item + 4) : nullptr;
	uint8_t* target = *(uint8_t**)(unit + g_vas->target);
	float distance = -1.0f;
	if (target) {
		float dx = *(float*)(target + 0xd0) - *(float*)(unit + 0xd0), dy = *(float*)(target + 0xd4) - *(float*)(unit + 0xd4),
		      dz = *(float*)(target + 0xd8) - *(float*)(unit + 0xd8);
		distance = sqrtf(dx * dx + dy * dy + dz * dz) / 100.0f;
	}
	XLS_LOG_INFO("mods: bots: plan: slot %u %s, life %u%%, target life %u%% at %.0f m, hurt %.0f, post %s.", slot,
	             name ? (const char*)(name + 8) : "(none)", p[kOwnLife], p[kTargetLife], distance,
	             *(float*)(unit + g_vas->recentDamage), p[kPostFree] ? "free" : "none");
}

// After a bot uses a data post (an Exec...Post item ends), that post is off its list for a while: the game keeps offering
// the same post while it boots up, so the bot looped run to it / activate / stand there thinking.
const DWORD kPostDone = 15000;
struct PostDone {
	uint8_t* post = nullptr;
	DWORD at = 0;
	bool inExec = false;
};
PostDone g_postDone[16];
uint8_t* g_postUnits[16];

const char* PlanItemName(uint8_t* plan)
{
	void* now = plan ? *(void**)(plan + 0x710) : nullptr;
	uint8_t* item = now ? *(uint8_t**)((uint8_t*)now + 0x30) : nullptr;
	uint8_t* name = item ? *(uint8_t**)(item + 4) : nullptr;
	return name ? (const char*)(name + 8) : "";
}

bool PostRecentlyDone(int slot, uint8_t* post)
{
	return slot >= 0 && slot < 16 && post && g_postDone[slot].post == post && GetTickCount() - g_postDone[slot].at < kPostDone;
}

// Dodge direction. cAIPlanMoveHmAction::vf7 (DX11 0x99b390), pad action 4, starts a dodge: it sets pad +0x1cc bit 31 and
// gives the pad a direction (pad vtable +0x14c, a world vector): away from grenades and the line of fire, else a random side.
// When the bot is out of its weapon's range (the plan item says which class: Avoid / Avoid-Short, beyond 20 / 7 m) the roll
// goes forward instead, diagonally, towards its target. Long and heavy weapons, and in-range bots, keep the game's choice.
typedef uint32_t(__attribute__((thiscall)) * HmActionFn)(uint8_t*, uint8_t*, uint8_t*, uint8_t*);
typedef void(__attribute__((thiscall)) * PadDirFn)(uint8_t*, float*);
HmActionFn g_hmActionOriginal = nullptr;
const uint32_t kDodgeVas[kBuildCount] = {0x99b390, 0x98bb40, 0x99b6b0, 0x98be70};
const uint32_t kPadDodge = 0x1cc, kPadSetDir = 0x14c;

uint32_t __attribute__((thiscall)) HmAction(uint8_t* move, uint8_t* unit, uint8_t* plan, uint8_t* pad)
{
	bool dodge = move && move[4] == 4 && pad;
	bool before = dodge && (*(uint32_t*)(pad + kPadDodge) >> 31);
	uint32_t r = g_hmActionOriginal(move, unit, plan, pad);
	if (!dodge || before || !(*(uint32_t*)(pad + kPadDodge) >> 31) || !unit || !plan || !BotMenuOptions().improved) {
		return r;
	}
	uint8_t* target = *(uint8_t**)(unit + g_vas->target);
	if (!target) {
		return r;
	}
	const char* item = PlanItemName(plan);
	float range = strcmp(item, "Avoid") == 0 ? 2000.0f : strcmp(item, "Avoid-Short") == 0 ? 700.0f : -1.0f;
	const float* a = (const float*)(unit + 0xd0);
	const float* b = (const float*)(target + 0xd0);
	float dx = b[0] - a[0], dz = b[2] - a[2], d = sqrtf(dx * dx + dz * dz);
	if (range < 0 || d < range || d < 1.0f) {
		return r;
	}
	dx /= d;
	dz /= d;
	float side = Rand() & 1 ? 0.6f : -0.6f;
	float dir[4] = {dx * 0.8f - dz * side, 0.0f, dz * 0.8f + dx * side, 0.0f};
	float n = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
	dir[0] /= n;
	dir[2] /= n;
	(*(PadDirFn*)(*(uint8_t**)pad + kPadSetDir))(pad, dir);
	static DWORD logged = 0;
	if (BotLogOn() && GetTickCount() - logged > 3000) {
		logged = GetTickCount();
		XLS_LOG_INFO("mods: bots: %p dodges towards its target %.0f m away (%s).", unit, d / 100.0f, item);
	}
	return r;
}

void ReactToHit(uint8_t* think, uint8_t* unit)
{
	const uint8_t slot = think[kThinkSlot];
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	uint8_t* slots = *(uint8_t**)At(g_vas->slots);
	if (slot >= 16 || !unit || !slots || !BotWorld(game) || !slots[0x1b1 + slot * 0x7c] || !BotMenuOptions().improved) {
		return;
	}
	uint8_t* p = unit + g_vas->param;
	p[kOwnLife] = LifePercent(unit);
	p[kTargetLife] = LifePercent(*(uint8_t**)(unit + g_vas->target));
	uint8_t* plan = *(uint8_t**)(think + kThinkPlan);
	uint8_t* post = plan ? *(uint8_t**)(plan + kPlanPost) : nullptr;
	{
		const char* item = PlanItemName(plan);
		bool exec = strncmp(item, "Exec", 4) == 0 && strstr(item, "Post");
		PostDone& d = g_postDone[slot];
		if (d.inExec && !exec && post) {
			d.post = post;
			d.at = GetTickCount();
			if (BotLogOn()) {
				XLS_LOG_INFO("mods: bots: slot %u used post %p: leaves it for %lu s.", slot, post, (unsigned long)(kPostDone / 1000));
			}
		}
		d.inExec = exec;
		g_postUnits[slot] = unit;
	}
	p[kPostFree] = post && !PostRecentlyDone(slot, post) ? 100 : 0;
	LogPlan(slot, plan, unit, p);
	Reaction& r = g_reactions[slot];
	const float damage = *(float*)(unit + g_vas->recentDamage);
	const bool hit = damage > r.damage;
	r.damage = damage;
	const DWORD now = GetTickCount();
	uint8_t* current = *(uint8_t**)(unit + g_vas->target);
	if (hit && !r.pending && (int)(now - r.nextSwitch) >= 0) {
		uint8_t* attacker = NewestAttacker(think);
		const int kind = GameKind();
		const bool freeForAll = kind >= 1 && kind <= 3; // Elimination: everyone's an enemy
		if (attacker && attacker != unit && attacker != current && Alive(attacker) &&
		    (freeForAll || attacker[g_vas->team] != unit[g_vas->team])) {
			const DWORD kReaction[] = {900, 500, 250, 100};
			r.pending = attacker;
			r.due = now + kReaction[BotMenuOptions().difficulty & 3];
		}
		else if (!attacker) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				XLS_LOG_WARN("mods: bots: a bot was hit but its attacker list is empty.");
			}
		}
	}
	if (r.pending && (int)(now - r.due) >= 0) {
		uint8_t* attacker = r.pending;
		r.pending = nullptr;
		// long overdue = this slot stopped thinking meanwhile (match over): the pointer may be gone
		if (now - r.due < 500 && Alive(attacker) && attacker != current) {
			TargetAttacker(think, unit, attacker);
			r.nextSwitch = now + 2000;
			static LONG logged = 0;
			if (InterlockedIncrement(&logged) <= 20) {
				XLS_LOG_INFO("mods: bots: slot %u hit, target %p -> attacker %p (now %p).", slot, current, attacker, *(uint8_t**)(unit + g_vas->target));
			}
		}
	}
}

// Where a bot with nobody to fight goes. Stock has no such thing: its idle items aim at Pef_Hm40's ShotPoint, 3-10 m away, and with no
// free post ExecDataPost (800 against RUN's 300) "takes" the bot's own spot for 5 s, as target 16 then resolves to it. So:
//   Target 16 with no post now fails, as does one that gunfire outranks (not in Post Grab, nor for a post the bot is at).
//   Target 20 (FreePoint, unused by stock; our brains' RUN and RunToPatrol, lp2brain.py roam) is a goal kept per bot until it's reached or
//   the bot stops getting closer: gunfire within earshot, else a far spawn point, which every map has and a player can stand on.
// Posts come between the two in the brains: ExecPostFar outscores RUN.
typedef char(__attribute__((thiscall)) * ResolveFn)(void*, uint8_t*, uint8_t*, void*, uint32_t, float*, void*);
ResolveFn g_resolveOriginal = nullptr;
const int kTgtPost = 16, kTgtRoam = 20, kKindPostGrab = 8, kInitPosSet = 0x52e4, kSetCount = 0x70, kSetArray = 0x7c, kRecPos = 0x10, kRecKind = 0x26;
const float kHearing = 6000.0f, kArrive = 600.0f, kRoamNear = 2000.0f, kPostHere = 800.0f, kProgress = 300.0f;
const DWORD kShotHeard = 3000, kNoiseLasts = 10000, kNoiseCheck = 250, kStuckCheck = 6000, kNoiseRest = 2000;
struct RoamGoal {
	float pos[3] = {0, 0, 0};
	float best = 0;
	bool has = false, noise = false;
	DWORD used = 0, set = 0, heard = 0, progress = 0, rest = 0;
	int hopFrom = -1, hopTo = -1; // the area link it's running along, its best distance to the hop's end and when that last improved
	float hopBest = 0;
	DWORD hopProgress = 0;
};
std::unordered_map<uint8_t*, RoamGoal> g_goals;
std::mutex g_goalLock;

float Distance2(const float* a, const float* b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

// The map split into areas: its start points grouped on a kAreaCell grid (x/z), each area's goal the point nearest its
// points' middle (a real spawn spot, so reachable). Per team (unit team byte, 16 kept): when its bots last went through an
// area (swept) and last heard fighting there. Bots go to fresh fighting first (mostly the nearest), else the nearest area
// the team hasn't swept lately that no teammate is already headed for; all swept, the team starts over.
const float kAreaCell = 2500.0f, kAreaLevel = 500.0f, kStep = 4500.0f, kStepUp = 600.0f, kLevelWeight = 4.0f;
const DWORD kHopStuck = 5000, kBlockedFor = 120000;
const DWORD kSweptFor = 120000, kFightFor = 30000;
struct Area {
	float pos[3] = {0, 0, 0};
	std::vector<int> links; // areas a bot can run to in one go (the game's path gives up on long runs)
	DWORD swept[16] = {}, fight[16] = {};
};
std::vector<Area> g_areas;
std::vector<const float*> g_points; // the start points (not base markers)
std::unordered_map<int64_t, DWORD> g_blocked; // link from * 1024 + to -> when a bot got stuck on it
std::unordered_map<int64_t, int> g_areaCells;
const void* g_areaSet = nullptr;
uint32_t g_areaCount = 0;

// x/z cells, and levels: the same spot on another floor is another area.
int64_t AreaCell(const float* p)
{
	return ((int64_t)floorf(p[0] / kAreaCell) * 100003 + (int64_t)floorf(p[2] / kAreaCell)) * 4099 + (int64_t)floorf(p[1] / kAreaLevel);
}

// Distance with height weighted up, so the nearest area is on the same floor.
float LevelDistance2(const float* a, const float* b)
{
	float dx = a[0] - b[0], dy = (a[1] - b[1]) * kLevelWeight, dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

// Caller holds g_goalLock. Rebuilt when the start point set changes (a new map or round).
void AreasBuild()
{
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	uint8_t* set = game ? *(uint8_t**)(game + kInitPosSet) : nullptr;
	uint32_t count = set ? *(uint32_t*)(set + kSetCount) : 0;
	uint8_t** records = set ? *(uint8_t***)(set + kSetArray) : nullptr;
	if (set == g_areaSet && count == g_areaCount) {
		return;
	}
	g_areaSet = set;
	g_areaCount = count;
	g_areas.clear();
	g_areaCells.clear();
	g_points.clear();
	if (!records || !count || count > 256) {
		return;
	}
	struct Sum { double x = 0, y = 0, z = 0; int n = 0; std::vector<const float*> pts; };
	std::unordered_map<int64_t, Sum> cells;
	for (uint32_t i = 0; i < count; i++) {
		uint8_t* r = records[i];
		if (!r || r[kRecKind] > 5) { // 6, 7: team base markers
			continue;
		}
		const float* pos = (const float*)(r + kRecPos);
		g_points.push_back(pos);
		Sum& c = cells[AreaCell(pos)];
		c.x += pos[0];
		c.y += pos[1];
		c.z += pos[2];
		c.n++;
		c.pts.push_back(pos);
	}
	for (auto& [cell, c] : cells) {
		float mid[3] = {(float)(c.x / c.n), (float)(c.y / c.n), (float)(c.z / c.n)};
		const float* best = c.pts[0];
		for (const float* p : c.pts) {
			best = Distance2(p, mid) < Distance2(best, mid) ? p : best;
		}
		Area a;
		memcpy(a.pos, best, sizeof(a.pos));
		g_areaCells[cell] = (int)g_areas.size();
		g_areas.push_back(a);
	}
	// links: near enough across and up or down; an area with none gets its nearest
	int links = 0;
	for (size_t i = 0; i < g_areas.size(); i++) {
		int nearest = -1;
		float nearD = 1e30f;
		for (size_t j = 0; j < g_areas.size(); j++) {
			if (i == j) {
				continue;
			}
			const float *a = g_areas[i].pos, *b = g_areas[j].pos;
			float across = (a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]), up = fabsf(a[1] - b[1]);
			if (across <= kStep * kStep && up <= kStepUp) {
				g_areas[i].links.push_back((int)j);
				links++;
			}
			float d = LevelDistance2(a, b);
			if (d < nearD) {
				nearD = d;
				nearest = (int)j;
			}
		}
		if (g_areas[i].links.empty() && nearest >= 0) {
			g_areas[i].links.push_back(nearest);
			links++;
		}
	}
	g_blocked.clear();
	XLS_LOG_INFO("mods: bots: map split into %u areas, %d links (%u start points).", (unsigned)g_areas.size(), links, count);
}

int AreaAt(const float* p)
{
	auto it = g_areaCells.find(AreaCell(p));
	if (it != g_areaCells.end()) {
		return it->second;
	}
	int best = -1; // between areas: the nearest one, by level first
	float bestD = 1e30f;
	for (size_t i = 0; i < g_areas.size(); i++) {
		float d = LevelDistance2(p, g_areas[i].pos);
		if (d < bestD) {
			bestD = d;
			best = (int)i;
		}
	}
	return best;
}

// Caller holds g_goalLock. Where to run now for a goal: in the goal's area, the goal; else the next area on the shortest route
// over the links (Dijkstra; a blocked link costs a lot, so routes go round it while there's another way). -1 hop: the goal.
const float* Waypoint(const float* from, const float* goal, int* hopFrom, int* hopTo)
{
	*hopFrom = *hopTo = -1;
	int start = AreaAt(from), end = AreaAt(goal), n = (int)g_areas.size();
	if (start < 0 || end < 0 || start == end || n > 1024) {
		return goal;
	}
	DWORD now = GetTickCount();
	std::vector<float> cost(n, 1e30f);
	std::vector<int> prev(n, -1);
	std::vector<bool> done(n, false);
	cost[start] = 0;
	for (;;) {
		int u = -1;
		for (int i = 0; i < n; i++) {
			if (!done[i] && cost[i] < 1e29f && (u < 0 || cost[i] < cost[u])) {
				u = i;
			}
		}
		if (u < 0 || u == end) {
			break;
		}
		done[u] = true;
		for (int v : g_areas[u].links) {
			auto it = g_blocked.find((int64_t)u * 1024 + v);
			bool blocked = it != g_blocked.end() && now - it->second < kBlockedFor;
			float c = cost[u] + sqrtf(Distance2(g_areas[u].pos, g_areas[v].pos)) + (blocked ? 1e6f : 0.0f);
			if (c < cost[v]) {
				cost[v] = c;
				prev[v] = u;
			}
		}
	}
	if (prev[end] < 0) {
		return goal; // no route: straight there, the game's way
	}
	int hop = end;
	while (prev[hop] != start) {
		hop = prev[hop];
	}
	*hopFrom = start;
	*hopTo = hop;
	return hop == end ? goal : g_areas[hop].pos;
}

// Caller holds g_goalLock.
bool RoamPoint(uint8_t* unit, const float* from, RoamGoal& g)
{
	AreasBuild();
	if (g_areas.empty()) {
		return false;
	}
	int team = unit[g_vas->team] & 15, here = AreaAt(from);
	DWORD now = GetTickCount();
	const float* mates[16];
	int nMates = 0;
	for (auto& [other, og] : g_goals) {
		if (other != unit && og.has && !og.noise && nMates < 16 && other[g_vas->team] == unit[g_vas->team]) {
			mates[nMates++] = og.pos;
		}
	}
	auto taken = [&](const float* p) {
		for (int i = 0; i < nMates; i++) {
			if (Distance2(p, mates[i]) < kAreaCell * kAreaCell / 4) {
				return true;
			}
		}
		return false;
	};
	int pick = -1;
	float bestScore = 1e30f;
	bool fight = false;
	for (size_t i = 0; i < g_areas.size(); i++) { // fresh fighting: metres away plus 2 per second old, a little chance
		const Area& a = g_areas[i];
		if ((int)i == here || !a.fight[team] || now - a.fight[team] > kFightFor) {
			continue;
		}
		float score = sqrtf(Distance2(from, a.pos)) / 100.0f + (now - a.fight[team]) / 500.0f + Rand() % 15;
		if (score < bestScore) {
			bestScore = score;
			pick = (int)i;
			fight = true;
		}
	}
	for (int pass = 0; pass < 2 && pick < 0; pass++) {
		for (size_t i = 0; i < g_areas.size(); i++) { // the nearest unswept area nobody on the team is headed for
			const Area& a = g_areas[i];
			if ((int)i == here || (a.swept[team] && now - a.swept[team] < kSweptFor)) {
				continue;
			}
			float score = sqrtf(Distance2(from, a.pos)) / 100.0f + (taken(a.pos) ? 100.0f : 0.0f) + Rand() % 20;
			if (score < bestScore) {
				bestScore = score;
				pick = (int)i;
			}
		}
		if (pick < 0) { // all swept: start over
			for (Area& a : g_areas) {
				a.swept[team] = 0;
			}
			if (BotLogOn()) {
				XLS_LOG_INFO("mods: bots: team %d swept every area, starting over.", team);
			}
		}
	}
	if (pick < 0) {
		return false;
	}
	memcpy(g.pos, g_areas[pick].pos, sizeof(g.pos));
	g.has = true;
	g.noise = false;
	if (BotLogOn()) {
		const Area& a = g_areas[pick];
		if (fight) {
			XLS_LOG_INFO("mods: bots: %p heads for fighting in area %d (%.0f s ago).", unit, pick, (now - a.fight[team]) / 1000.0f);
		}
		else {
			XLS_LOG_INFO("mods: bots: %p sweeps area %d.", unit, pick);
		}
	}
	return true;
}

// Caller holds g_goalLock: the area the bot is in is swept for its team; gunfire it hears marks the shot's area.
void AreasNote(uint8_t* unit, const float* here, const RadarShotInfo* shots, int n, bool everyone, DWORD now)
{
	AreasBuild();
	int team = unit[g_vas->team] & 15, at = AreaAt(here);
	if (at >= 0) {
		g_areas[at].swept[team] = now;
	}
	for (int i = 0; i < n; i++) {
		if (shots[i].who != unit && (everyone || shots[i].team != unit[g_vas->team]) && Distance2(here, shots[i].pos) < kHearing * kHearing) {
			int area = AreaAt(shots[i].pos);
			if (area >= 0) {
				g_areas[area].fight[team] = now;
			}
		}
	}
}

// Caller holds g_goalLock.
void GoalUpdate(uint8_t* unit, RoamGoal& g, DWORD now)
{
	const float* here = (const float*)(unit + 0xd0);
	if (now - g.heard >= kNoiseCheck && (int)(now - g.rest) >= 0) {
		g.heard = now;
		RadarShotInfo shots[32];
		int n = RadarRecentShots(shots, 32, kShotHeard), kind = GameKind();
		bool everyone = kind >= 1 && kind <= 3;
		AreasNote(unit, here, shots, n, everyone, now);
		float nearest = g.has && g.noise ? Distance2(here, g.pos) : kHearing * kHearing;
		const float* heard = nullptr;
		for (int i = 0; i < n; i++) {
			float d = Distance2(here, shots[i].pos);
			if (shots[i].who != unit && (everyone || shots[i].team != unit[g_vas->team]) && d < nearest && d > kArrive * kArrive) {
				nearest = d;
				heard = shots[i].pos;
			}
		}
		if (heard) {
			memcpy(g.pos, heard, sizeof(g.pos));
			g.has = g.noise = true;
			g.set = now;
			g.best = sqrtf(nearest);
			g.progress = now;
			if (BotLogOn()) {
				XLS_LOG_INFO("mods: bots: %p hears gunfire %.0f m away.", unit, g.best / 100.0f);
			}
		}
	}
	bool gap = now - g.used > 5000; // it was fighting or dead: its timers mean nothing
	g.used = now;
	if (gap) {
		g.progress = now;
		g.best = g.has ? sqrtf(Distance2(here, g.pos)) : 0;
	}
	bool renew = !g.has;
	if (g.has) {
		float d = sqrtf(Distance2(here, g.pos));
		if (d < kArrive || (g.noise && now - g.set > kNoiseLasts)) {
			if (g.noise) {
				g.rest = now + kNoiseRest; // it's seen what there was to see
			}
			renew = true;
		}
		else if (d < g.best - kProgress) {
			g.best = d;
			g.progress = now;
		}
		else if (now - g.progress > kStuckCheck) {
			renew = true;
			if (BotLogOn()) {
				XLS_LOG_INFO("mods: bots: %p gave up on a goal %.0f m away.", unit, d / 100.0f);
			}
		}
	}
	if (renew) {
		g.has = false;
		g.noise = false;
		if (RoamPoint(unit, here, g)) {
			g.set = g.progress = now;
			g.best = sqrtf(Distance2(here, g.pos));
			if (BotLogOn()) {
				XLS_LOG_INFO("mods: bots: %p roams to a point %.0f m away.", unit, g.best / 100.0f);
			}
		}
	}
}

char __attribute__((thiscall)) Resolve(void* self, uint8_t* unit, uint8_t* plan, void* ctx, uint32_t target, float* out, void* info)
{
	const uint8_t type = target & 0xff;
	if ((type != kTgtPost && type != kTgtRoam) || !unit || !plan) {
		return g_resolveOriginal(self, unit, plan, ctx, target, out, info);
	}
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	if (!BotWorld(game) || !BotMenuOptions().improved) {
		return g_resolveOriginal(self, unit, plan, ctx, target, out, info);
	}
	DWORD now = GetTickCount();
	std::lock_guard<std::mutex> lock(g_goalLock);
	if (g_goals.size() > 128) {
		g_goals.clear();
	}
	RoamGoal& g = g_goals[unit];
	GoalUpdate(unit, g, now);
	if (type == kTgtPost) {
		uint8_t* post = *(uint8_t**)(plan + kPlanPost);
		if (!post) {
			return 0;
		}
		for (int s = 0; s < 16; s++) { // the bot's own slot: whichever slot's think has this plan
			if (g_postDone[s].post == post && PostRecentlyDone(s, post) && g_postUnits[s] == unit) {
				return 0;
			}
		}
		bool here = Distance2((const float*)(unit + 0xd0), (const float*)(post + 0xd0)) < kPostHere * kPostHere;
		if (g.has && g.noise && !here && GameKind() != kKindPostGrab) {
			return 0;
		}
		return g_resolveOriginal(self, unit, plan, ctx, target, out, info);
	}
	if (!g.has) {
		return g_resolveOriginal(self, unit, plan, ctx, target, out, info);
	}
	AreasBuild();
	const float* here = (const float*)(unit + 0xd0);
	int hopFrom, hopTo;
	const float* to = Waypoint(here, g.pos, &hopFrom, &hopTo);
	float left = sqrtf(Distance2(here, to));
	if (hopFrom != g.hopFrom || hopTo != g.hopTo || now - g.hopProgress > 2 * kHopStuck) { // a new hop (or a stale one)
		g.hopFrom = hopFrom;
		g.hopTo = hopTo;
		g.hopBest = left;
		g.hopProgress = now;
	}
	else if (left < g.hopBest - kProgress) {
		g.hopBest = left;
		g.hopProgress = now;
	}
	else if (hopFrom >= 0 && now - g.hopProgress > kHopStuck) { // no closer in a while: that link doesn't work for now
		g_blocked[(int64_t)hopFrom * 1024 + hopTo] = now;
		g.hopProgress = now;
		if (BotLogOn()) {
			XLS_LOG_INFO("mods: bots: %p stuck going from area %d to %d (%.0f m left): link blocked for a while.", unit, hopFrom, hopTo, left / 100.0f);
		}
	}
	out[0] = to[0];
	out[1] = to[1];
	out[2] = to[2];
	out[3] = 0.0f;
	static LONG logged = 0;
	if (InterlockedIncrement(&logged) <= 3) {
		XLS_LOG_INFO("mods: bots: roam goal in use (%.0f, %.0f, %.0f).", g.pos[0], g.pos[1], g.pos[2]);
	}
	return 1;
}

uint32_t __attribute__((thiscall)) ThinkBody(uint8_t* think, uint8_t* unit)
{
	ReactToHit(think, unit);
	return g_thinkBodyOriginal(think, unit);
}


}

bool BotsBuildBrains(std::vector<ArcEntry>* entries)
{
	const ArcEntry* stock = nullptr;
	for (const ArcEntry& e : *entries) {
		if (_stricmp(e.name.c_str(), kStockBrain) == 0) {
			stock = &e;
		}
		else if (e.name.size() > strlen(kStockBrain) && _strnicmp(e.name.c_str(), kStockBrain, strlen(kStockBrain)) == 0) {
			XLS_LOG_INFO("mods: bots: uHm6d.arc already has its own brains, using those.");
			return false;
		}
	}
	if (!stock || stock->data.size() != kBrainStockSize || BrainCrc(stock->data) != kBrainStockCrc) {
		XLS_LOG_WARN("mods: bots: the stock brain isn't the one the Improved brains were made from: Stock brains only.");
		return false;
	}
	std::vector<ArcEntry> add;
	for (const EmbeddedBrain& brain : kEmbeddedBrains) {
		ArcEntry e = *stock;
		e.name = std::string(kStockBrain) + "_" + brain.mode;
		if (!BrainApply(stock->data, brain, &e.data)) {
			XLS_LOG_WARN("mods: bots: embedded brain %s failed its check: Stock brains only.", brain.mode);
			return false;
		}
		add.push_back(std::move(e));
	}
	entries->insert(entries->end(), add.begin(), add.end());
	return true;
}

void BotsInstall(unsigned char* base)
{
	g_base = base;
	for (const BotVas& v : kBotVas) {
		if (GameBuild() >= 0 && &v - kBotVas != GameBuild()) {
			continue;
		}
		g_vas = &v;
		if (memcmp(At(v.thinkSet), kThinkPrologue, sizeof(kThinkPrologue)) == 0 &&
		    memcmp(At(v.filterWeapon), kFilterPrologue, sizeof(kFilterPrologue)) == 0 &&
		    memcmp(At(v.evalParam), kEvalPrologue, sizeof(kEvalPrologue)) == 0) {
			break;
		}
		g_vas = nullptr;
	}
	if (!g_vas) {
		XLS_LOG_WARN("mods: bots: functions not recognised, bot brains not changed.");
		return;
	}
	g_brains = true; // built when the game opens uHm6d.arc (BotsBuildBrains), or an overlay's; a missing one falls back to stock in ThinkSet
	g_thinkOriginal = (ThinkSetFn)Detour(At(g_vas->thinkSet), kThinkPrologue, sizeof(kThinkPrologue), (void*)&ThinkSet);
	g_filterOriginal = (FilterFn)Detour(At(g_vas->filterWeapon), kFilterPrologue, sizeof(kFilterPrologue), (void*)&FilterWeapon);
	g_evalOriginal = (EvalFn)Detour(At(g_vas->evalParam), kEvalPrologue, sizeof(kEvalPrologue), (void*)&EvalParam);
	static const uint8_t kThinkBodyPrologue[] = {0x55, 0x56, 0x8b, 0xf1, 0x8a, 0x86, 0x84, 0x00, 0x00, 0x00}; // push ebp; push esi; mov esi,ecx; mov al,[esi+0x84]
	g_thinkBodyOriginal = (ThinkFn)Detour(At(g_vas->think), kThinkBodyPrologue, sizeof(kThinkBodyPrologue), (void*)&ThinkBody);
	if (!g_thinkBodyOriginal) {
		XLS_LOG_WARN("mods: bots: think code not recognised, bots won't react to being shot.");
	}
	static const uint8_t kHmPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0};
	if (GameBuild() >= 0) {
		g_hmActionOriginal = (HmActionFn)Detour(At(kDodgeVas[GameBuild()]), kHmPrologue, sizeof(kHmPrologue), (void*)&HmAction);
	}
	if (!g_hmActionOriginal) {
		XLS_LOG_WARN("mods: bots: dodge code not recognised, dodges keep the game's direction.");
	}
	static const uint8_t kResolvePrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x81, 0xec, 0xd4, 0x01, 0x00, 0x00};
	g_resolveOriginal = (ResolveFn)Detour(At(g_vas->resolve), kResolvePrologue, sizeof(kResolvePrologue), (void*)&Resolve);
	if (!g_resolveOriginal) {
		XLS_LOG_WARN("mods: bots: target code not recognised, bots won't roam.");
	}
	g_seed = GetTickCount();
	XLS_LOG_INFO("mods: bots: installed (%s).", BuildName((int)(g_vas - kBotVas)));
}

}
