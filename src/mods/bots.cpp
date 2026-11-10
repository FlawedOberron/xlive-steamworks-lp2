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
#include "mods/packages.h"
#include "mods/brains.h"
#include "mods/package.h"
#include "mods/parts.h"

#include "core/config.h"
#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

#include <algorithm>
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
// A bot's archetype belongs to its slot (the think's, +0x84), so it keeps it over respawns: every life is a new unit with
// stock traits. Per unit: the traits before and after scaling, so a new unit built at an old one's address (stock traits
// again) is scaled again, and one already scaled never twice.
struct Persona {
	int archetype = 0;
	uint8_t before[7] = {}, after[7] = {};
};
std::unordered_map<uint8_t*, Persona> g_personality; // unit -> what it was given
int g_slotArchetype[16] = {};           // archetype + 1 (0: none yet), reset at each area load
std::atomic<uint8_t*> g_slotUnit[16];   // the unit each slot's think last ran (ThinkBody)
std::mutex g_personalityLock;
const size_t kFilterCharacterSize = 0x16b0; // past the other weapon slot on every build
std::atomic<uint32_t> g_seed{0}; // AI runs on worker threads
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
	uint32_t seed = g_seed.load(std::memory_order_relaxed) * 1103515245u + 12345u;
	g_seed.store(seed, std::memory_order_relaxed);
	return seed >> 8;
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
	static const uint32_t kTraits[7] = {kAttacker, kCalm, kQuickness, kHold, kImpatient, kGrenade, kAppeal};
	std::lock_guard<std::mutex> lock(g_personalityLock); // bots first evaluate on several worker threads at once
	uint8_t* p = unit + g_vas->param;
	uint8_t now[7];
	for (int i = 0; i < 7; i++) {
		now[i] = p[kTraits[i]];
	}
	auto it = g_personality.find(unit);
	if (it != g_personality.end()) {
		const Persona& q = it->second;
		if (memcmp(now, q.before, 7) != 0 || memcmp(q.before, q.after, 7) == 0) {
			return q.archetype;
		}
		g_personality.erase(it); // stock traits again: another bot built where that one was
	}
	if (g_personality.size() > 256) {
		g_personality.clear(); // stale units
	}
	int slot = -1;
	for (int i = 0; i < 16; i++) {
		slot = g_slotUnit[i].load(std::memory_order_relaxed) == unit ? i : slot;
	}
	BotOptions o = BotMenuOptions();
	int a;
	if (o.personality > 0) {
		a = (o.personality - 1) % kArchetypeCount;
	}
	else if (slot >= 0 && g_slotArchetype[slot] > 0) {
		a = g_slotArchetype[slot] - 1;
	}
	else {
		a = (int)(Rand() % kArchetypeCount);
	}
	if (slot >= 0) {
		g_slotArchetype[slot] = a + 1;
	}
	Persona q;
	q.archetype = a;
	memcpy(q.before, now, 7);
	const Archetype& k = kArchetypes[a];
	// A player match's bots are built as campaign partners, and some come with a partner's attacker trait (10, the
	// battle bots' is 100): they'd hardly shoot. In a player match those start from the battle bots' value.
	const uint8_t* game = *(uint8_t**)At(g_vas->game);
	bool raised = game && game[0x256] && BotsInPlayerMatch() && p[kAttacker] < 50;
	if (raised) {
		p[kAttacker] = 100;
	}
	// difficulty on top of the archetype: fight, dodge, grenades and urgency up, waiting down (and the reverse)
	const float kDifficulty[] = {0.6f, 1.0f, 1.3f, 2.0f};
	float d = kDifficulty[o.difficulty & 3];
	Scale(p, kAttacker, k.attacker * d);
	Scale(p, kCalm, k.calm / d);
	Scale(p, kQuickness, k.quickness * d);
	Scale(p, kHold, k.hold);
	Scale(p, kImpatient, k.impatient * d);
	Scale(p, kGrenade, k.grenade * d);
	Scale(p, kAppeal, k.appeal);
	for (int i = 0; i < 7; i++) {
		q.after[i] = p[kTraits[i]];
	}
	g_personality[unit] = q;
	XLS_LOG_INFO("mods: bots: %p (slot %d) is a %s, difficulty x%.1f (attacker %u -> %u%s, quickness %u -> %u, appeal %u -> %u).", unit, slot,
	             k.name, d, q.before[0], q.after[0], raised ? ", raised from a partner's" : "", q.before[2], q.after[2], q.before[6], q.after[6]);
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

// pad buttons (+0x1c8): pick up 0x2000 (pad action 12), change weapon 0x8 (action 14); the plan's weapon target +0x88c
const uint32_t kPlanWeapon = 0x88c, kPadButtons = 0x1c8, kPadPickUp = 0x2000, kPadChange = 0x8;
const int kPadActionPickUp = 0xc;
bool SwapBeforePickUp(uint8_t* unit, uint8_t* plan);

uint32_t __attribute__((thiscall)) HmAction(uint8_t* move, uint8_t* unit, uint8_t* plan, uint8_t* pad)
{
	if (move && move[4] == kPadActionPickUp && unit && plan && pad && BotMenuOptions().improved) {
		uint32_t& buttons = *(uint32_t*)(pad + kPadButtons);
		bool pressed = buttons & kPadPickUp;
		uint32_t r = g_hmActionOriginal(move, unit, plan, pad);
		if (!pressed && (buttons & kPadPickUp) && SwapBeforePickUp(unit, plan)) {
			buttons = (buttons & ~kPadPickUp) | kPadChange;
			static DWORD logged = 0;
			if (BotLogOn() && GetTickCount() - logged > 3000) {
				logged = GetTickCount();
				XLS_LOG_INFO("mods: bots: %p switches to its worse weapon before picking up.", unit);
			}
		}
		return r;
	}
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
	// the link being crossed (zones), for ThinkBody's presses: kind (-1 none), its start / end, when Resolve last said so
	int linkKind = -1;
	float linkA[3] = {0, 0, 0}, linkB[3] = {0, 0, 0};
	DWORD linkSeen = 0;
	DWORD jumpAt = 0, grappleAt = 0, aimFrom = 0, unstickAt = 0; // last presses; when it started aiming the anchor
	DWORD steerAt = 0;      // Traverse steering it across a link: last frame it did, and whether it's past the start
	bool steerPast = false;
	float steerLink[3] = {0, 0, 0};
	uint32_t pathCalls = 0, pathOk = 0, pathArrived = 0; // the game's path step for it since the last trace line (PathStep)
	float pathRadius = 0, pathTo[3] = {0, 0, 0};
	DWORD traceAt = 0;      // the movement trace (Resolve): when it last logged, from where
	float tracePos[3] = {0, 0, 0};
	float grappleY = 0;     // height when it fired, to log whether it climbed
	bool grappleCheck = false;
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
const DWORD kSweptFor = 120000, kFightFor = 30000, kAvoidFor = 60000;
// A map package's zones (lp2navmap) replace the start point areas: links then have a kind and where they start and end.
struct Hop {
	int to;
	int kind; // NavKind
	float a[3], b[3];
	float length;
};
const bool kBotsGrapple = true; // grapple links: ThinkBody aims the bot's cursor at the link's end and presses the anchor (untested)
struct Area {
	float pos[3] = {0, 0, 0};
	std::vector<int> links; // areas a bot can run to in one go (the game's path gives up on long runs)
	std::vector<Hop> hops;  // zones: the links with how they're crossed
	DWORD swept[16] = {}, fight[16] = {};
	DWORD avoid[16] = {};   // per team: when one of its bots last gave up on a goal here (RoamPoint leaves it a while)
	float box[4] = {0, 0, 0, 0}; // zones: the outline's bounds, min x / z, max x / z
};
std::vector<Area> g_areas;
std::vector<const float*> g_points; // the start points (not base markers)
std::unordered_map<int64_t, DWORD> g_blocked; // link from * 4096 + to -> when a bot got stuck on it
NavZones g_nav;      // the map package's (under g_goalLock)
int g_navVersion = 0, g_navBuilt = -1;
std::unordered_map<int64_t, int> g_areaCells;

// Regions: the zones grouped into a few places worth a look (a field, a building, the tunnels), what roaming bots choose
// between; the zones stay for routing. Seeds spread over the zone graph about (the map's farthest graph distance / kRegionSpanShare) apart along it (farthest first, at most
// kRegionMax, then two rounds of re-centring), each zone joins the seed nearest along the graph, climbs counted triple,
// so tunnels under a field stay their own. A region whose biggest zone is most of it is open: a look from inside sweeps
// it. Others want a few of their zones visited (kRegionVisits) before the bot moves on.
const float kRegionSpanShare = 2.5f, kRegionSpanLeast = 2000.0f, kRegionClimb = 3.0f;
const int kRegionMax = 8, kRegionVisits = 3;
struct Region {
	int center = 0; // its middle zone
	std::vector<int> zones;
	bool open = false;
	DWORD swept[16] = {};
};
std::vector<Region> g_regions;
std::vector<int> g_zoneRegion; // per zone
LONG g_traceLines = 0;          // movement trace lines this map (Resolve)
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
float ZoneArea(int i)
{
	return std::max(g_nav.zones[i].size[0], 100.0f) * std::max(g_nav.zones[i].size[2], 100.0f);
}

// graph distance from the sources to every zone (links both ways: this is about what's near, not which way it goes)
std::vector<float> RegionReach(const std::vector<std::vector<std::pair<int, float>>>& edges, const std::vector<int>& sources)
{
	size_t n = edges.size();
	std::vector<float> d(n, 1e30f);
	std::vector<bool> done(n, false);
	for (int s : sources) {
		d[s] = 0;
	}
	for (;;) {
		int u = -1;
		for (size_t i = 0; i < n; i++) {
			if (!done[i] && d[i] < 1e29f && (u < 0 || d[i] < d[u])) {
				u = (int)i;
			}
		}
		if (u < 0) {
			return d;
		}
		done[u] = true;
		for (auto [v, w] : edges[u]) {
			d[v] = std::min(d[v], d[u] + w);
		}
	}
}

void RegionsBuild()
{
	g_regions.clear();
	g_zoneRegion.assign(g_areas.size(), -1);
	size_t n = g_areas.size();
	if (n == 0 || g_nav.zones.size() != n) {
		return;
	}
	std::vector<std::vector<std::pair<int, float>>> edges(n);
	for (const NavLink& l : g_nav.links) {
		const float* a = g_areas[l.from].pos;
		const float* b = g_areas[l.to].pos;
		float dx = a[0] - b[0], dz = a[2] - b[2];
		float w = sqrtf(dx * dx + dz * dz) + kRegionClimb * fabsf(a[1] - b[1]);
		edges[l.from].push_back({l.to, w});
		edges[l.to].push_back({l.from, w});
	}
	int first = 0; // the biggest zone
	for (size_t i = 1; i < n; i++) {
		first = ZoneArea((int)i) > ZoneArea(first) ? (int)i : first;
	}
	std::vector<int> seeds = {first};
	float span = 0; // from the map's size: Pirate Fortress ~85 m, a medium package ~35 m
	for (;;) {
		std::vector<float> d = RegionReach(edges, seeds);
		int farthest = -1;
		for (size_t i = 0; i < n; i++) {
			if (d[i] < 1e29f && (farthest < 0 || d[i] > d[farthest])) {
				farthest = (int)i;
			}
		}
		if (farthest >= 0 && span == 0) {
			span = std::max(kRegionSpanLeast, d[farthest] / kRegionSpanShare);
		}
		if (farthest < 0 || d[farthest] < span || (int)seeds.size() >= kRegionMax) {
			break;
		}
		seeds.push_back(farthest);
	}
	auto assign = [&]() {
		std::vector<std::vector<float>> reach;
		for (int s : seeds) {
			reach.push_back(RegionReach(edges, {s}));
		}
		for (size_t i = 0; i < n; i++) {
			int best = -1;
			float bestD = 1e30f;
			for (size_t k = 0; k < seeds.size(); k++) {
				float d = reach[k][i] < 1e29f ? reach[k][i] : 1e20f + Distance2(g_areas[i].pos, g_areas[seeds[k]].pos); // off the graph: nearest
				if (d < bestD) {
					bestD = d;
					best = (int)k;
				}
			}
			g_zoneRegion[i] = best;
		}
	};
	for (int round = 0; round < 3; round++) {
		assign();
		if (round == 2) {
			break;
		}
		for (size_t k = 0; k < seeds.size(); k++) { // re-centre: the region's zone nearest its area-weighted middle
			double x = 0, y = 0, z = 0, w = 0;
			for (size_t i = 0; i < n; i++) {
				if (g_zoneRegion[i] == (int)k) {
					double a = ZoneArea((int)i);
					x += a * g_areas[i].pos[0], y += a * g_areas[i].pos[1], z += a * g_areas[i].pos[2], w += a;
				}
			}
			if (w <= 0) {
				continue;
			}
			float mid[3] = {(float)(x / w), (float)(y / w), (float)(z / w)};
			int best = seeds[k];
			for (size_t i = 0; i < n; i++) {
				if (g_zoneRegion[i] == (int)k && LevelDistance2(g_areas[i].pos, mid) < LevelDistance2(g_areas[best].pos, mid)) {
					best = (int)i;
				}
			}
			seeds[k] = best;
		}
	}
	g_regions.resize(seeds.size());
	int open = 0;
	for (size_t k = 0; k < seeds.size(); k++) {
		Region& r = g_regions[k];
		r.center = seeds[k];
		float total = 0, biggest = 0;
		for (size_t i = 0; i < n; i++) {
			if (g_zoneRegion[i] == (int)k) {
				r.zones.push_back((int)i);
				total += ZoneArea((int)i);
				biggest = std::max(biggest, ZoneArea((int)i));
			}
		}
		r.open = r.zones.size() <= 2 || biggest >= 0.5f * total;
		open += r.open;
	}
	g_traceLines = 0;
	XLS_LOG_INFO("mods: bots: %u zones in %u regions (%d open: one look sweeps them).", (unsigned)n, (unsigned)g_regions.size(), open);
}

void AreasBuild()
{
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	uint8_t* set = game ? *(uint8_t**)(game + kInitPosSet) : nullptr;
	uint32_t count = set ? *(uint32_t*)(set + kSetCount) : 0;
	uint8_t** records = set ? *(uint8_t***)(set + kSetArray) : nullptr;
	if (set == g_areaSet && count == g_areaCount && g_navBuilt == g_navVersion) {
		return;
	}
	g_areaSet = set;
	g_areaCount = count;
	g_navBuilt = g_navVersion;
	g_areas.clear();
	g_areaCells.clear();
	if (!g_nav.zones.empty()) { // the package's zones
		g_points.clear();
		for (uint32_t i = 0; records && i < count && count <= 256; i++) {
			if (records[i] && records[i][kRecKind] <= 5) {
				g_points.push_back((const float*)(records[i] + kRecPos));
			}
		}
		for (const NavZone& z : g_nav.zones) {
			Area a;
			memcpy(a.pos, z.pos, sizeof(a.pos));
			a.box[0] = a.box[1] = 1e30f;
			a.box[2] = a.box[3] = -1e30f;
			for (const auto& loop : z.outline) {
				for (const auto& q : loop) {
					a.box[0] = std::min(a.box[0], q[0]), a.box[1] = std::min(a.box[1], q[2]);
					a.box[2] = std::max(a.box[2], q[0]), a.box[3] = std::max(a.box[3], q[2]);
				}
			}
			g_areas.push_back(a);
		}
		int kinds[5] = {};
		for (const NavLink& l : g_nav.links) {
			Hop h = {l.to, l.kind, {l.a[0], l.a[1], l.a[2]}, {l.b[0], l.b[1], l.b[2]}, l.length};
			g_areas[l.from].hops.push_back(h);
			if (std::find(g_areas[l.from].links.begin(), g_areas[l.from].links.end(), l.to) == g_areas[l.from].links.end()) {
				g_areas[l.from].links.push_back(l.to);
			}
			kinds[l.kind % 5]++;
		}
		g_blocked.clear();
		XLS_LOG_INFO("mods: bots: the map's %u zones, links: %d walk, %d jump, %d drop, %d grapple (%s), %d launch.", (unsigned)g_areas.size(),
		             kinds[kNavWalk], kinds[kNavJump], kinds[kNavDrop], kinds[kNavGrapple], kBotsGrapple ? "used within the anchor range" : "not used yet",
		             kinds[kNavLaunch]);
		RegionsBuild();
		return;
	}
	g_regions.clear();
	g_zoneRegion.clear();
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

// Zones: the zone whose outline (loops on the floor, even-odd) the point is in; on stacked floors the nearest in height.
int ZoneAt(const float* p)
{
	int best = -1;
	float bestUp = 1e30f;
	for (size_t i = 0; i < g_areas.size() && i < g_nav.zones.size(); i++) {
		const Area& a = g_areas[i];
		if (p[0] < a.box[0] || p[2] < a.box[1] || p[0] > a.box[2] || p[2] > a.box[3]) {
			continue;
		}
		const NavZone& z = g_nav.zones[i];
		float up = fabsf(p[1] - z.pos[1]);
		if (up > z.size[1] + 300.0f || up >= bestUp) {
			continue;
		}
		bool in = false;
		for (const auto& loop : z.outline) {
			for (size_t k = 0, j = loop.size() - 1; k < loop.size(); j = k++) {
				const auto& u = loop[k];
				const auto& v = loop[j];
				if ((u[2] > p[2]) != (v[2] > p[2]) && p[0] < (v[0] - u[0]) * (p[2] - u[2]) / (v[2] - u[2]) + u[0]) {
					in = !in;
				}
			}
		}
		if (in) {
			best = (int)i;
			bestUp = up;
		}
	}
	return best;
}

int AreaAt(const float* p)
{
	auto it = g_areaCells.find(AreaCell(p));
	if (it != g_areaCells.end()) {
		return it->second;
	}
	if (!g_nav.zones.empty()) {
		int zone = ZoneAt(p);
		if (zone >= 0) {
			return zone;
		}
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

// Zone routes: a link's cost is its run plus what crossing it takes (a jump or a drop a little, a launch less than its
// distance: it's quick, a grapple a lot: only where walking is much longer). Grapples beyond the match's anchor range
// aren't links at all. The point to run to: across a walk link, the next zone; else the link's start, then its end once
// there (a ledge to drop off, a launch volume to walk into: force.cpp throws the bot, a grapple's firing spot).
const float kHopAt = 600.0f;      // a link's start counts as reached this close
const float kLookAhead = 1200.0f; // route points closer than this are skipped (the game counts itself arrived early)
const float kJumpUp = 50.0f, kJumpGap = 150.0f; // a jump link needs a jump past these (else it's run across)

const float* ZoneWaypoint(const float* from, const float* goal, int start, int end, int* hopFrom, int* hopTo, const Hop** link)
{
	int n = (int)g_areas.size();
	DWORD now = GetTickCount();
	float range = RulesGrappleRange();
	std::vector<float> cost(n, 1e30f);
	std::vector<int> prev(n, -1);
	std::vector<const Hop*> via(n, nullptr);
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
		for (const Hop& h : g_areas[u].hops) {
			if (h.kind == kNavGrapple && (!kBotsGrapple || h.length > range)) {
				continue;
			}
			float run = sqrtf(Distance2(g_areas[u].pos, h.a)) + sqrtf(Distance2(h.b, g_areas[h.to].pos));
			float cross = h.kind == kNavWalk ? 0.0f : h.kind == kNavJump ? 300.0f : h.kind == kNavDrop ? 200.0f : h.kind == kNavLaunch ? 150.0f : 1500.0f;
			cross += h.kind == kNavLaunch ? sqrtf(Distance2(h.a, h.b)) * 0.3f : sqrtf(Distance2(h.a, h.b));
			auto it = g_blocked.find((int64_t)u * 4096 + h.to);
			bool blocked = it != g_blocked.end() && now - it->second < kBlockedFor;
			float c = cost[u] + run + cross + (blocked ? 1e6f : 0.0f);
			if (c < cost[h.to]) {
				cost[h.to] = c;
				prev[h.to] = u;
				via[h.to] = &h;
			}
		}
	}
	if (prev[end] < 0) {
		return goal;
	}
	// The game's move action stops a few metres short of the point it's given (bots stood 3 - 9 m from a hop for a
	// minute): route points within kLookAhead are skipped for the next one, a link's start counts as reached at kHopAt.
	std::vector<int> path;
	for (int at = end; at != start; at = prev[at]) {
		path.push_back(at);
	}
	std::reverse(path.begin(), path.end());
	int before = start;
	for (int hop : path) {
		const Hop* h = via[hop];
		*hopFrom = before;
		*hopTo = hop;
		*link = h;
		// walked: a walk link, a drop, a jump that needs none (the navmesh links those, lp2navgen.cross_links)
		bool run = !h || h->kind == kNavWalk || h->kind == kNavDrop;
		if (h && h->kind == kNavJump) {
			float dx = h->b[0] - h->a[0], dz = h->b[2] - h->a[2];
			run = h->b[1] - h->a[1] < kJumpUp && sqrtf(dx * dx + dz * dz) < kJumpGap;
		}
		if (run) {
			const float* point = hop == end ? goal : g_areas[hop].pos;
			if (hop == end || Distance2(from, point) > kLookAhead * kLookAhead) {
				return point;
			}
			before = hop;
			continue;
		}
		static DWORD logged = 0;
		if (GetTickCount() - logged > 3000) {
			logged = GetTickCount();
			static const char* const kKinds[] = {"walk", "jump", "drop", "grapple", "launch"};
			XLS_LOG_INFO("mods: bots: route from zone %d to %d by %s (%.0f m to its start).", before, hop, kKinds[h->kind % 5], sqrtf(Distance2(from, h->a)) / 100.0f);
		}
		return Distance2(from, h->a) > kHopAt * kHopAt ? h->a : h->b;
	}
	return goal;
}

// Caller holds g_goalLock. Where to run now for a goal: in the goal's area, the goal; else the next area on the shortest route
// over the links (Dijkstra; a blocked link costs a lot, so routes go round it while there's another way). -1 hop: the goal.
const float* Waypoint(const float* from, const float* goal, int* hopFrom, int* hopTo, const Hop** link)
{
	*hopFrom = *hopTo = -1;
	*link = nullptr;
	int start = AreaAt(from), end = AreaAt(goal), n = (int)g_areas.size();
	if (start < 0 || end < 0 || start == end || n > 4096) {
		return goal;
	}
	if (!g_nav.zones.empty()) {
		return ZoneWaypoint(from, goal, start, end, hopFrom, hopTo, link);
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
			auto it = g_blocked.find((int64_t)u * 4096 + v);
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
// A roaming bot (no target: it isn't fighting) marks its area swept as it goes, so gunfire in an area that a teammate
// roamed through after the shot was checked: nobody was found there, so it no longer draws the team in (noise goals and
// the fight pass both skip it). Fresh shots there count again.
const DWORD kCheckedAfter = 1000;
bool Checked(const Area& a, int team, DWORD shotAt)
{
	return a.swept[team] && (int)(a.swept[team] - shotAt) > (int)kCheckedAfter;
}

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
		// one that hasn't asked lately is fighting, dead or gone (its unit may be freed: not read)
		if (other != unit && og.has && !og.noise && nMates < 16 && now - og.used < 5000 && other[g_vas->team] == unit[g_vas->team]) {
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
	auto avoided = [&](const Area& a) { return a.avoid[team] && now - a.avoid[team] < kAvoidFor; }; // a bot gave up on a goal there
	int pick = -1;
	float bestScore = 1e30f;
	bool fight = false;
	for (size_t i = 0; i < g_areas.size(); i++) { // fresh fighting: metres away plus 2 per second old, a little chance
		const Area& a = g_areas[i];
		if ((int)i == here || !a.fight[team] || now - a.fight[team] > kFightFor || Checked(a, team, a.fight[team]) || avoided(a)) {
			continue;
		}
		float score = sqrtf(Distance2(from, a.pos)) / 100.0f + (now - a.fight[team]) / 500.0f + Rand() % 15;
		if (score < bestScore) {
			bestScore = score;
			pick = (int)i;
			fight = true;
		}
	}
	int region = -1;
	if (pick < 0 && !g_regions.empty()) {
		auto swept = [&](const Region& r) { return r.swept[team] && now - r.swept[team] < kSweptFor; };
		int hr = here >= 0 && here < (int)g_zoneRegion.size() ? g_zoneRegion[here] : -1;
		if (hr >= 0 && !swept(g_regions[hr])) { // in a region not looked over yet: one of its zones it hasn't been in
			for (int z : g_regions[hr].zones) {
				const Area& a = g_areas[z];
				if (z == here || (a.swept[team] && now - a.swept[team] < kSweptFor) || avoided(a)) {
					continue;
				}
				float score = (taken(a.pos) ? 100.0f : 0.0f) + Rand() % 50;
				if (score < bestScore) {
					bestScore = score;
					pick = z;
					region = hr;
				}
			}
		}
		for (int pass = 0; pass < 2 && pick < 0; pass++) { // the nearest region nobody looked over, nobody on the team headed for
			for (size_t k = 0; k < g_regions.size(); k++) {
				const Region& r = g_regions[k];
				if ((int)k == hr || swept(r) || avoided(g_areas[r.center])) {
					continue;
				}
				const float* c = g_areas[r.center].pos;
				float score = sqrtf(Distance2(from, c)) / 100.0f + (taken(c) ? 100.0f : 0.0f) + Rand() % 20;
				if (score < bestScore) {
					bestScore = score;
					pick = r.center;
					region = (int)k;
				}
			}
			if (pick < 0) {
				for (Region& r : g_regions) {
					r.swept[team] = 0;
				}
				if (BotLogOn()) {
					XLS_LOG_INFO("mods: bots: team %d looked over every region, starting over.", team);
				}
			}
		}
	}
	for (int pass = 0; pass < 2 && pick < 0; pass++) {
		for (size_t i = 0; i < g_areas.size(); i++) { // the nearest unswept area nobody on the team is headed for
			const Area& a = g_areas[i];
			if ((int)i == here || (a.swept[team] && now - a.swept[team] < kSweptFor) || avoided(a)) {
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
		for (Area& a : g_areas) { // nowhere left but places given up on: they count again
			a.avoid[team] = 0;
		}
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
		else if (region >= 0) {
			XLS_LOG_INFO("mods: bots: %p heads for zone %d of region %d.", unit, pick, region);
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
		if (at < (int)g_zoneRegion.size() && g_zoneRegion[at] >= 0) {
			Region& r = g_regions[g_zoneRegion[at]];
			int seen = 0;
			for (int z : r.zones) {
				seen += g_areas[z].swept[team] && now - g_areas[z].swept[team] < kSweptFor;
			}
			if ((r.open || seen >= std::min<int>(kRegionVisits, (int)r.zones.size())) && !(r.swept[team] && now - r.swept[team] < kSweptFor)) {
				r.swept[team] = now;
				if (BotLogOn()) {
					XLS_LOG_INFO("mods: bots: team %d swept region %d.", team, g_zoneRegion[at]);
				}
			}
		}
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
			int sa = AreaAt(shots[i].pos);
			if (sa >= 0 && Checked(g_areas[sa], unit[g_vas->team] & 15, shots[i].at)) {
				continue;   // a teammate (or itself) has been there since: nothing found
			}
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
				int na = AreaAt(g.pos);
				if (na >= 0) {
					g_areas[na].swept[unit[g_vas->team] & 15] = now;
				}
				if (BotLogOn()) {
					XLS_LOG_INFO("mods: bots: %p checked the noise: nobody, moving on.", unit);
				}
			}
			renew = true;
		}
		else if (d < g.best - kProgress) {
			g.best = d;
			g.progress = now;
		}
		else if (now - g.progress > kStuckCheck) {
			renew = true;
			int ga = AreaAt(g.pos); // not picked again for a while, by it or its team
			if (ga >= 0) {
				g_areas[ga].avoid[unit[g_vas->team] & 15] = now;
			}
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

// The game's path step (cAIPlanMoveTrace::vf7 -> DX11 FUN_00fb9070): thiscall(mover, unit, from, to, ?, arrival radius,
// float* direction out, ?), ret 0x1c; 0 when it doesn't move the bot (within the radius: "arrived", or no path). Counted
// per bot for the trace.
typedef uint32_t(__attribute__((thiscall)) * PathStepFn)(void*, uint8_t*, float*, float*, uint32_t, float, float*, int);
PathStepFn g_pathStepOriginal = nullptr;
const uint32_t kPathStepVas[kBuildCount] = {0xfb9070, 0xfa8fb0, 0xfb93e0, 0xfa9260};
const uint8_t kPathStepPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x64};

uint32_t __attribute__((thiscall)) PathStep(void* self, uint8_t* unit, float* from, float* to, uint32_t a5, float radius, float* out, int a8)
{
	uint32_t r = g_pathStepOriginal(self, unit, from, to, a5, radius, out, a8);
	if (unit && from && to && unit[0x1f] == 1 && g_traceLines < 150) { // only while the trace still logs
		std::lock_guard<std::mutex> lock(g_goalLock);
		auto it = g_goals.find(unit);
		if (it != g_goals.end()) {
			RoamGoal& g = it->second;
			g.pathCalls++;
			g.pathOk += r != 0;
			g.pathArrived += r == 0 && Distance2(from, to) <= radius * radius;
			g.pathRadius = radius;
			memcpy(g.pathTo, to, sizeof(g.pathTo));
		}
	}
	return r;
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
	const Hop* link = nullptr;
	const float* to = Waypoint(here, g.pos, &hopFrom, &hopTo, &link);
	g.linkKind = link ? link->kind : -1;
	if (link) {
		memcpy(g.linkA, link->a, sizeof(g.linkA));
		memcpy(g.linkB, link->b, sizeof(g.linkB));
	}
	g.linkSeen = now;
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
		g_blocked[(int64_t)hopFrom * 4096 + hopTo] = now;
		g.hopProgress = now;
		if (BotLogOn()) {
			XLS_LOG_INFO("mods: bots: %p stuck going from area %d to %d (%.0f m left): link blocked for a while.", unit, hopFrom, hopTo, left / 100.0f);
		}
	}
	if (now - g.traceAt >= 15000) { // how roaming goes: a line per bot every 15 s, the first 150 of a map
		if (g.traceAt && g_traceLines < 150) {
			g_traceLines++;
			int z = AreaAt(here), gz = AreaAt(g.pos);
			auto reg = [](int zone) { return zone >= 0 && zone < (int)g_zoneRegion.size() ? g_zoneRegion[zone] : -1; };
			XLS_LOG_INFO("mods: bots: trace %p moved %.1f m in %.0f s; zone %d (region %d) -> goal zone %d (region %d) %.0f m away; "
			             "hop %d -> %d kind %d, waypoint %.1f m away (%.0f %.0f %.0f)%s.", unit, sqrtf(Distance2(here, g.tracePos)) / 100.0f,
			             (now - g.traceAt) / 1000.0f, z, reg(z), gz, reg(gz), sqrtf(Distance2(here, g.pos)) / 100.0f, hopFrom, hopTo,
			             g.linkKind, left / 100.0f, to[0], to[1], to[2], now - g.steerAt < 15000 ? ", steered across a link" : "");
			XLS_LOG_INFO("mods: bots: trace %p path step: %u calls, %u went, %u arrived (radius %.1f m), last to (%.0f %.0f %.0f) %.1f m away.",
			             unit, g.pathCalls, g.pathOk, g.pathArrived, g.pathRadius / 100.0f, g.pathTo[0], g.pathTo[1], g.pathTo[2],
			             sqrtf(Distance2(here, g.pathTo)) / 100.0f);
		}
		g.traceAt = now;
		memcpy(g.tracePos, here, sizeof(g.tracePos));
		g.pathCalls = g.pathOk = g.pathArrived = 0;
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

// Crossing links. The think is the bot's pad: its body clears the buttons (+0x1c8) each frame, the plan presses them
// (cAIPlanMoveHmAction: action 7 jump 0x2, action 0x10 anchor 0x4), then the unit reads them; so presses added after
// the body count for that frame. Jump links: jump at the edge. Grapple links: at the firing spot the bot's cursor
// (think +0x1a8, cVirtualCursor mCursorPos +0x60 / mCursorTargetPos +0x70) is held on the link's end for a moment, then
// the anchor fires. Launch links need nothing (walking into the volume throws it). Stalled on any link: one jump.
const uint32_t kThinkCursor = 0x1a8, kCursorPos = 0x60, kCursorTarget = 0x70, kPadJump = 0x2, kPadAnchor = 0x4;
const float kJumpAt = 150.0f, kGrappleAt = 900.0f;
// Steering: the game's path ends where its navmesh does (a ledge, a gap), a few metres short, so within kSteerFrom of a
// link's start the bot's move direction (think +0x1e0, cThink::vf83: what cAIPlanMoveTrace sets each frame, a flat
// world offset) is pointed at the start, then from kSteerEdge on at the far end, across the edge (jump, drop, launch);
// a grapple only to its firing spot.
const uint32_t kThinkMoveDir = 0x1e0;
const float kSteerFrom = 700.0f, kSteerEdge = 120.0f;
const DWORD kLinkFresh = 700, kJumpRest = 900, kGrappleRest = 4000, kAimFor = 350, kUnstickAfter = 1800, kUnstickRest = 2500;

void Traverse(uint8_t* think, uint8_t* unit)
{
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	if (!unit || unit[0x1f] != 1 || !BotWorld(game) || !BotMenuOptions().improved || !Alive(unit)) {
		return;
	}
	const float* here = (const float*)(unit + 0xd0);
	DWORD now = GetTickCount();
	uint32_t press = 0;
	const char* what = nullptr;
	float logA = 0, logB = 0;
	bool steer = false;
	float steerDir[4] = {0, 0, 0, 0};
	{
		std::lock_guard<std::mutex> lock(g_goalLock);
		auto it = g_goals.find(unit);
		if (it == g_goals.end()) {
			return;
		}
		RoamGoal& g = it->second;
		if (g.grappleCheck && now - g.grappleAt > 1500) {
			g.grappleCheck = false;
			if (BotLogOn()) {
				XLS_LOG_INFO("mods: bots: %p grapple %s (%.1f m up).", unit, here[1] - g.grappleY > 150.0f ? "climbed" : "didn't climb",
				             (here[1] - g.grappleY) / 100.0f);
			}
		}
		if (now - g.linkSeen > kLinkFresh) {
			g.aimFrom = 0;
			return;
		}
		auto flat = [](const float* a, const float* b) {
			float dx = a[0] - b[0], dz = a[2] - b[2];
			return sqrtf(dx * dx + dz * dz);
		};
		if (g.linkKind >= kNavJump && g.linkKind <= kNavLaunch) {
			if (memcmp(g.steerLink, g.linkA, sizeof(g.steerLink)) != 0) {
				memcpy(g.steerLink, g.linkA, sizeof(g.steerLink));
				g.steerPast = false;
			}
			float da = flat(here, g.linkA);
			if (da < kSteerFrom) {
				g.steerPast = g.steerPast || (da < kSteerEdge && g.linkKind != kNavGrapple);
				const float* t = g.steerPast ? g.linkB : g.linkA;
				if (g.linkKind != kNavGrapple || da > kSteerEdge) {
					steer = true;
					steerDir[0] = t[0] - here[0];
					steerDir[2] = t[2] - here[2];
					g.steerAt = now;
				}
			}
		}
		// a jump only where it's needed: the far side more than a step up or a real gap across; anything else (a seam, a
		// short hop down) is run across, as a drop is
		bool seam = g.linkB[1] - g.linkA[1] < kJumpUp && flat(g.linkA, g.linkB) < kJumpGap;
		if (g.linkKind == kNavJump && !seam && flat(here, g.linkA) < kJumpAt && flat(here, g.linkB) > 60.0f && now - g.jumpAt > kJumpRest) {
			press = kPadJump;
			g.jumpAt = now;
			what = "jumps";
			logA = flat(here, g.linkB) / 100.0f;
			logB = (g.linkB[1] - here[1]) / 100.0f;
		}
		else if (g.linkKind == kNavGrapple && Distance2(here, g.linkA) < kGrappleAt * kGrappleAt && now - g.grappleAt > kGrappleRest) {
			if (!g.aimFrom) {
				g.aimFrom = now;
			}
			uint8_t* cursor = *(uint8_t**)(think + kThinkCursor);
			if (cursor) {
				float* pos = (float*)(cursor + kCursorPos);
				float* target = (float*)(cursor + kCursorTarget);
				for (int i = 0; i < 3; i++) {
					pos[i] = target[i] = g.linkB[i];
				}
			}
			if (now - g.aimFrom > kAimFor) {
				press = kPadAnchor;
				g.grappleAt = now;
				g.aimFrom = 0;
				g.grappleY = here[1];
				g.grappleCheck = true;
				what = "fires the anchor";
				logA = sqrtf(Distance2(here, g.linkB)) / 100.0f;
				logB = (g.linkB[1] - here[1]) / 100.0f;
			}
		}
		else {
			g.aimFrom = 0;
			if (g.hopFrom >= 0 && g.linkKind != kNavLaunch && now - g.hopProgress > kUnstickAfter && now - g.unstickAt > kUnstickRest &&
			    now - g.jumpAt > kJumpRest) {
				press = kPadJump;
				g.unstickAt = g.jumpAt = now;
				what = "jumps to get unstuck";
				logA = g.hopBest / 100.0f;
				logB = 0;
			}
		}
	}
	if (steer) {
		memcpy(think + kThinkMoveDir, steerDir, sizeof(steerDir));
	}
	if (!press) {
		return;
	}
	*(uint32_t*)(think + kPadButtons) |= press;
	static DWORD logged = 0;
	if (BotLogOn() && GetTickCount() - logged > 1000) {
		logged = GetTickCount();
		XLS_LOG_INFO("mods: bots: %p %s (%.1f m to go, %.1f m up).", unit, what, logA, logB);
	}
}

// Aim. The think's "aim at the target" command (cThink::vf31 0x6000000c -> DX11 FUN_00903f10, thiscall(think, unit,
// lead), ret 8) writes the point the bot aims at to the unit (+0x1600 DX11, +0x15f0 DX9): its target's position, led,
// plus a height by what the target is. The bot's view is then turned straight at it (cThinkPlayer::vf13 takes the pitch
// from it with no turn rate), and the game's own per-difficulty aim offsets are all 0: bots snap on and never miss.
// AI slots with the Improved brain aim at that point plus an error (per slot; a slot's think runs on one thread):
//  - tracking: the aim follows the point as a spring (omega, damping: under 1 it swings past), and only follows a
//    share of the target's movement at once (follow), so a moving target is trailed. A new target: the aim starts where
//    it was and swings over. Starting to aim after a second without: off by up to `acquire` (radians of the range).
//  - wander: a slow drift around the point, `wander` radians of the range, more while the target or the bot moves.
// By the AI Enemy Battle difficulty. Applied right after the game sets the point, and each think frame after that
// while nothing else has moved it (the game leaves it between commands).
typedef void(__attribute__((thiscall)) * AimFn)(uint8_t*, uint8_t*, int);
AimFn g_aimOriginal = nullptr;
const uint32_t kAimFnVas[kBuildCount] = {0x903f10, 0x8f45a0, 0x9041e0, 0x8f4760};
const uint8_t kAimPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x81, 0xec, 0x98, 0x00, 0x00, 0x00};
struct AimSkill {
	float follow, omega, damping, wander, acquire;
};
const AimSkill kAimSkill[4] = { // easy, normal, hard, veteran
	{0.50f, 5.0f, 0.45f, 0.035f, 0.25f},
	{0.70f, 8.0f, 0.60f, 0.020f, 0.16f},
	{0.85f, 12.0f, 0.80f, 0.011f, 0.09f},
	{0.95f, 18.0f, 1.00f, 0.005f, 0.04f},
};
const float kAimTargetSpeed = 400.0f, kAimOwnSpeed = 800.0f, kAimMovingMost = 4.0f; // cm/s that double the wander; its cap
struct AimState {
	uint8_t* unit = nullptr;
	uint8_t* target = nullptr;
	bool has = false, inBody = false; // the state below is set; the game set the point during this think frame
	double truthAt = 0, at = 0, wanderNext = 0;
	float truth[3] = {}, lastTruth[3] = {}, wrote[3] = {}, targetPos[3] = {}, ownPos[3] = {};
	float e[3] = {}, ev[3] = {};      // the tracking error and its speed
	float n[3] = {}, nGoal[3] = {};   // the wander
	float targetSpeed = 0, ownSpeed = 0;
};
AimState g_aim[16];

double Seconds()
{
	static const double kPerTick = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return 1.0 / (double)f.QuadPart;
	}();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * kPerTick;
}

uint32_t AimOut()
{
	return g_vas->held == 0x1688 ? 0x1600 : 0x15f0;
}

void AimApply(AimState& s, uint8_t* unit, uint8_t slot, double now)
{
	const AimSkill& k = kAimSkill[BotMenuOptions().difficulty & 3];
	float* aim = (float*)(unit + AimOut());
	const float* own = (const float*)(unit + 0xd0);
	uint8_t* target = *(uint8_t**)(unit + g_vas->target);
	float dt = (float)(now - s.at);
	float range = sqrtf(Distance2(s.truth, own));
	auto rnd = [] { return (int)(Rand() % 2001) / 1000.0f - 1.0f; };
	if (!s.has || dt > 1.0f || dt < 0.0f) { // starts aiming
		float size = std::min(range * k.acquire, 400.0f);
		s.e[0] = rnd() * size, s.e[1] = rnd() * size * 0.5f, s.e[2] = rnd() * size;
		memset(s.ev, 0, sizeof(s.ev));
		memset(s.n, 0, sizeof(s.n));
		memset(s.nGoal, 0, sizeof(s.nGoal));
		s.wanderNext = now;
		s.targetSpeed = s.ownSpeed = 0;
		dt = 0.0f;
	}
	else {
		float keep = target != s.target ? 1.0f : 1.0f - k.follow; // how much of the point's move the aim doesn't make
		for (int i = 0; i < 3; i++) {
			s.e[i] -= keep * (s.truth[i] - s.lastTruth[i]);
		}
		if (dt > 0.001f) {
			if (target && target == s.target) {
				float v = std::min(2000.0f, sqrtf(Distance2((const float*)(target + 0xd0), s.targetPos)) / dt);
				s.targetSpeed += (v - s.targetSpeed) * 0.25f;
			}
			float v = std::min(2000.0f, sqrtf(Distance2(own, s.ownPos)) / dt);
			s.ownSpeed += (v - s.ownSpeed) * 0.25f;
		}
		for (float left = std::min(dt, 0.25f); left > 0.0f;) {
			float h = std::min(left, 0.02f);
			left -= h;
			for (int i = 0; i < 3; i++) {
				s.ev[i] += (-k.omega * k.omega * s.e[i] - 2.0f * k.damping * k.omega * s.ev[i]) * h;
				s.e[i] += s.ev[i] * h;
			}
		}
	}
	float off = sqrtf(s.e[0] * s.e[0] + s.e[1] * s.e[1] + s.e[2] * s.e[2]);
	if (off > 3000.0f) {
		for (int i = 0; i < 3; i++) {
			s.e[i] *= 3000.0f / off;
		}
	}
	if (now >= s.wanderNext) {
		float size = range * k.wander * std::min(kAimMovingMost, 1.0f + s.targetSpeed / kAimTargetSpeed + s.ownSpeed / kAimOwnSpeed);
		s.nGoal[0] = rnd() * size, s.nGoal[1] = rnd() * size * 0.6f, s.nGoal[2] = rnd() * size;
		s.wanderNext = now + 0.3 + (Rand() % 400) / 1000.0;
	}
	float blend = 1.0f - expf(-dt / 0.15f);
	for (int i = 0; i < 3; i++) {
		s.n[i] += (s.nGoal[i] - s.n[i]) * blend;
		aim[i] = s.truth[i] + s.e[i] + s.n[i];
	}
	memcpy(s.wrote, aim, sizeof(s.wrote));
	memcpy(s.lastTruth, s.truth, sizeof(s.lastTruth));
	memcpy(s.ownPos, own, sizeof(s.ownPos));
	if (target) {
		memcpy(s.targetPos, target + 0xd0, sizeof(s.targetPos));
	}
	s.target = target;
	s.unit = unit;
	s.at = now;
	s.has = true;
	static DWORD logged = 0;
	static LONG lines = 0;
	if (GetTickCount() - logged > 2000 && lines < 40) { // a sample every 2 s, the first 40: enough to judge the numbers
		logged = GetTickCount();
		lines++;
		XLS_LOG_INFO("mods: bots: aim: slot %u aims %.0f cm off at %.1f m (target moving %.1f m/s, itself %.1f m/s).", slot,
		             sqrtf(Distance2(aim, s.truth)), range / 100.0f, s.targetSpeed / 100.0f, s.ownSpeed / 100.0f);
	}
}

void __attribute__((thiscall)) Aim(uint8_t* think, uint8_t* unit, int lead)
{
	g_aimOriginal(think, unit, lead);
	uint8_t slot = think ? think[kThinkSlot] : 16;
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	uint8_t* slots = *(uint8_t**)At(g_vas->slots);
	if (slot >= 16 || !unit || !slots || !BotWorld(game) || !slots[0x1b1 + slot * 0x7c] || !BotMenuOptions().improved) {
		return;
	}
	AimState& s = g_aim[slot];
	double now = Seconds();
	memcpy(s.truth, unit + AimOut(), sizeof(s.truth));
	s.truthAt = now;
	s.inBody = true;
	AimApply(s, unit, slot, now);
}

// After the think's body: a frame the game didn't set the point in, while it's still as we left it.
void AimKeep(uint8_t* think, uint8_t* unit)
{
	uint8_t slot = think[kThinkSlot];
	if (slot >= 16 || !unit || !g_aimOriginal) {
		return;
	}
	AimState& s = g_aim[slot];
	if (s.inBody) {
		s.inBody = false;
		return;
	}
	double now = Seconds();
	if (s.has && s.unit == unit && now - s.truthAt < 1.0 && memcmp(unit + AimOut(), s.wrote, sizeof(s.wrote)) == 0) {
		AimApply(s, unit, slot, now);
	}
}

uint32_t __attribute__((thiscall)) ThinkBody(uint8_t* think, uint8_t* unit)
{
	if (think[kThinkSlot] < 16 && unit) {
		g_slotUnit[think[kThinkSlot]].store(unit, std::memory_order_relaxed); // whose bot it is (Personality)
	}
	ReactToHit(think, unit);
	uint32_t r = g_thinkBodyOriginal(think, unit);
	AimKeep(think, unit);
	if (r) {
		Traverse(think, unit);
	}
	return r;
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


namespace {

// Weapon pickups (Improved brains). The AI's weapon target (plan +0x88c, resolved by target type 17) comes from the game's
// nearest-takeable-weapon search, cdecl (character, float* dist, 0, 0, 1, plan's skip list +0x5a8, its count +0x5a4)
// (DX11 FUN_009af7d0): nearest only, whatever it is. Its two AI callers (the plan's target update FUN_00917b50 and
// FUN_0091b520) call ours instead: the nearest few, each scored by how much better it is than the worse of the bot's two
// weapons (class value, personality, an empty one counts as nothing), minus a point a metre; none better: no target.
// Picking up drops the HELD weapon, so HmAction turns the pickup press into a weapon change while the held one is the better.
typedef uint8_t*(__cdecl* WeaponSearchFn)(uint8_t*, float*, uint32_t, uint32_t, uint32_t, uint8_t**, uint32_t);
WeaponSearchFn g_weaponSearch = nullptr;
struct WeaponVas {
	uint32_t search, call1, call2;
};
const WeaponVas kWeaponVas[kBuildCount] = {
	{0x9af7d0, 0x917bf1, 0x91b644}, // DX11
	{0x99fff0, 0x908261, 0x90bcb4}, // DX9
	{0x9afad0, 0x917dd1, 0x91b8d4}, // DX11 1.1
	{0x9a0180, 0x908371, 0x90be74}, // DX9 1.1
};

int WeaponKind(const uint8_t* w)
{
	return w ? w[0x1e] : -1;
}

// what a weapon is worth to this bot (0..~110): rockets and heavies, then shotguns, long, standard, support, handguns
float WeaponValue(const uint8_t* w, int archetype)
{
	if (!w) {
		return 0.0f;
	}
	enum { kStandard, kShort, kLong, kHeavy, kSupport, kHand, kOther };
	int k = WeaponKind(w), c;
	switch (k) {
	case 0x30: c = kStandard; break;
	case 0x31: case 0x05: case 0x44: c = kShort; break;
	case 0x32: case 0x02: case 0x42: c = kLong; break;
	case 0x33: case 0x04: case 0x41: case 0x43: case 0x45: case 0x46: case 0x47: c = kHeavy; break;
	case 0x34: c = kSupport; break;
	case 0x0c: c = kHand; break;
	case 0x01: case 0x03: case 0x06: case 0x07: c = kStandard; break;
	default: c = kOther; break;
	}
	static const float kBase[] = {60.0f, 78.0f, 68.0f, 88.0f, 50.0f, 20.0f, 40.0f};
	// aggressor, tactician, cautious, show-off, grenadier: standard, short, long, heavy, support, handgun, other
	static const float kLean[5][7] = {
		{1.0f, 1.25f, 0.8f, 1.1f, 0.8f, 1.0f, 1.0f},
		{1.1f, 1.0f, 1.15f, 1.0f, 1.0f, 1.0f, 1.0f},
		{1.0f, 0.85f, 1.25f, 0.95f, 1.35f, 1.0f, 1.0f},
		{0.95f, 1.1f, 1.0f, 1.2f, 0.9f, 1.0f, 1.0f},
		{1.0f, 1.0f, 0.9f, 1.25f, 1.0f, 1.0f, 1.0f},
	};
	float v = kBase[c] * (archetype >= 0 && archetype < 5 ? kLean[archetype][c] : 1.0f);
	return v;
}

// a carried weapon's worth: nothing once it's empty (rounds loaded and in reserve, the held weapon's fields)
float CarriedValue(const uint8_t* w, int archetype)
{
	if (!w) {
		return 0.0f;
	}
	const uint32_t d = g_vas->held == 0x1688 ? 0 : 0x10;
	int16_t loaded = *(const int16_t*)(w + 0x20fc - d), reserve = *(const int16_t*)(w + 0x20fe - d);
	return loaded <= 0 && reserve <= 0 ? 0.0f : WeaponValue(w, archetype);
}

bool IsGrenade(const uint8_t* w)
{
	int k = WeaponKind(w);
	return k >= 0x15 && k <= 0x19;
}

// grenades go in their own slot: taking another type drops the bot's own on the spot, which then looks like a pickup
// again (bots looped between the two). So a grenade is only worth a short detour, and once a bot has gone for one it
// leaves grenades alone for a while.
std::mutex g_grenadeLock;
std::unordered_map<uint8_t*, DWORD> g_grenadeAt; // character -> when it last went for grenades
const DWORD kGrenadeRest = 60000;
const float kGrenadeGain = 12.0f;

bool SameWeapon(const uint8_t* a, const uint8_t* b)
{
	return a && b && (*(const uint32_t*)(a + 0x1c) >> 8) == (*(const uint32_t*)(b + 0x1c) >> 8);
}

uint8_t* __cdecl WeaponSearch(uint8_t* ch, float* dist, uint32_t a, uint32_t b, uint32_t c, uint8_t** list, uint32_t count)
{
	uint8_t* nearest = g_weaponSearch(ch, dist, a, b, c, list, count); // with the plan's own list: it also clears taken ones
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	if (!nearest || !ch || ch[0x1f] != 1 || !BotWorld(game) || !BotMenuOptions().improved) {
		return nearest;
	}
	count &= 0xffff;
	const int kMax = 6;
	uint8_t* skip[64];
	uint32_t n = 0;
	for (uint32_t i = 0; list && i < count && n < 56; i++) {
		skip[n++] = list[i];
	}
	uint8_t* found[kMax];
	float away[kMax];
	int m = 0;
	found[m] = nearest;
	away[m++] = *dist;
	while (m < kMax) {
		skip[n++] = found[m - 1];
		float d = 0.0f;
		uint8_t* w = g_weaponSearch(ch, &d, a, b, c, skip, n);
		if (!w) {
			break;
		}
		found[m] = w;
		away[m++] = d;
	}
	int arch = Personality(ch);
	uint8_t* held = *(uint8_t**)(ch + g_vas->held);
	uint8_t* other = *(uint8_t**)(ch + g_vas->other);
	float worst = std::min(CarriedValue(held, arch), CarriedValue(other, arch));
	DWORD now = GetTickCount();
	bool grenadesRest;
	{
		std::lock_guard<std::mutex> lock(g_grenadeLock);
		auto it = g_grenadeAt.find(ch);
		grenadesRest = it != g_grenadeAt.end() && now - it->second < kGrenadeRest;
	}
	int best = -1;
	float bestScore = 0.0f, bestGain = 0.0f;
	for (int i = 0; i < m; i++) {
		float gain;
		if (IsGrenade(found[i])) {
			gain = grenadesRest ? -1.0f : kGrenadeGain;
		}
		else if (SameWeapon(found[i], held) || SameWeapon(found[i], other)) { // ammo for one it has: only when that one runs dry
			const uint8_t* mine = SameWeapon(found[i], held) ? held : other;
			gain = CarriedValue(mine, arch) <= 0.0f ? 30.0f : -1.0f;
		}
		else {
			gain = WeaponValue(found[i], arch) - worst;
		}
		float score = gain - away[i] / 100.0f;
		if (gain > 0.0f && (best < 0 || score > bestScore)) {
			best = i;
			bestScore = score;
			bestGain = gain;
		}
	}
	static DWORD logged = 0;
	if (BotLogOn() && GetTickCount() - logged > 3000) {
		logged = GetTickCount();
		XLS_LOG_INFO("mods: bots: %p weapons near: %d, carries %02x / %02x; %s %02x %.0f m away (gain %.0f), nearest %02x.", ch, m,
		             WeaponKind(held), WeaponKind(other), best >= 0 ? "goes for" : "none worth it, skips", best >= 0 ? WeaponKind(found[best]) : 0,
		             best >= 0 ? away[best] / 100.0f : 0.0f, bestGain, WeaponKind(nearest));
	}
	if (best < 0) {
		return nullptr;
	}
	if (IsGrenade(found[best])) {
		std::lock_guard<std::mutex> lock(g_grenadeLock);
		if (g_grenadeAt.size() > 256) {
			g_grenadeAt.clear();
		}
		g_grenadeAt[ch] = now;
	}
	*dist = away[best];
	return found[best];
}

bool PatchCall(uint8_t* at, uint8_t* expect, void* to)
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
	FlushInstructionCache(GetCurrentProcess(), at, 5);
	return true;
}

// the pickup press, when the bot holds the better of its two weapons: switch first so the worse one is dropped
bool SwapBeforePickUp(uint8_t* unit, uint8_t* plan)
{
	uint8_t* w = *(uint8_t**)(plan + kPlanWeapon);
	uint8_t* held = *(uint8_t**)(unit + g_vas->held);
	uint8_t* other = *(uint8_t**)(unit + g_vas->other);
	if (!w || IsGrenade(w) || !held || !other || SameWeapon(w, held) || SameWeapon(w, other) || WeaponKind(held) == 0x0c || WeaponKind(other) == 0x0c) {
		return false;
	}
	int arch = Personality(unit);
	return CarriedValue(held, arch) > CarriedValue(other, arch) + 2.0f;
}

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
	if (GameBuild() >= 0) {
		g_pathStepOriginal = (PathStepFn)Detour(At(kPathStepVas[GameBuild()]), kPathStepPrologue, sizeof(kPathStepPrologue), (void*)&PathStep);
	}
	if (!g_pathStepOriginal) {
		XLS_LOG_WARN("mods: bots: path step not recognised, the trace won't show it.");
	}
	if (GameBuild() >= 0) {
		g_aimOriginal = (AimFn)Detour(At(kAimFnVas[GameBuild()]), kAimPrologue, sizeof(kAimPrologue), (void*)&Aim);
	}
	if (!g_aimOriginal) {
		XLS_LOG_WARN("mods: bots: aim code not recognised, bots keep the game's perfect aim.");
	}
	if (GameBuild() >= 0) {
		const WeaponVas& w = kWeaponVas[GameBuild()];
		static const uint8_t kSearchPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x14, 0x53, 0x56, 0x57};
		if (memcmp(At(w.search), kSearchPrologue, sizeof(kSearchPrologue)) == 0) {
			g_weaponSearch = (WeaponSearchFn)At(w.search);
			bool one = PatchCall(At(w.call1), At(w.search), (void*)&WeaponSearch);
			bool two = PatchCall(At(w.call2), At(w.search), (void*)&WeaponSearch);
			XLS_LOG_INFO("mods: bots: weapon pickups ranked (%s%s).", one ? "plan" : "", two ? (one ? " + target" : "target") : "");
		}
		else {
			XLS_LOG_WARN("mods: bots: weapon search not recognised, bots take the nearest weapon.");
		}
	}
	g_seed = GetTickCount();
	XLS_LOG_INFO("mods: bots: installed (%s).", BuildName((int)(g_vas - kBotVas)));
}

void BotsSetMap(const MapPhysics* physics)
{
	{
		std::lock_guard<std::mutex> lock(g_personalityLock); // a new match: its units and slots are new
		g_personality.clear();
		memset(g_slotArchetype, 0, sizeof(g_slotArchetype));
	}
	std::lock_guard<std::mutex> lock(g_goalLock);
	g_goals.clear();
	g_nav = physics ? physics->nav : NavZones();
	g_navVersion++;
	if (!g_nav.zones.empty()) {
		XLS_LOG_INFO("mods: bots: map package has %u bot zones, %u links.", (unsigned)g_nav.zones.size(), (unsigned)g_nav.links.size());
	}
}

}
