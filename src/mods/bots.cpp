// Bot brains (lp2-tools/notes/bot-match-plan.md). Battle bots (uHm26 and its variants) bind their plan, the
// rThinkPlanAI "tool\AI\Think\HumanBotBt" (autoArc\uHm6d.arc), as each one spawns: uHm26::vf41 -> the
// character spec's think setter (DX9 FUN_00f7f0f0, thiscall(spec, path): resource manager get + spec->unit
// +0xfc). With an overlay of uHm6d.arc in Mods (lp2-tools lp2brain.py builds one) we hand it the brain for the
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
#include "mods/parts.h"

#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

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
};
const BotVas kBotVas[kBuildCount] = {
	{0xf8f060, 0x98fd90, 0x18520c0, 0x1688, 0x16a4, 0x992600, 0x2428}, // DX11
	{0xf7f0f0, 0x980410, 0x1842020, 0x1678, 0x1694, 0x982c80, 0x2418}, // DX9
	{0xf8f4f0, 0x98ff70, 0x18530c0, 0x1688, 0x16a4, 0x9927e0, 0x2428}, // DX11 1.1
	{0xf7f400, 0x9806e0, 0x1842020, 0x1678, 0x1694, 0x982f50, 0x2418}, // DX9 1.1
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
	const float kDifficulty[] = {0.6f, 1.0f, 1.3f, 1.6f};
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

float __attribute__((thiscall)) EvalParam(uint8_t* eval, uint8_t* unit, void* b, void* c)
{
	uint8_t* game = *(uint8_t**)At(g_vas->game);
	if (!unit || unit[0x1f] != 1 || !game || game[kAreaCategory] != 0) {
		return g_evalOriginal(eval, unit, b, c);
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
	g_brains = OverlayRedirect(ExeDirectory() + L"nativePC\\arcLp2Xml\\autoArc\\uHm6d.arc") != nullptr;
	g_thinkOriginal = (ThinkSetFn)Detour(At(g_vas->thinkSet), kThinkPrologue, sizeof(kThinkPrologue), (void*)&ThinkSet);
	g_filterOriginal = (FilterFn)Detour(At(g_vas->filterWeapon), kFilterPrologue, sizeof(kFilterPrologue), (void*)&FilterWeapon);
	g_evalOriginal = (EvalFn)Detour(At(g_vas->evalParam), kEvalPrologue, sizeof(kEvalPrologue), (void*)&EvalParam);
	g_seed = GetTickCount();
	XLS_LOG_INFO("mods: bots: installed (%s), per-mode brains %s.", BuildName((int)(g_vas - kBotVas)),
	             g_brains ? "on (uHm6d.arc overlaid)" : "off (no uHm6d.arc overlay)");
}

}
