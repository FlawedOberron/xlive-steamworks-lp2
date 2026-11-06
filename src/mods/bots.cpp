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
};
const BotVas kBotVas[kBuildCount] = {
	{0xf8f060, 0x98fd90, 0x18520c0, 0x1688, 0x16a4, 0x992600, 0x2428, 0x93b1b0, 0x90ffb0, 0x1852850, 0x1194, 0x18b4, 0xce2, 0x900880, 0xd84}, // DX11
	{0xf7f0f0, 0x980410, 0x1842020, 0x1678, 0x1694, 0x982c80, 0x2418, 0x92b820, 0x900640, 0x18427b0, 0x1184, 0x18a4, 0xcd2, 0x8f0f20, 0xd74}, // DX9
	{0xf8f4f0, 0x98ff70, 0x18530c0, 0x1688, 0x16a4, 0x9927e0, 0x2428, 0x93b420, 0x910280, 0x1853850, 0x1194, 0x18b4, 0xce2, 0x900b60, 0xd84}, // DX11 1.1
	{0xf7f400, 0x9806e0, 0x1842020, 0x1678, 0x1694, 0x982f50, 0x2418, 0x92b9e0, 0x900800, 0x18427b0, 0x1184, 0x18a4, 0xcd2, 0x8f10d0, 0xd74}, // DX9 1.1
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

uint32_t __attribute__((thiscall)) ThinkSet(void* spec, const char* path)
{
	if (g_brains && BotMenuOptions().improved && path && strcmp(path, kStockBrain) == 0) {
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

float __attribute__((thiscall)) EvalParam(uint8_t* eval, uint8_t* unit, void* b, void* c)
{
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	if (!unit || unit[0x1f] != 1 || !game || game[kAreaCategory] != 0) {
		return g_evalOriginal(eval, unit, b, c);
	}
	LogAimValues();
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
void LogPlan(uint8_t slot, uint8_t* plan, uint8_t* unit, const uint8_t* p)
{
	static const bool enabled = [] {
		Bytes unused;
		return ReadWholeFile(Cfg().modCacheDir + L"bot-log.txt", &unused);
	}();
	static void* last[16];
	if (!enabled || !plan) {
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

void ReactToHit(uint8_t* think, uint8_t* unit)
{
	const uint8_t slot = think[kThinkSlot];
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	uint8_t* slots = *(uint8_t**)At(g_vas->slots);
	if (slot >= 16 || !unit || !game || !slots || game[kAreaCategory] != 0 || !slots[0x1b1 + slot * 0x7c] || !BotMenuOptions().improved) {
		return;
	}
	uint8_t* p = unit + g_vas->param;
	p[kOwnLife] = LifePercent(unit);
	p[kTargetLife] = LifePercent(*(uint8_t**)(unit + g_vas->target));
	uint8_t* plan = *(uint8_t**)(think + kThinkPlan);
	p[kPostFree] = plan && *(void**)(plan + kPlanPost) ? 100 : 0;
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
	g_seed = GetTickCount();
	XLS_LOG_INFO("mods: bots: installed (%s).", BuildName((int)(g_vas - kBotVas)));
}

}
