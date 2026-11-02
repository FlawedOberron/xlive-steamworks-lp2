#include "mods/lp2.h"
#include "mods/packages.h"

#include "core/log.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

// Map packages' damage volumes (map.json "damage_volumes"): damage over time inside boxes, separate for players and
// VS (% of full health per second, or a kill at once).
//
// We use the game's own damage over time, "slip damage" (fire grenades, campaign area 2-3): a character is set burning
// (DX9 FUN_009a95a0, DX11 0x9b8e20, thiscall(character, param no 0-2, attacker): friendly-fire check, the burning
// effect, flag +0xd70 0x4000000, timer +0x1660 = the param's duration), then every frame its tick (DX9 FUN_009a93c0)
// counts the timer down and, each param interval, damages it by the param's amount (FUN_009a7930, the game's damage:
// deaths, kill credit, network as for a fire grenade). The params come from one lookup (DX9 FUN_00862180, DX11
// 0x871820, stdcall(character, no) -> 0x14-byte entry {damage, radius, duration, interval, off} from the human / VS
// / akrid table by the character's type), called only by those two. We redirect both calls: for param 2 (AREA 2_3,
// not used on online maps) and a character inside one of our volumes, an entry made for it: its type's value of the
// volume, scaled by its own full health (+0x18c4 DX9 / +0x18d4 DX11). Every frame a character is in a volume we set it
// burning again (from the gravity hook's per-character update), so it burns while inside and a moment after.
// Timers are in the game's delta units (stage +0x14810, mTrueDeltaTime): their rate per second is measured.

namespace xls::mods {

namespace {

struct Build {
	uint32_t lookup, lookupCalls[2];
	uint32_t apply;
	uint32_t maxLife;   // full health in the character (float; health itself follows it, encoded)
	uint32_t stage;     // stage pointer (RVA): + 0x14810 = the frame's delta
	uint32_t damage, damageCall; // the game's damage (thiscall(character, life change ^ 0x80000000)), the tick's call
	uint32_t lifeKey;   // pointer (RVA): + 0x1f58 = the key health is XORed with
	uint32_t oldPos, oldWorldPos; // uCharacter mOldPos / mOldWorldPos (vf55 saves +0x30 / +0xd0 there each update)
	uint32_t setAngle, setAngleCall; // uCoord's set angle (thiscall(unit, float euler[3] radians)), the set spawner's call
};
const Build kBuilds[kBuildCount] = {
	{0x471820, {0x5b8d3d, 0x5b8ea5}, 0x5b8e20, 0x18d4, 0x14d08ac, 0x5b71b0, 0x5b8d5b, 0x1452850, 0xc20, 0xc70, 0x277b00, 0xb81695}, // LP2DX11.exe
	{0x462180, {0x5a94bd, 0x5a9625}, 0x5a95a0, 0x18c4, 0x14c080c, 0x5a7930, 0x5a94db, 0x14427b0, 0xc10, 0xc60, 0x26cad0, 0xb71805}, // LP2DX9.exe
	{0x471bc0, {0x5b905d, 0x5b91c5}, 0x5b9140, 0x18d4, 0x14d18ac, 0x5b74d0, 0x5b907b, 0x1453850, 0xc20, 0xc70, 0x277c70, 0xb81b25}, // LP2DX11.exe 1.1
	{0x462400, {0x5a960d, 0x5a9775}, 0x5a96f0, 0x18c4, 0x14c080c, 0x5a7a80, 0x5a962b, 0x14427b0, 0xc10, 0xc60, 0x26cce0, 0xb71b45}, // LP2DX9.exe 1.1
};
// After full health (uCharacterBase's "Life & Energy"): mLife (encoded) +4, mOldLife +8, mEnergyMax +0xc, mEnergy +0x10.
const uint32_t kLife = 4, kEnergyMax = 0xc, kEnergy = 0x10;
const uint32_t kSign = 0x80000000; // the damage call's key (.rdata, both builds): a damage is a negative life change
const uint32_t kSlot = 2;          // AREA 2_3
const uint32_t kEnvironment = 0x14; // attacker: none (the game's default for slip damage units)
const float kTick = 0.25f;         // seconds between hits
const float kLinger = 0.4f;        // seconds a character keeps burning after leaving

struct Entry {
	float damage, radius, duration, interval;
	uint32_t off;
};

typedef const uint8_t*(__stdcall* LookupFn)(uint8_t*, uint32_t);
typedef void(__fastcall* ApplyFn)(uint8_t*, void*, uint32_t, uint32_t);
typedef uint32_t(__fastcall* DamageFn)(uint8_t*, void*, uint32_t);
typedef void(__fastcall* SetAngleFn)(uint8_t*, void*, const float*);
LookupFn g_lookup = nullptr;
ApplyFn g_apply = nullptr;
DamageFn g_damage = nullptr;
SetAngleFn g_setAngle = nullptr;
uint8_t* g_base = nullptr;
const Build* g_build = nullptr;

// The map's volumes, set at area load, read on the game's threads: a fixed table, the count published last.
const int kMax = 256;   // terrain layers make one box per merged patch of cells
DamageVolume g_vol[kMax];
volatile LONG g_count = 0;
volatile LONG g_entered[kMax]; // logged once each

// Which volume each burning character is in (the lookup only gets the character).
const int kChars = 64;
struct Burning {
	uint8_t* ch;
	LONG volume;
};
Burning g_chars[kChars];
SRWLOCK g_lock = SRWLOCK_INIT;

// Entries handed out (read by the caller at once): a ring.
Entry g_ring[64];
volatile LONG g_next = 0;

// Delta units per second, measured on one character's updates (the game's delta is per frame).
CRITICAL_SECTION g_rateLock;
uint8_t* g_rateChar = nullptr;
LARGE_INTEGER g_rateLast = {};
double g_rateDelta = 0.0, g_rateSecs = 0.0;
volatile float g_rate = 30.0f;     // until measured: the game's 30 fps frames

bool IsPlayer(const uint8_t* ch)
{
	return ch[0x1f] == 1;   // 1 human, 2 VS, 3 akrid (FUN_009a1bf0's types; the tick's own checks)
}

float Value(const DamageVolume& v, const uint8_t* ch, bool* kill)
{
	bool player = IsPlayer(ch);
	*kill = player ? v.playerKill : v.vsKill;
	return player ? ScriptValue(v.sid, kSetDamagePlayer, v.player) : ScriptValue(v.sid, kSetDamageVs, v.vs);
}

LONG VolumeOf(uint8_t* ch)
{
	LONG v = -1;
	AcquireSRWLockShared(&g_lock);
	for (const Burning& b : g_chars) {
		if (b.ch == ch) {
			v = b.volume;
			break;
		}
	}
	ReleaseSRWLockShared(&g_lock);
	return v;
}

void SetVolume(uint8_t* ch, LONG volume)
{
	AcquireSRWLockExclusive(&g_lock);
	Burning* free = nullptr;
	for (Burning& b : g_chars) {
		if (b.ch == ch) {
			b.volume = volume;
			ReleaseSRWLockExclusive(&g_lock);
			return;
		}
		if (!free && !b.ch) {
			free = &b;
		}
	}
	if (!free) {   // full (characters gone): start over
		memset(g_chars, 0, sizeof(g_chars));
		free = &g_chars[0];
	}
	free->ch = ch;
	free->volume = volume;
	ReleaseSRWLockExclusive(&g_lock);
}

void Measure(uint8_t* ch)
{
	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	uint8_t* stage = *(uint8_t**)(g_base + g_build->stage);
	if (!stage) {
		return;
	}
	float delta = *(float*)(stage + 0x14810);
	EnterCriticalSection(&g_rateLock);
	double since = g_rateChar ? double(now.QuadPart - g_rateLast.QuadPart) / double(freq.QuadPart) : 0.0;
	if (!g_rateChar || since > 1.0) {   // the measured character is gone: take this one
		g_rateChar = ch;
		g_rateLast = now;
	}
	else if (ch == g_rateChar && since > 0.0) {
		g_rateDelta += delta;
		g_rateSecs += since;
		g_rateLast = now;
		if (g_rateSecs >= 2.0) {
			float r = float(g_rateDelta / g_rateSecs);
			if (r > 0.5f && r < 1000.0f) {
				if (fabsf(r - g_rate) > g_rate * 0.2f) {
					XLS_LOG_INFO("mods: damage: game time runs %.1f units per second.", r);
				}
				g_rate = r;
			}
			g_rateDelta = g_rateSecs = 0.0;
		}
	}
	LeaveCriticalSection(&g_rateLock);
}

const uint8_t* __stdcall Lookup(uint8_t* ch, uint32_t no)
{
	const uint8_t* stock = g_lookup(ch, no);
	if (!g_count || (no & 0xff) != kSlot || !ch) {
		return stock;
	}
	LONG v = VolumeOf(ch);
	if (v < 0 || v >= g_count) {
		return stock;
	}
	bool kill = false;
	float value = Value(g_vol[v], ch, &kill);
	float full = *(const float*)(ch + g_build->maxLife);
	if (!(full > 0.0f)) {
		full = 1000.0f;
	}
	float rate = g_rate;
	Entry* e = &g_ring[InterlockedIncrement(&g_next) & 63];
	e->damage = kill ? full * 4.0f : full * value / 100.0f * kTick;
	e->radius = stock ? ((const Entry*)stock)->radius : 0.0f;
	e->duration = kLinger * rate;
	e->interval = kTick * rate;
	e->off = 0;
	return (const uint8_t*)e;
}

bool IsCall(uint8_t* base, uint32_t rva, uint32_t fn)
{
	uint8_t* at = base + rva;
	return at[0] == 0xe8 && at + 5 + *(int32_t*)(at + 1) == base + fn;
}

bool WriteCall(uint8_t* at, void* to)
{
	int32_t rel = (int32_t)((uint8_t*)to - (at + 5));
	DWORD old = 0;
	if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	memcpy(at + 1, &rel, 4);
	VirtualProtect(at + 1, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), at, 5);
	return true;
}

}

void DamageInstall(unsigned char* base)
{
	InitializeCriticalSection(&g_rateLock);
	for (const Build& b : kBuilds) {
		if (GameBuild() >= 0 && &b - kBuilds != GameBuild()) {
			continue;
		}
		if (!IsCall(base, b.lookupCalls[0], b.lookup) || !IsCall(base, b.lookupCalls[1], b.lookup)) {
			continue;
		}
		g_base = base;
		g_build = &b;
		g_lookup = (LookupFn)(base + b.lookup);
		g_apply = (ApplyFn)(base + b.apply);
		for (uint32_t c : b.lookupCalls) {
			WriteCall(base + c, (void*)&Lookup);
		}
		if (IsCall(base, b.damageCall, b.damage)) {
			g_damage = (DamageFn)(base + b.damage);
		}
		else {
			XLS_LOG_WARN("mods: damage: the damage call not recognised: scripts cannot damage players.");
		}
		if (IsCall(base, b.setAngleCall, b.setAngle)) {
			g_setAngle = (SetAngleFn)(base + b.setAngle);
		}
		else {
			XLS_LOG_WARN("mods: damage: the set angle call not recognised: teleports keep the facing.");
		}
		XLS_LOG_INFO("mods: damage: installed (%s).", BuildName((int)(&b - kBuilds)));
		return;
	}
	XLS_LOG_WARN("mods: damage: calls not recognised, damage volumes ignored.");
}

void DamageSetMap(const MapPhysics* physics)
{
	g_count = 0;
	MemoryBarrier();
	AcquireSRWLockExclusive(&g_lock);
	memset(g_chars, 0, sizeof(g_chars));
	ReleaseSRWLockExclusive(&g_lock);
	LONG n = 0;
	if (physics && g_build) {
		for (const DamageVolume& v : physics->damage) {
			if (n == kMax) {
				XLS_LOG_WARN("mods: damage: more than %d damage volumes, the rest ignored.", kMax);
				break;
			}
			g_entered[n] = 0;
			g_vol[n++] = v;
		}
	}
	MemoryBarrier();
	g_count = n;
	if (n) {
		XLS_LOG_INFO("mods: damage: %ld damage volume(s).", n);
	}
}

void DamageCharacter(unsigned char* ch)
{
	LONG n = g_count;
	if (!n || !g_apply) {
		return;
	}
	Measure(ch);
	const float* pos = (const float*)(ch + 0x30);
	for (LONG i = 0; i < n; i++) {
		const DamageVolume& v = g_vol[i];
		bool kill = false;
		if ((!(Value(v, ch, &kill) > 0.0f) && !kill) || !ScriptVolumeOn(v.sid)) {
			continue;   // nothing for this kind of character, or switched off by a script
		}
		if (InBox(v.min, v.max, v.turn, pos)) {
			if (!g_entered[i] && InterlockedExchange(&g_entered[i], 1) == 0) {
				XLS_LOG_INFO("mods: damage: a %s entered damage volume %ld at %.0f %.0f %.0f.", IsPlayer(ch) ? "player" : "VS", i, pos[0], pos[1], pos[2]);
			}
			SetVolume(ch, i);
			g_apply(ch, nullptr, kSlot, kEnvironment);
			return;
		}
	}
}

bool CharacterStats(const unsigned char* ch, CharacterVitals* out)
{
	if (!ch || !g_build) {
		return false;
	}
	const uint8_t* key = *(uint8_t* const*)(g_base + g_build->lifeKey);
	if (!key) {
		return false;
	}
	uint32_t bits = *(const uint32_t*)(ch + g_build->maxLife + kLife) ^ *(const uint32_t*)(key + 0x1f58);
	memcpy(&out->life, &bits, 4);
	out->lifeMax = *(const float*)(ch + g_build->maxLife);
	out->energy = *(const float*)(ch + g_build->maxLife + kEnergy);
	out->energyMax = *(const float*)(ch + g_build->maxLife + kEnergyMax);
	return true;
}

void CharacterDamage(unsigned char* ch, float amount)
{
	if (!ch || !g_damage || !(amount > 0.0f)) {
		return;
	}
	uint32_t bits;
	float change = amount;   // the call takes the change XOR the sign bit: +amount reads as -amount
	memcpy(&bits, &change, 4);
	g_damage(ch, nullptr, bits ^ kSign);
}

void CharacterAddEnergy(unsigned char* ch, float amount)
{
	if (!ch || !g_build) {
		return;
	}
	float* energy = (float*)(ch + g_build->maxLife + kEnergy);
	float top = *(const float*)(ch + g_build->maxLife + kEnergyMax);
	float v = *energy + amount;
	v = v < 0.0f ? 0.0f : v;
	*energy = top > 0.0f && v > top ? top : v;
}

void CharacterTeleport(unsigned char* ch, const float pos[3], float yaw)
{
	// the position (+0x30), its world copy (+0xd0) and both "old" ones the move sweeps collision from: no drag back
	if (!ch || !g_build) {
		return;
	}
	for (uint32_t at : {0x30u, 0xd0u, g_build->oldPos, g_build->oldWorldPos}) {
		memcpy(ch + at, pos, 12);
	}
	if (g_setAngle && yaw == yaw) {   // NaN: keep its facing. As the set spawner turns a unit (record rot, degrees)
		float euler[4] = {0.0f, yaw * 3.14159265f / 180.0f, 0.0f, 0.0f};
		g_setAngle(ch, nullptr, euler);
	}
}

}
