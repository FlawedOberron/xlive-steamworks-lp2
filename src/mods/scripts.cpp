#include "mods/lp2.h"
#include "mods/packages.h"

#include "api/xsession.h"
#include "core/json.h"
#include "core/log.h"
#include "core/steam.h"

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// Map packages' visual scripts (map.json "script", "script_volumes"; the editor's scripts.gd): a node graph run during
// the match. Pulses go from an output port to the input ports linked to it; a pulse chain is cut after kSteps nodes
// (a loop). Variables are numbers (on / off = 1 / 0), players (a match slot, -1 nobody), lists of players, markers,
// lists of markers or vectors (x, y, z cm and a yaw in degrees, NaN: no facing: a teleport keeps the player's); node
// values ("operands") are a number or a variable's value. Its actions switch our volumes' effects and change their
// settings (gravity, damage, post-process: each module asks ScriptVolumeOn / ScriptValue), and damage players or
// change their T-Eng (damage.cpp).
//
// Host-authoritative. In a lobby only its owner runs the graph; the others follow its state. Triggers come from the
// machine that sees them best: each player's own machine reports its own character going into / out of the script's
// volumes (by match slot, RulesSlotOf), the host sees the AI's and its own. Messages on their own Steam channel
// (kChannel), reliable:
//   trigger  {type 1, u32 key, u8 slot, u8 who, u16 sid, u8 entered}                    a player -> the host
//   vitals   {type 3, u32 key, u8 slot, f32 life, life max, T-Eng, T-Eng max}           a player -> the host, 4 / s
//   act      {type 4, u32 key, u8 slot, u8 action, f32 amount, f32 pos[3]}              the host -> every member
//   spawn    {type 5, u32 key, u8 kind, u16 item, f32 pos[3], f32 yaw}                  the host -> every member
// A player's health and T-Eng are its own machine's: it reports them, and applies the actions on its own slot (from
// its character's update). Offline, or the host's own slot, directly. Spawns: the host rolls the chance, the item and
// the point; every machine makes the object (spawn.cpp) on its next character update.
//   state    {type 2, u32 key, u16 volumes, volumes x u8 on, u16 n, n x {u16 sid, u8 setting, f32 from, f32 to,
//             f32 elapsed, f32 fade}}                                                    the host -> every member
// The host sends its state on a change and every 2 s (late joiners); a state for another map (key) is ignored.
// Offline, everything here.
// Not yet: buttons, movers, messages on screen: they need the game's gimmicks / HUD decoded.

namespace xls::mods {

namespace {

const int kMaxVolumes = 256;
const int kSteps = 1000;             // nodes one pulse chain may run through (for each: lists of 16 players)
const int kChannel = 0x10002;        // after net.cpp's control (0x10000) and transfer.cpp's (0x10001)
enum : uint8_t { kTrigger = 1, kState = 2, kVitals = 3, kAct = 4, kSpawn = 5, kMode = 6 };
const int kSlots = 16;

struct Delay {
	double at;
	int node;
};

struct Report {
	int slot, who, sid;
	bool entered;
};

struct Act {
	int slot, action;
	float amount;
	float pos[3];   // teleport
};

struct Spawn {
	int kind, item;
	float pos[3], yaw;
};

struct Vitals {
	CharacterVitals v;
	double at = 0.0;     // 0: never seen
	bool owner = false;  // from the player's own machine
};

struct State {
	ScriptGraph graph;
	std::vector<float> vars;
	std::vector<std::vector<int>> lists; // list variables: match slots
	std::vector<std::array<float, 4>> vecs; // vector variables: x, y, z, yaw
	std::vector<double> timerAt;   // per node: when a running timer fires next (0 = stopped)
	std::vector<uint8_t> flip;     // flip-flop: next output; gate: open; once: fired
	std::vector<double> lastAt;    // cooldown: when it last passed (0: never)
	std::vector<int> count;        // counter
	std::vector<Delay> delays;
	std::unordered_map<uint8_t*, std::vector<uint8_t>> inside; // character -> per script volume: in it
	std::unordered_map<uint8_t*, double> seen;
	std::deque<Report> reports;    // host: players' triggers to run; a player: ours to send
	bool started = false;
	double lastTick = 0.0;
	int steps = 0;
};

struct Value {
	bool set = false;
	float from = 0.0f, to = 0.0f, fade = 0.0f;
	double t0 = 0.0;
};

CRITICAL_SECTION g_lock;
bool g_lockReady = false;
State* g_state = nullptr;            // null: no script on this map
volatile LONG g_on[kMaxVolumes];     // read without the lock by the volume modules
volatile LONG g_active = 0;
volatile LONG g_follow = 0;          // a lobby member, not its owner: the owner runs the graph
volatile LONG g_key = 0;             // the map's graph hash (0: no script)
volatile LONG g_dirty = 0;           // host: state changed since sent
SRWLOCK g_valueLock = SRWLOCK_INIT;
Value g_values[kMaxVolumes][kSettings];
float g_base[kMaxVolumes][kSettings]; // the map's own settings (what a fade starts from)
std::mt19937 g_random;
// under g_lock
Vitals g_vitals[kSlots];             // host / offline: every player's, by slot
CharacterVitals g_self;              // a player: its own, to report
int g_selfSlot = -1;
std::deque<Act> g_local, g_send;     // actions to apply here (on the slot's character update) / to send (host)
std::deque<Spawn> g_spawnHere, g_spawnSend; // spawns to make here (on any character update) / to send (host)

// Game modes (<game>\Modes\<name>.lp2mode "compiled", picked by mode.txt): a second graph, run by the host (offline:
// here) on the game thread (ModeFrame). Its holds / eliminations / releases / texts go to the players as kMode messages.
enum : uint8_t { kModeHold = 0, kModeElim = 1, kModeRelease = 2, kModeText = 3, kModeMasks = 4 };
State* g_mode = nullptr;              // under g_lock
std::string g_modeName;
std::vector<ScriptMarker> g_mapMarkers; // the map's script markers (a mode's markers too)
struct ModeEv {
	int kind, a, b;
};
std::mutex g_modeEvLock;
std::vector<ModeEv> g_modeEvents;
int g_modeDeaths[kSlots] = {};        // this round
// the mode's score (Score node): what placement ranks by, apart from the game's own points (kills etc., which still
// feed the stock results and career points). Match and this round, per player and per team.
int g_modeScore[kSlots] = {}, g_modeRoundScore[kSlots] = {}, g_modeTeamScore[2] = {}, g_modeTeamRoundScore[2] = {};
int g_modeLives = 0;                  // respawn rules: deaths allowed per round (0: no limit)
bool g_modeManual = false;            // respawn rules: back only by Spawn player
struct ModeEnd {
	bool pending = false, match = false, team = false;
	int side = -1;                    // a slot or a team; -1 by score
};
ModeEnd g_modeEnd;
double g_modeLastFrame = 0.0;
std::deque<std::vector<uint8_t>> g_modeSend; // under g_lock: messages for the players
template <typename T>
void Put(std::vector<uint8_t>* m, T v);
template <typename T>
bool Take(const uint8_t*& p, const uint8_t* end, T* v);

double Now()
{
	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	return double(now.QuadPart) / double(freq.QuadPart);
}

float Evaluate(const Value& v, double now)
{
	float t = v.fade > 0.0f ? float((now - v.t0) / v.fade) : 1.0f;
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return v.from + (v.to - v.from) * t;
}

float Get(const State& s, const ScriptOperand& o)
{
	return o.var >= 0 ? (o.var < (int)s.vars.size() ? s.vars[o.var] : 0.0f) : o.num;
}

void SetVar(State& s, int var, float value)
{
	if (var >= 0 && var < (int)s.vars.size()) {
		s.vars[var] = value;
	}
}

int Kind(const State& s, int var)
{
	return var >= 0 && var < (int)s.graph.varKinds.size() ? s.graph.varKinds[var] : kVarNumber;
}

int PlayerIn(const State& s, int var)
{
	int slot = var >= 0 && var < (int)s.vars.size() ? (int)lrintf(s.vars[var]) : -1;
	return slot >= 0 && slot < kSlots ? slot : -1;
}

int MarkerIn(const State& s, int var)
{
	int k = var >= 0 && var < (int)s.vars.size() ? (int)lrintf(s.vars[var]) : -1;
	return k >= 0 && k < (int)s.graph.markers.size() ? k : -1;
}

// name against pattern: * any run, ? any one character (both lower case)
bool Wild(const char* name, const char* pattern)
{
	if (*pattern == '*') {
		return Wild(name, pattern + 1) || (*name && Wild(name + 1, pattern));
	}
	if (!*pattern) {
		return !*name;
	}
	return *name && (*pattern == '?' || *pattern == *name) && Wild(name + 1, pattern + 1);
}

std::vector<int> Markers(const State& s, const std::string& pattern)
{
	std::string p = pattern;
	for (char& c : p) {
		c = (char)tolower((unsigned char)c);
	}
	std::vector<int> out;
	for (size_t i = 0; i < s.graph.markers.size(); i++) {
		if (Wild(s.graph.markers[i].name.c_str(), p.c_str())) {
			out.push_back((int)i);
		}
	}
	return out;
}

}

int ScriptMarkerFind(const char* pattern, float (*out)[4], int max)
{
	State* st = g_state;
	if (!st || !pattern) {
		return 0;
	}
	int n = 0;
	for (int k : Markers(*st, pattern)) {
		if (n >= max) {
			break;
		}
		const ScriptMarker& m = st->graph.markers[k];
		out[n][0] = m.pos[0];
		out[n][1] = m.pos[1];
		out[n][2] = m.pos[2];
		out[n][3] = m.yaw;
		n++;
	}
	return n;
}

namespace {

// a list entry from its variable: a player's slot or a marker's number, by the variable's kind
int ItemIn(const State& s, int var)
{
	return Kind(s, var) == kVarMarker ? MarkerIn(s, var) : PlayerIn(s, var);
}

// the marker's floor centre, or anywhere on its box's floor (in its turned frame: lp2mapc.volume_corners' turn)
void MarkerPoint(const ScriptMarker& m, bool anywhere, float out[3])
{
	memcpy(out, m.pos, 12);
	if (anywhere) {
		float x = std::uniform_real_distribution<float>(-0.5f, 0.5f)(g_random) * m.size[0];
		float z = std::uniform_real_distribution<float>(-0.5f, 0.5f)(g_random) * m.size[2];
		float a = m.yaw * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
		out[0] += x * c + z * sn;
		out[2] += -x * sn + z * c;
	}
}

typedef std::array<float, 4> Vec;
const Vec kNoVec = {0.0f, 0.0f, 0.0f, NAN};

// a vector operand: its variable's, else a zero vector without facing
Vec VecOf(const State& s, const ScriptOperand& o)
{
	return o.var >= 0 && o.var < (int)s.vecs.size() && Kind(s, o.var) == kVarVector ? s.vecs[o.var] : kNoVec;
}

void SetVec(State& s, int var, const Vec& v)
{
	if (var >= 0 && var < (int)s.vecs.size()) {
		s.vecs[var] = v;
	}
}

// turned by yaw degrees about y (lp2mapc.volume_corners' turn: local x, z -> world)
void Turn(float yaw, float x, float z, float* ox, float* oz)
{
	float a = yaw * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
	*ox = x * c + z * sn;
	*oz = -x * sn + z * c;
}

// a player's position and facing (its quaternion's turn about y, +0x40 x y z w)
bool PlayerPose(int slot, Vec* out)
{
	const uint8_t* body = RulesBody(slot);
	if (!body) {
		return false;
	}
	const float* p = (const float*)(body + 0x30);
	const float* q = (const float*)(body + 0x40);
	*out = {p[0], p[1], p[2], 2.0f * atan2f(q[1], q[3]) * 180.0f / 3.14159265f};
	return true;
}

Vec VecMath(int op, const Vec& a, const Vec& bv, float b, const ScriptMarker* m, float* number)
{
	Vec r = a;
	switch (op) {
	case kVecAdd:
	case kVecSub: {
		float k = op == kVecAdd ? 1.0f : -1.0f;
		for (int i = 0; i < 3; i++) {
			r[i] = a[i] + k * bv[i];
		}
		r[3] = a[3] == a[3] ? a[3] : bv[3];
		break;
	}
	case kVecScale:
		for (int i = 0; i < 3; i++) {
			r[i] = a[i] * b;
		}
		break;
	case kVecRotate:
		Turn(b, a[0], a[2], &r[0], &r[2]);
		r[3] = a[3] + b;
		break;
	case kVecToLocal:   // where in the marker's frame (its floor centre, turned by its yaw)
		if (m) {
			float dx = a[0] - m->pos[0], dz = a[2] - m->pos[2];
			Turn(-m->yaw, dx, dz, &r[0], &r[2]);
			r[1] = a[1] - m->pos[1];
			r[3] = a[3] - m->yaw;
		}
		break;
	case kVecFromLocal:
		if (m) {
			Turn(m->yaw, a[0], a[2], &r[0], &r[2]);
			r[0] += m->pos[0];
			r[1] = a[1] + m->pos[1];
			r[2] += m->pos[2];
			r[3] = a[3] + m->yaw;
		}
		break;
	case kVecLength: *number = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); break;
	case kVecGetX: *number = a[0]; break;
	case kVecGetY: *number = a[1]; break;
	case kVecGetZ: *number = a[2]; break;
	case kVecGetYaw: *number = a[3] == a[3] ? a[3] : 0.0f; break;
	case kVecSetX: r[0] = b; break;
	case kVecSetY: r[1] = b; break;
	case kVecSetZ: r[2] = b; break;
	case kVecSetYaw: r[3] = b; break;
	}
	return r;
}

std::vector<int>* ListIn(State& s, int var)
{
	return var >= 0 && var < (int)s.lists.size() ? &s.lists[var] : nullptr;
}

// A player's vitals as last known (false: gone, or never seen)
bool VitalsOf(int slot, CharacterVitals* out, double now)
{
	if (slot < 0 || slot >= kSlots || !RulesBody(slot) || !g_vitals[slot].at || now - g_vitals[slot].at > 3.0) {
		return false;
	}
	*out = g_vitals[slot].v;
	return true;
}

bool Check(int slot, int check, float a, double now)
{
	CharacterVitals v;
	bool here = VitalsOf(slot, &v, now);
	bool dead = here && !(v.life > 0.0f);
	switch (check) {
	case kCheckGone: return !here;
	case kCheckDead: return dead;
	case kCheckGoneOrDead: return !here || dead;
	case kCheckAlive: return here && !dead;
	case kCheckHealthBelow: return here && v.lifeMax > 0.0f && v.life * 100.0f / v.lifeMax < a;
	case kCheckTengBelow: return here && v.energy < a;
	}
	return false;
}

float Stat(int slot, int stat, double now)
{
	CharacterVitals v;
	if (!VitalsOf(slot, &v, now)) {
		return 0.0f;
	}
	switch (stat) {
	case kStatHealth: return v.life;
	case kStatHealthPercent: return v.lifeMax > 0.0f ? v.life * 100.0f / v.lifeMax : 0.0f;
	case kStatHealthMax: return v.lifeMax;
	case kStatTeng: return v.energy;
	case kStatTengMax: return v.energyMax;
	}
	return 0.0f;
}

void PlayerAct(int slot, int action, float amount, const float* pos = nullptr)
{
	if (slot < 0 || slot >= kSlots || !RulesBody(slot)) {
		return;
	}
	// offline (every slot is here) or the host's own: applied here; the others' on their machines
	bool here = !SessionPresenceLobby().IsValid() || slot == RulesLocalSlot();
	Act a = {slot, action, amount, {0.0f, 0.0f, 0.0f}};
	if (pos) {
		memcpy(a.pos, pos, sizeof(a.pos));
	}
	(here ? g_local : g_send).push_back(a);
	XLS_LOG_INFO("mods: script: player %d: %s %g.", slot + 1, kScriptActions[action], amount);
}

// on the slot's own character update
void Apply(unsigned char* ch, const Act& a)
{
	CharacterVitals v;
	if (!CharacterStats(ch, &v)) {
		return;
	}
	switch (a.action) {
	case kActDamage: CharacterDamage(ch, a.amount); break;
	case kActDamagePercent: CharacterDamage(ch, v.lifeMax * a.amount / 100.0f); break;
	case kActKill: CharacterDamage(ch, (v.lifeMax > 0.0f ? v.lifeMax : 1000.0f) * 4.0f); break;
	case kActAddTeng: CharacterAddEnergy(ch, a.amount); break;
	case kActRemoveTeng: CharacterAddEnergy(ch, -a.amount); break;
	case kActTeleport: CharacterTeleport(ch, a.pos, a.amount); break;
	}
}

double Seconds(const ScriptNode& n)
{
	if (n.maxSeconds > n.seconds) {
		return std::uniform_real_distribution<double>(n.seconds, n.maxSeconds)(g_random);
	}
	return n.seconds;
}

float Math(int op, float a, float b)
{
	switch (op) {
	case kOpAdd: return a + b;
	case kOpSub: return a - b;
	case kOpMul: return a * b;
	case kOpDiv: return b != 0.0f ? a / b : 0.0f;
	case kOpMin: return a < b ? a : b;
	case kOpMax: return a > b ? a : b;
	case kOpMod: return b != 0.0f ? fmodf(a, b) : 0.0f;
	case kOpEq: return fabsf(a - b) < 1e-4f;
	case kOpNe: return fabsf(a - b) >= 1e-4f;
	case kOpLt: return a < b;
	case kOpLe: return a <= b;
	case kOpGt: return a > b;
	case kOpGe: return a >= b;
	}
	return 0.0f;
}

std::string Format(const State& s, const std::string& text)
{
	// {name}: the variable's value
	std::string out;
	for (size_t i = 0; i < text.size(); i++) {
		size_t end = text[i] == '{' ? text.find('}', i) : std::string::npos;
		int var = -1;
		if (end != std::string::npos) {
			std::string name = text.substr(i + 1, end - i - 1);
			for (char& c : name) {
				c = (char)tolower((unsigned char)c);
			}
			for (size_t k = 0; k < s.graph.varNames.size(); k++) {
				var = s.graph.varNames[k] == name ? (int)k : var;
			}
		}
		if (var >= 0 && var < (int)s.vars.size()) {
			char buf[64];
			int slot = PlayerIn(s, var);
			if (Kind(s, var) == kVarPlayer) {
				std::string name = slot >= 0 ? LivesName(slot) : std::string();
				if (!name.empty()) {
					snprintf(buf, sizeof(buf), "%s", name.c_str());
				}
				else {
					snprintf(buf, sizeof(buf), slot >= 0 ? "player %d" : "nobody", slot + 1);
				}
			}
			else if (Kind(s, var) == kVarList || Kind(s, var) == kVarMarkers) {
				snprintf(buf, sizeof(buf), Kind(s, var) == kVarList ? "%u player(s)" : "%u marker(s)", var < (int)s.lists.size() ? (unsigned)s.lists[var].size() : 0u);
			}
			else if (Kind(s, var) == kVarVector && var < (int)s.vecs.size()) {
				const Vec& v = s.vecs[var];
				if (v[3] == v[3]) {
					snprintf(buf, sizeof(buf), "(%.0f, %.0f, %.0f, facing %.0f)", v[0], v[1], v[2], v[3]);
				}
				else {
					snprintf(buf, sizeof(buf), "(%.0f, %.0f, %.0f)", v[0], v[1], v[2]);
				}
			}
			else if (Kind(s, var) == kVarMarker) {
				int k = MarkerIn(s, var);
				snprintf(buf, sizeof(buf), "%s", k >= 0 ? s.graph.markers[k].name.c_str() : "no marker");
			}
			else {
				snprintf(buf, sizeof(buf), "%g", s.vars[var]);
			}
			out += buf;
			i = end;
		}
		else {
			out += text[i];
		}
	}
	return out;
}

void Fire(State& s, int node, int in, double now);

// ---------------------------------------------------------------- game modes

void ModeSendSlot(uint8_t op, int slot, const float* pos = nullptr, float yaw = NAN)
{
	std::vector<uint8_t> m;
	Put<uint8_t>(&m, kMode);
	Put<uint8_t>(&m, op);
	Put<uint8_t>(&m, (uint8_t)slot);
	Put<uint8_t>(&m, pos ? 1 : 0);
	for (int i = 0; i < 3; i++) {
		Put<float>(&m, pos ? pos[i] : 0.0f);
	}
	Put<float>(&m, yaw);
	g_modeSend.push_back(std::move(m));
}

// showTo 0 everyone, 1 the slot, 2 the team
void ModeShowHere(const std::string& text, int style, uint32_t rgb, float seconds, int showTo, int who)
{
	int self = RulesLocalSlot();
	bool here = showTo == 0 || (showTo == 1 && who == self) || (showTo == 2 && self >= 0 && RulesTeamOf(self) == who);
	if (here) {
		LivesMessage(text.c_str(), style, rgb, seconds > 0.0f ? seconds : 3.0f);
	}
}

void ModeText(const std::string& text, int style, uint32_t rgb, float seconds, int showTo, int who)
{
	ModeShowHere(text, style, rgb, seconds, showTo, who);
	std::vector<uint8_t> m;
	Put<uint8_t>(&m, kMode);
	Put<uint8_t>(&m, kModeText);
	Put<uint8_t>(&m, (uint8_t)style);
	Put<uint32_t>(&m, rgb);
	Put<float>(&m, seconds);
	Put<uint8_t>(&m, (uint8_t)showTo);
	Put<int8_t>(&m, (int8_t)who);
	Put<uint16_t>(&m, (uint16_t)std::min<size_t>(text.size(), 400));
	m.insert(m.end(), text.begin(), text.begin() + std::min<size_t>(text.size(), 400));
	g_modeSend.push_back(std::move(m));
}

void ModeHold(int slot, bool eliminate)
{
	if (eliminate) {
		RulesEliminate(slot, true);
	}
	else {
		RulesHold(slot, true);
	}
	ModeSendSlot(eliminate ? kModeElim : kModeHold, slot);
}

void Out(State& s, int node, int port, double now)
{
	const ScriptNode& n = s.graph.nodes[node];
	for (const ScriptLink& l : n.out[port]) {
		if (l.node >= 0 && l.node < (int)s.graph.nodes.size()) {
			Fire(s, l.node, l.in, now);
		}
	}
}

void SetVolume(int sid, int value)
{
	if (sid < 0 || sid >= kMaxVolumes) {
		return;
	}
	LONG v = value == 2 ? !g_on[sid] : value;
	if (InterlockedExchange(&g_on[sid], v) != v) {
		InterlockedExchange(&g_dirty, 1);
	}
	XLS_LOG_INFO("mods: script: volume %d %s.", sid, v ? "on" : "off");
}

void SetSetting(int sid, int setting, float to, float fade, double now)
{
	if (sid < 0 || sid >= kMaxVolumes || setting < 0 || setting >= kSettings) {
		return;
	}
	AcquireSRWLockExclusive(&g_valueLock);
	Value& v = g_values[sid][setting];
	v.from = v.set ? Evaluate(v, now) : g_base[sid][setting];
	v.to = to;
	v.fade = fade;
	v.t0 = now;
	v.set = true;
	ReleaseSRWLockExclusive(&g_valueLock);
	InterlockedExchange(&g_dirty, 1);
	XLS_LOG_INFO("mods: script: volume %d %s -> %g over %g s.", sid, kScriptSettings[setting], to, fade);
}

void MapModeEvent(const std::string& name, int player, double now);

void Fire(State& s, int node, int in, double now)
{
	if (++s.steps > kSteps) {
		if (s.steps == kSteps + 1) {
			XLS_LOG_WARN("mods: script: a pulse ran through %d nodes (a loop?): cut.", kSteps);
		}
		return;
	}
	const ScriptNode& n = s.graph.nodes[node];
	switch (n.type) {
	case kScriptStart:
	case kScriptEnter:
		Out(s, node, 0, now);
		break;
	case kScriptTimer:
		s.timerAt[node] = in == 0 ? now + Seconds(n) : 0.0;
		break;
	case kScriptDelay:
		s.delays.push_back({now + Seconds(n), node});
		break;
	case kScriptFlipflop: {
		int k = s.flip[node];
		s.flip[node] = !k;
		Out(s, node, k, now);
		break;
	}
	case kScriptCounter:
		if (in == 1) {
			s.count[node] = 0;
		}
		else if (++s.count[node] >= n.target) {
			s.count[node] = 0;
			Out(s, node, 0, now);
		}
		break;
	case kScriptSet:
		if (n.var >= 0 && n.var < (int)s.vars.size()) {
			s.vars[n.var] = n.toggle ? (s.vars[n.var] != 0.0f ? 0.0f : 1.0f) : Get(s, n.a);
		}
		Out(s, node, 0, now);
		break;
	case kScriptIf:
		Out(s, node, n.var >= 0 && n.var < (int)s.vars.size() && s.vars[n.var] != 0.0f ? 0 : 1, now);
		break;
	case kScriptVolume:
		SetVolume(n.sid, in == 0 ? 1 : in == 1 ? 0 : 2);
		Out(s, node, 0, now);
		break;
	case kScriptLog:
		XLS_LOG_INFO("mods: script: %s", Format(s, n.text).c_str());
		Out(s, node, 0, now);
		break;
	case kScriptRandom: {
		float a = Get(s, n.a), b = Get(s, n.b);
		if (b < a) {
			float t = a;
			a = b;
			b = t;
		}
		float v;
		if (n.integer) {
			int lo = (int)ceilf(a), hi = (int)floorf(b);
			v = (float)std::uniform_int_distribution<int>(lo, hi >= lo ? hi : lo)(g_random);
		}
		else {
			v = b > a ? std::uniform_real_distribution<float>(a, b)(g_random) : a;
		}
		SetVar(s, n.var, v);
		Out(s, node, 0, now);
		break;
	}
	case kScriptMath:
		SetVar(s, n.var, Math(n.op, Get(s, n.a), Get(s, n.b)));
		Out(s, node, 0, now);
		break;
	case kScriptCompare:
		Out(s, node, Math(n.op, Get(s, n.a), Get(s, n.b)) != 0.0f ? 0 : 1, now);
		break;
	case kScriptSetValue:
		SetSetting(n.sid, n.setting, Get(s, n.a), n.seconds > 0.0f ? n.seconds : 0.0f, now);
		Out(s, node, 0, now);
		break;
	case kScriptPlayers:
		if (std::vector<int>* l = ListIn(s, n.list)) {
			l->clear();
			for (int slot = 0; slot < kSlots; slot++) {
				if (RulesBody(slot)) {
					l->push_back(slot);
				}
			}
		}
		Out(s, node, 0, now);
		break;
	case kScriptList:
		if (std::vector<int>* l = ListIn(s, n.list)) {
			int slot = ItemIn(s, n.player);
			auto at = std::find(l->begin(), l->end(), slot);
			if (in == 0 && slot >= 0 && at == l->end()) {
				l->push_back(slot);
			}
			else if (in == 1 && at != l->end()) {
				l->erase(at);
			}
			else if (in == 2) {
				l->clear();
			}
		}
		Out(s, node, 0, now);
		break;
	case kScriptListRemove:
		if (std::vector<int>* l = Kind(s, n.list) == kVarList ? ListIn(s, n.list) : nullptr) {   // players' lists
			float a = Get(s, n.a);
			l->erase(std::remove_if(l->begin(), l->end(), [&](int slot) { return Check(slot, n.check, a, now); }), l->end());
		}
		Out(s, node, 0, now);
		break;
	case kScriptListGet: {
		std::vector<int>* l = ListIn(s, n.list);
		int k = !l || l->empty() ? -1 : n.random ? std::uniform_int_distribution<int>(0, (int)l->size() - 1)(g_random) : (int)lrintf(Get(s, n.a)) - 1;
		bool found = l && k >= 0 && k < (int)l->size();
		SetVar(s, n.player, found ? (float)(*l)[k] : -1.0f);
		Out(s, node, found ? 0 : 1, now);
		break;
	}
	case kScriptListCount: {
		std::vector<int>* l = ListIn(s, n.list);
		SetVar(s, n.var, l ? (float)l->size() : 0.0f);
		Out(s, node, 0, now);
		break;
	}
	case kScriptForEach: {
		std::vector<int>* l = ListIn(s, n.list);
		std::vector<int> each = l ? *l : std::vector<int>();   // a copy: the chain may change the list
		for (int slot : each) {
			SetVar(s, n.player, (float)slot);
			Out(s, node, 0, now);
		}
		Out(s, node, 1, now);
		break;
	}
	case kScriptPlayerCheck:
		Out(s, node, Check(PlayerIn(s, n.player), n.check, Get(s, n.a), now) ? 0 : 1, now);
		break;
	case kScriptPlayerGet:
		SetVar(s, n.var, Stat(PlayerIn(s, n.player), n.stat, now));
		Out(s, node, 0, now);
		break;
	case kScriptPlayerAct:
		PlayerAct(PlayerIn(s, n.player), n.action, Get(s, n.a));
		Out(s, node, 0, now);
		break;
	case kScriptFindMarkers: {
		std::vector<int> found = Markers(s, n.text);
		if (std::vector<int>* l = ListIn(s, n.list)) {
			*l = found;
		}
		Out(s, node, found.empty() ? 1 : 0, now);
		break;
	}
	case kScriptGetMarker: {
		std::vector<int> found = Markers(s, n.text);
		int k = found.empty() ? -1 : n.random ? found[std::uniform_int_distribution<int>(0, (int)found.size() - 1)(g_random)] : found[0];
		SetVar(s, n.player, (float)k);
		Out(s, node, k >= 0 ? 0 : 1, now);
		break;
	}
	case kScriptSpawn: {
		int k = MarkerIn(s, n.player);
		float chance = Get(s, n.a);
		bool roll = chance >= 100.0f || std::uniform_real_distribution<float>(0.0f, 100.0f)(g_random) < chance;
		if (k < 0 || n.items.empty() || !roll) {
			Out(s, node, 1, now);
			break;
		}
		Spawn sp = {n.spawnKind, n.items[std::uniform_int_distribution<int>(0, (int)n.items.size() - 1)(g_random)],
			{0.0f, 0.0f, 0.0f}, s.graph.markers[k].yaw};
		MarkerPoint(s.graph.markers[k], n.random, sp.pos);
		g_spawnHere.push_back(sp);
		if (SessionPresenceLobby().IsValid()) {
			g_spawnSend.push_back(sp);
		}
		Out(s, node, 0, now);
		break;
	}
	case kScriptTeleport: {
		int slot = PlayerIn(s, n.player), k = MarkerIn(s, n.marker);
		bool toVec = n.vec >= 0 && n.vec < (int)s.vecs.size();
		if (slot < 0 || (!toVec && k < 0) || !RulesBody(slot)) {
			Out(s, node, 1, now);
			break;
		}
		float pos[3], yaw;
		if (toVec) {   // a vector without facing keeps the player's
			memcpy(pos, s.vecs[n.vec].data(), sizeof(pos));
			yaw = s.vecs[n.vec][3];
		}
		else {
			MarkerPoint(s.graph.markers[k], n.random, pos);
			yaw = s.graph.markers[k].yaw;
		}
		PlayerAct(slot, kActTeleport, n.face ? yaw : NAN, pos);   // amount: the yaw (NaN: keep facing)
		Out(s, node, 0, now);
		break;
	}
	case kScriptGetPos: {
		Vec v = kNoVec;
		int k = MarkerIn(s, n.marker);
		bool ok = n.fromMarker ? k >= 0 : PlayerPose(PlayerIn(s, n.player), &v);
		if (n.fromMarker && ok) {
			const ScriptMarker& m = s.graph.markers[k];
			v = {m.pos[0], m.pos[1], m.pos[2], m.yaw};
		}
		SetVec(s, n.var, v);
		Out(s, node, ok ? 0 : 1, now);
		break;
	}
	case kScriptVector: {
		int k = MarkerIn(s, n.marker);
		float number = 0.0f;
		Vec r = VecMath(n.vecOp, VecOf(s, n.a), VecOf(s, n.b), Get(s, n.b), k >= 0 ? &s.graph.markers[k] : nullptr, &number);
		if (n.vecOp >= kVecLength && n.vecOp <= kVecGetYaw) {
			SetVar(s, n.var, number);
		}
		else {
			SetVec(s, n.var, r);
		}
		Out(s, node, 0, now);
		break;
	}
	case kScriptSwitch: {   // the first case port whose value A equals, else default
		float v = Get(s, n.a);
		int port = 6;   // "default"
		for (int i = 0; i < (int)n.cases.size() && i < 6; i++) {
			if (fabsf(v - n.cases[i]) < 1e-4f) {
				port = i;
				break;
			}
		}
		Out(s, node, port, now);
		break;
	}
	case kScriptSequence:
		for (int i = 0; i < 6; i++) {
			Out(s, node, i, now);
		}
		break;
	case kScriptGate:
		if (in == 0 && s.flip[node]) {
			Out(s, node, 0, now);
		}
		else if (in > 0) {
			s.flip[node] = in == 1 ? 1 : in == 2 ? 0 : !s.flip[node];
		}
		break;
	case kScriptOnce:
		if (in == 1) {
			s.flip[node] = 0;
		}
		else if (!s.flip[node]) {
			s.flip[node] = 1;
			Out(s, node, 0, now);
		}
		break;
	case kScriptBranch: {
		int count = n.target < 1 ? 1 : n.target > 6 ? 6 : n.target;
		Out(s, node, std::uniform_int_distribution<int>(0, count - 1)(g_random), now);
		break;
	}
	case kScriptZoneOutline: {
		float h = Get(s, n.a);
		ZonesShow(Format(s, n.text).c_str(), in == 0, n.rgb, h > 0.0f ? h : 300.0f, n.seconds);
		InterlockedExchange(&g_dirty, 1);
		Out(s, node, 0, now);
		break;
	}
	case kModeMatchStart:
	case kModeRoundStart:
	case kModeRoundEnd:
	case kModeWorldUpdate:
	case kModeOnKill:
	case kModeOnDeath:
	case kModeOnSpawn:
	case kScriptOnModeEvent:
		Out(s, node, 0, now);   // events: ModeDispatch fires them
		break;
	case kModeSpawnPlayer: {
		int slot = PlayerIn(s, n.player), k = MarkerIn(s, n.marker);
		if (slot < 0 || !RulesInPlay(slot)) {
			Out(s, node, 1, now);
			break;
		}
		float pos[3], yaw = NAN;
		const float* at = nullptr;
		if (k >= 0) {
			MarkerPoint(s.graph.markers[k], n.random, pos);
			yaw = n.face ? s.graph.markers[k].yaw : NAN;
			at = pos;
		}
		RulesEliminate(slot, false);
		RulesRelease(slot, at, yaw);
		ModeSendSlot(kModeRelease, slot, at, yaw);
		Out(s, node, 0, now);
		break;
	}
	case kModeHoldPlayer:
	case kModeEliminate: {
		int slot = PlayerIn(s, n.player);
		if (slot >= 0) {
			ModeHold(slot, n.type == kModeEliminate);
		}
		Out(s, node, 0, now);
		break;
	}
	case kModeRespawnRules:
		g_modeManual = n.manual;
		g_modeLives = std::max(0, (int)lrintf(Get(s, n.b)));
		XLS_LOG_INFO("mods: mode: respawns %s, %d li%s a round.", g_modeManual ? "only by Spawn player" : "automatic", g_modeLives,
		             g_modeLives == 1 ? "fe" : "ves");
		Out(s, node, 0, now);
		break;
	case kModeQueue: {
		std::vector<int>* q = ListIn(s, n.list);
		int slot = PlayerIn(s, n.player);
		bool empty = false;
		if (q) {
			auto it = std::find(q->begin(), q->end(), slot);
			if ((in == 0 || in == 1) && slot >= 0 && it == q->end()) {
				q->insert(in == 0 ? q->end() : q->begin(), slot);
			}
			else if (in == 2) {
				empty = q->empty();
				SetVar(s, n.player, empty ? -1.0f : (float)q->front());
				if (!empty) {
					q->erase(q->begin());
				}
			}
			else if (in == 3 && it != q->end()) {
				q->erase(it);
			}
			else if (in == 4) {
				q->clear();
			}
		}
		Out(s, node, empty ? 1 : 0, now);
		break;
	}
	case kModeScore: {
		int slot = PlayerIn(s, n.player);
		int team = slot >= 0 && n.scoreOf != 0 ? RulesTeamOf(slot) : -1;
		int v = (int)lrintf(Get(s, n.a));
		int* match = nullptr;
		int* round = nullptr;
		if (n.scoreOf == 0 && slot >= 0) {
			match = &g_modeScore[slot];
			round = &g_modeRoundScore[slot];
		}
		else if (team == 0 || team == 1) {
			match = &g_modeTeamScore[team];
			round = &g_modeTeamRoundScore[team];
		}
		if (match) {
			if (in == 1) { // set: the round's part follows
				*round += v - *match;
				*match = v;
			}
			else {
				*match += v;
				*round += v;
			}
			XLS_LOG_INFO("mods: mode: score of %s %d %s %d: %d.", n.scoreOf == 0 ? "player" : "team", n.scoreOf == 0 ? slot : team,
			             in == 1 ? "set to" : "+", v, *match);
		}
		Out(s, node, 0, now);
		break;
	}
	case kModePlayerInfo: {
		int slot = PlayerIn(s, n.player);
		float v = 0.0f;
		if (slot >= 0) {
			v = n.info == 0 ? (float)RulesTeamOf(slot) : n.info == 1 ? (float)g_modeScore[slot] : n.info == 3 ? (float)g_modeDeaths[slot]
				: n.info == 4 ? (g_modeLives > 0 ? (float)std::max(0, g_modeLives - g_modeDeaths[slot]) : -1.0f) : 0.0f;
		}
		SetVar(s, n.var, v);
		Out(s, node, 0, now);
		break;
	}
	case kModeTeamList: {
		std::vector<int>* q = ListIn(s, n.list);
		int slot = PlayerIn(s, n.player), team = slot >= 0 ? RulesTeamOf(slot) : -1;
		if (q) {
			q->clear();
			for (int k = 0; k < kSlots; k++) {
				if (RulesInPlay(k) && (!RulesTeams() || slot < 0 || RulesTeamOf(k) == team) && (!n.alive || !RulesHeld(k))) {
					q->push_back(k);
				}
			}
		}
		Out(s, node, 0, now);
		break;
	}
	case kModeEndRound:
	case kModeEndMatch: {
		if (!g_modeEnd.pending) {
			int slot = PlayerIn(s, n.player);
			ModeEnd e;
			e.pending = true;
			e.match = n.type == kModeEndMatch;
			if (slot >= 0 && n.scoreOf == 0) {
				e.side = slot;
			}
			else if (slot >= 0) {
				int t = RulesTeamOf(slot);
				e.team = true;
				e.side = n.scoreOf == 2 ? (t == 0 ? 1 : 0) : t;
			}
			g_modeEnd = e;
			XLS_LOG_INFO("mods: mode: end %s, won by %s %d.", e.match ? "match" : "round", e.team ? "team" : "player", e.side);
		}
		break;
	}
	case kModeShowText: {
		int slot = PlayerIn(s, n.player);
		int who = n.showTo == 1 ? slot : n.showTo == 2 ? (slot >= 0 ? RulesTeamOf(slot) : -1) : -1;
		ModeText(Format(s, n.text), n.style, n.rgb, n.seconds, n.showTo, who);
		Out(s, node, 0, now);
		break;
	}
	case kModeFireEvent:
		if (&s == g_mode) {
			MapModeEvent(n.text, PlayerIn(s, n.player), now);
		}
		Out(s, node, 0, now);
		break;
	case kScriptChance:
		Out(s, node, std::uniform_real_distribution<float>(0.0f, 100.0f)(g_random) < Get(s, n.a) ? 0 : 1, now);
		break;
	case kScriptCooldown:
		if (in == 1) {
			s.lastAt[node] = 0.0;
		}
		else if (!s.lastAt[node] || now - s.lastAt[node] >= n.seconds) {
			s.lastAt[node] = now;
			Out(s, node, 0, now);
		}
		else {
			Out(s, node, 1, now);
		}
		break;
	case kScriptLoop: {
		// a, a + 1 ... b (or down to b) into the variable, at most kSteps times
		float a = Get(s, n.a), b = Get(s, n.b), step = b >= a ? 1.0f : -1.0f;
		for (int k = 0; k <= kSteps && (step > 0.0f ? a <= b : a >= b) && s.steps <= kSteps; k++, a += step) {
			SetVar(s, n.var, a);
			Out(s, node, 0, now);
		}
		Out(s, node, 1, now);
		break;
	}
	}
}

void Trigger(State& s, int sid, int who, int slot, bool entered, double now)
{
	for (size_t i = 0; i < s.graph.nodes.size(); i++) {
		const ScriptNode& n = s.graph.nodes[i];
		if (n.type == kScriptEnter && n.sid == sid && (n.who == 0 || n.who == who)) {
			SetVar(s, n.player, slot >= 0 && slot < kSlots ? (float)slot : -1.0f);
			s.steps = 0;
			Out(s, (int)i, entered ? 0 : 1, now);
		}
	}
}

void Tick(State& s, double now)
{
	if (!s.started) {
		s.started = true;
		for (size_t i = 0; i < s.graph.nodes.size(); i++) {
			const ScriptNode& n = s.graph.nodes[i];
			if (n.type == kScriptTimer && n.autostart) {
				s.timerAt[i] = now + Seconds(n);
			}
		}
		for (size_t i = 0; i < s.graph.nodes.size(); i++) {
			if (s.graph.nodes[i].type == kScriptStart) {
				s.steps = 0;
				Fire(s, (int)i, 0, now);
			}
		}
	}
	while (!s.reports.empty()) {   // players' triggers
		Report r = s.reports.front();
		s.reports.pop_front();
		Trigger(s, r.sid, r.who, r.slot, r.entered, now);
	}
	for (size_t i = 0; i < s.graph.nodes.size(); i++) {
		if (s.timerAt[i] > 0.0 && now >= s.timerAt[i]) {
			const ScriptNode& n = s.graph.nodes[i];
			double next = Seconds(n);
			s.timerAt[i] = n.repeat ? now + (next > 0.05 ? next : 0.05) : 0.0;
			s.steps = 0;
			Out(s, (int)i, 0, now);
		}
	}
	if (!s.delays.empty()) {
		std::vector<Delay> due;
		for (size_t i = 0; i < s.delays.size();) {
			if (now >= s.delays[i].at) {
				due.push_back(s.delays[i]);
				s.delays[i] = s.delays.back();
				s.delays.pop_back();
			}
			else {
				i++;
			}
		}
		for (const Delay& d : due) {
			s.steps = 0;
			Out(s, d.node, 0, now);
		}
	}
	// characters not updated for a while (gone): forgotten without a "left"
	for (auto it = s.seen.begin(); it != s.seen.end();) {
		if (now - it->second > 3.0) {
			s.inside.erase(it->first);
			it = s.seen.erase(it);
		}
		else {
			++it;
		}
	}
}

State* NewState(const ScriptGraph& graph)
{
	State* s = new State();
	s->graph = graph;
	size_t n = s->graph.nodes.size();
	s->vars.assign(s->graph.vars > 0 ? s->graph.vars : 0, 0.0f);
	s->lists.assign(s->vars.size(), std::vector<int>());
	s->vecs.assign(s->vars.size(), kNoVec);
	for (size_t v = 0; v < s->vars.size(); v++) {
		if (Kind(*s, (int)v) == kVarPlayer || Kind(*s, (int)v) == kVarMarker) {
			s->vars[v] = -1.0f;   // nobody / none
		}
	}
	for (const auto& [v, value] : s->graph.varInit) {
		SetVar(*s, v, value);
	}
	s->timerAt.assign(n, 0.0);
	s->flip.assign(n, 0);
	s->lastAt.assign(n, 0.0);
	for (size_t i = 0; i < n; i++) {
		if (s->graph.nodes[i].type == kScriptGate) {
			s->flip[i] = s->graph.nodes[i].autostart ? 1 : 0;
		}
	}
	s->count.assign(n, 0);
	return s;
}

// A mode event into its nodes: the event's values into their variables, then their output
void ModeDispatch(State& s, int type, int a, int b, float x, double now)
{
	for (size_t i = 0; i < s.graph.nodes.size(); i++) {
		const ScriptNode& n = s.graph.nodes[i];
		if (n.type != type) {
			continue;
		}
		if (type == kModeRoundStart || type == kModeRoundEnd) {
			SetVar(s, n.var, (float)a);
		}
		else if (type == kModeWorldUpdate) {
			SetVar(s, n.var, x);
		}
		else if (type == kModeOnKill) {
			SetVar(s, n.player, (float)a);
			SetVar(s, n.victim, (float)b);
		}
		else if (type == kModeOnDeath || type == kModeOnSpawn) {
			SetVar(s, n.player, (float)a);
		}
		s.steps = 0;
		Out(s, (int)i, 0, now);
	}
}

// a mode's "Fire map event": the map's own script (host) runs its "On mode event" nodes with that name (case ignored)
void MapModeEvent(const std::string& name, int player, double now)
{
	State* s = g_state;
	if (!s) {
		return;
	}
	int ran = 0;
	for (size_t i = 0; i < s->graph.nodes.size(); i++) {
		const ScriptNode& n = s->graph.nodes[i];
		if (n.type != kScriptOnModeEvent || _stricmp(n.text.c_str(), name.c_str()) != 0) {
			continue;
		}
		SetVar(*s, n.player, (float)player);
		s->steps = 0;
		Out(*s, (int)i, 0, now);
		ran++;
	}
	static std::string logged;
	if (logged != name) {
		logged = name;
		XLS_LOG_INFO("mods: mode: map event \"%s\" (player %d): %d map node(s).", name.c_str(), player, ran);
	}
}

void ModeApply(const uint8_t* p, const uint8_t* end)
{
	uint8_t op;
	if (!Take(p, end, &op)) {
		return;
	}
	if (op == kModeText) {
		uint8_t style, showTo;
		uint32_t rgb;
		float seconds;
		int8_t who;
		uint16_t len;
		if (Take(p, end, &style) && Take(p, end, &rgb) && Take(p, end, &seconds) && Take(p, end, &showTo) && Take(p, end, &who) && Take(p, end, &len) && end - p >= len) {
			ModeShowHere(std::string((const char*)p, len), style, rgb, seconds, showTo, who);
		}
		return;
	}
	if (op == kModeMasks) {
		uint32_t held, out;
		if (Take(p, end, &held) && Take(p, end, &out)) {
			for (int k = 0; k < kSlots; k++) {
				if (((out >> k) & 1) != RulesEliminated(k)) {
					RulesEliminate(k, (out >> k) & 1);
				}
				if (!((out >> k) & 1) && ((held >> k) & 1) != RulesHeld(k) && ((held >> k) & 1)) {
					RulesHold(k, true);   // releases come as their own messages (the spawn)
				}
			}
		}
		return;
	}
	uint8_t slot, has;
	float pos[3], yaw;
	if (!Take(p, end, &slot) || !Take(p, end, &has) || !Take(p, end, &pos[0]) || !Take(p, end, &pos[1]) || !Take(p, end, &pos[2]) || !Take(p, end, &yaw) || slot >= kSlots) {
		return;
	}
	if (op == kModeHold) {
		RulesHold(slot, true);
	}
	else if (op == kModeElim) {
		RulesEliminate(slot, true);
	}
	else if (op == kModeRelease) {
		RulesEliminate(slot, false);
		RulesRelease(slot, has ? pos : nullptr, yaw);
	}
}

uint32_t Key(const ScriptGraph& g)
{
	uint32_t h = 2166136261u;
	auto mix = [&h](uint32_t v) {
		for (int i = 0; i < 4; i++, v >>= 8) {
			h = (h ^ (v & 0xff)) * 16777619u;
		}
	};
	mix((uint32_t)g.nodes.size());
	mix((uint32_t)g.volumes);
	for (const ScriptNode& n : g.nodes) {
		mix((uint32_t)n.type);
		mix((uint32_t)n.sid);
		mix((uint32_t)n.var);
		mix((uint32_t)n.setting);
		mix((uint32_t)n.player);
		mix((uint32_t)n.list);
	}
	return h ? h : 1;
}

// ---------------------------------------------------------------- network

template <typename T>
void Put(std::vector<uint8_t>* m, T v)
{
	const uint8_t* p = (const uint8_t*)&v;
	m->insert(m->end(), p, p + sizeof(T));
}

template <typename T>
bool Take(const uint8_t*& p, const uint8_t* end, T* v)
{
	if (end - p < (ptrdiff_t)sizeof(T)) {
		return false;
	}
	memcpy(v, p, sizeof(T));
	p += sizeof(T);
	return true;
}

void SendTo(uint64_t peer, const std::vector<uint8_t>& m)
{
	if (!SteamReady() || !SteamNetworkingMessages()) {
		return;
	}
	SteamNetworkingIdentity id;
	id.SetSteamID64(peer);
	SteamNetworkingMessages()->SendMessageToUser(id, m.data(), (uint32)m.size(),
		k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_AutoRestartBrokenSession, kChannel);
}

std::vector<uint8_t> StateMessage(uint32_t key, int volumes, double now)
{
	std::vector<uint8_t> m;
	Put<uint8_t>(&m, kState);
	Put<uint32_t>(&m, key);
	Put<uint16_t>(&m, (uint16_t)volumes);
	for (int i = 0; i < volumes; i++) {
		Put<uint8_t>(&m, g_on[i] ? 1 : 0);
	}
	std::vector<uint8_t> values;
	uint16_t n = 0;
	AcquireSRWLockShared(&g_valueLock);
	for (int i = 0; i < volumes; i++) {
		for (int k = 0; k < kSettings; k++) {
			const Value& v = g_values[i][k];
			if (v.set) {
				Put<uint16_t>(&values, (uint16_t)i);
				Put<uint8_t>(&values, (uint8_t)k);
				Put<float>(&values, v.from);
				Put<float>(&values, v.to);
				Put<float>(&values, (float)(now - v.t0));
				Put<float>(&values, v.fade);
				n++;
			}
		}
	}
	ReleaseSRWLockShared(&g_valueLock);
	Put<uint16_t>(&m, n);
	m.insert(m.end(), values.begin(), values.end());
	ZonesPack(&m);   // the zone outlines shown (zones.cpp)
	return m;
}

void ApplyState(const uint8_t* p, const uint8_t* end, uint32_t key, int volumes, double now)
{
	uint32_t k = 0;
	uint16_t nv = 0, n = 0;
	if (!Take(p, end, &k) || k != key || !Take(p, end, &nv) || end - p < nv) {
		return;   // another map's
	}
	for (int i = 0; i < nv; i++, p++) {
		LONG on = *p ? 1 : 0;
		if (i < volumes && InterlockedExchange(&g_on[i], on) != on) {
			XLS_LOG_INFO("mods: script: volume %d %s (host).", i, on ? "on" : "off");
		}
	}
	if (!Take(p, end, &n)) {
		return;
	}
	bool whole = true;
	AcquireSRWLockExclusive(&g_valueLock);
	for (int i = 0; i < n; i++) {
		uint16_t sid;
		uint8_t setting;
		float from, to, elapsed, fade;
		if (!Take(p, end, &sid) || !Take(p, end, &setting) || !Take(p, end, &from) || !Take(p, end, &to) || !Take(p, end, &elapsed) || !Take(p, end, &fade)) {
			whole = false;
			break;
		}
		if (sid < volumes && setting < kSettings) {
			g_values[sid][setting] = {true, from, to, fade, now - elapsed};
		}
	}
	ReleaseSRWLockExclusive(&g_valueLock);
	if (whole) {
		ZonesUnpack(p, end);
	}
}

void OnMessage(const uint8_t* p, size_t size, uint32_t key, int volumes, double now, bool follow)
{
	const uint8_t* end = p + size;
	uint8_t type = 0;
	if (!Take(p, end, &type)) {
		return;
	}
	if (type == kState && follow) {
		ApplyState(p, end, key, volumes, now);
	}
	else if (type == kTrigger && !follow) {
		uint32_t k;
		uint8_t slot, who, entered;
		uint16_t sid;
		if (Take(p, end, &k) && k == key && Take(p, end, &slot) && Take(p, end, &who) && Take(p, end, &sid) && Take(p, end, &entered) && sid < volumes) {
			EnterCriticalSection(&g_lock);
			if (g_state) {
				g_state->reports.push_back({slot < kSlots ? slot : -1, who, sid, entered != 0});
			}
			LeaveCriticalSection(&g_lock);
		}
	}
	else if (type == kVitals && !follow) {
		uint32_t k;
		uint8_t slot;
		CharacterVitals v;
		if (Take(p, end, &k) && k == key && Take(p, end, &slot) && slot < kSlots && Take(p, end, &v.life) && Take(p, end, &v.lifeMax) && Take(p, end, &v.energy) && Take(p, end, &v.energyMax)) {
			EnterCriticalSection(&g_lock);
			g_vitals[slot] = {v, now, true};
			LeaveCriticalSection(&g_lock);
		}
	}
	else if (type == kSpawn && follow) {
		uint32_t k;
		uint8_t kind;
		uint16_t item;
		Spawn sp;
		if (Take(p, end, &k) && k == key && Take(p, end, &kind) && Take(p, end, &item) && Take(p, end, &sp.pos[0]) && Take(p, end, &sp.pos[1]) && Take(p, end, &sp.pos[2]) && Take(p, end, &sp.yaw)) {
			sp.kind = kind;
			sp.item = item;
			EnterCriticalSection(&g_lock);
			g_spawnHere.push_back(sp);
			LeaveCriticalSection(&g_lock);
		}
	}
	else if (type == kMode && follow) {
		ModeApply(p, end);
	}
	else if (type == kAct && follow) {
		uint32_t k;
		uint8_t slot, action;
		float amount, pos[3];
		if (Take(p, end, &k) && k == key && Take(p, end, &slot) && Take(p, end, &action) && Take(p, end, &amount) && Take(p, end, &pos[0]) && Take(p, end, &pos[1]) && Take(p, end, &pos[2]) && action < kActions && (int)slot == RulesLocalSlot()) {
			EnterCriticalSection(&g_lock);
			g_local.push_back({slot, action, amount, {pos[0], pos[1], pos[2]}});
			LeaveCriticalSection(&g_lock);
			XLS_LOG_INFO("mods: script: %s %g (host).", kScriptActions[action], amount);
		}
	}
}

DWORD WINAPI SyncThread(LPVOID)
{
	double lastSent = 0.0, lastVitals = 0.0;
	for (;; Sleep(50)) {
		double now = Now();
		uint32_t key = (uint32_t)g_key;
		int volumes = 0;
		std::deque<Report> send;
		std::deque<Act> acts;
		std::deque<Spawn> spawns;
		CharacterVitals mine = {};
		int selfSlot = -1;
		if (key) {
			EnterCriticalSection(&g_lock);
			if (g_state) {
				volumes = g_state->graph.volumes <= kMaxVolumes ? g_state->graph.volumes : 0;
				if (g_follow) {
					send.swap(g_state->reports);
				}
			}
			acts.swap(g_send);
			spawns.swap(g_spawnSend);
			mine = g_self;
			selfSlot = g_selfSlot;
			LeaveCriticalSection(&g_lock);
		}
		CSteamID lobby = SessionPresenceLobby();
		uint64_t host = 0, self = 0;
		std::vector<uint64_t> members;
		if (lobby.IsValid() && SteamReady() && SteamMatchmaking()) {
			host = LobbyOwner(SteamMatchmaking(), lobby).ConvertToUint64();
			self = SteamLocalId().ConvertToUint64();
			int count = SteamMatchmaking()->GetNumLobbyMembers(lobby);
			for (int i = 0; i < count; i++) {
				uint64_t m = LobbyMember(SteamMatchmaking(), lobby, i).ConvertToUint64();
				if (m && m != self) {
					members.push_back(m);
				}
			}
		}
		bool follow = host && host != self;
		if (InterlockedExchange(&g_follow, follow) != (LONG)follow) {
			XLS_LOG_INFO("mods: script: %s.", follow ? "following the host's script" : "running the script here");
		}
		if (SteamReady() && SteamNetworkingMessages()) {
			SteamNetworkingMessage_t* in[32];
			int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(kChannel, in, 32);
			for (int i = 0; i < count; i++) {
				OnMessage((const uint8_t*)in[i]->m_pData, (size_t)in[i]->m_cbSize, key, volumes, now, follow);   // checks the key itself
				in[i]->Release();
			}
		}
		if (!follow) {   // the game mode's messages (any map, scripted or not)
			std::deque<std::vector<uint8_t>> modeOut;
			bool modeOn = false;
			EnterCriticalSection(&g_lock);
			modeOut.swap(g_modeSend);
			modeOn = g_mode != nullptr;
			LeaveCriticalSection(&g_lock);
			static double lastMasks = 0.0;
			if (modeOn && now - lastMasks > 2.0) {   // late joiners, lost packets
				lastMasks = now;
				std::vector<uint8_t> m;
				Put<uint8_t>(&m, kMode);
				Put<uint8_t>(&m, kModeMasks);
				uint32_t held = 0, out = 0;
				for (int k = 0; k < kSlots; k++) {
					held |= RulesHeld(k) ? 1u << k : 0;
					out |= RulesEliminated(k) ? 1u << k : 0;
				}
				Put<uint32_t>(&m, held);
				Put<uint32_t>(&m, out);
				modeOut.push_back(std::move(m));
			}
			for (const auto& m : modeOut) {
				for (uint64_t peer : members) {
					SendTo(peer, m);
				}
			}
		}
		if (!key) {
			continue;
		}
		if (follow) {
			for (const Report& r : send) {
				std::vector<uint8_t> m;
				Put<uint8_t>(&m, kTrigger);
				Put<uint32_t>(&m, key);
				Put<uint8_t>(&m, (uint8_t)r.slot);
				Put<uint8_t>(&m, (uint8_t)r.who);
				Put<uint16_t>(&m, (uint16_t)r.sid);
				Put<uint8_t>(&m, r.entered ? 1 : 0);
				SendTo(host, m);
			}
			if (selfSlot >= 0 && now - lastVitals >= 0.25) {
				std::vector<uint8_t> m;
				Put<uint8_t>(&m, kVitals);
				Put<uint32_t>(&m, key);
				Put<uint8_t>(&m, (uint8_t)selfSlot);
				Put<float>(&m, mine.life);
				Put<float>(&m, mine.lifeMax);
				Put<float>(&m, mine.energy);
				Put<float>(&m, mine.energyMax);
				SendTo(host, m);
				lastVitals = now;
			}
		}
		else {
			for (const Spawn& sp : spawns) {
				std::vector<uint8_t> m;
				Put<uint8_t>(&m, kSpawn);
				Put<uint32_t>(&m, key);
				Put<uint8_t>(&m, (uint8_t)sp.kind);
				Put<uint16_t>(&m, (uint16_t)sp.item);
				for (float f : sp.pos) {
					Put<float>(&m, f);
				}
				Put<float>(&m, sp.yaw);
				for (uint64_t peer : members) {
					SendTo(peer, m);
				}
			}
			for (const Act& a : acts) {
				std::vector<uint8_t> m;
				Put<uint8_t>(&m, kAct);
				Put<uint32_t>(&m, key);
				Put<uint8_t>(&m, (uint8_t)a.slot);
				Put<uint8_t>(&m, (uint8_t)a.action);
				Put<float>(&m, a.amount);
				for (float f : a.pos) {
					Put<float>(&m, f);
				}
				for (uint64_t peer : members) {
					SendTo(peer, m);
				}
			}
			if (!members.empty() && (InterlockedExchange(&g_dirty, 0) || now - lastSent > 2.0)) {
				std::vector<uint8_t> m = StateMessage(key, volumes, now);
				for (uint64_t peer : members) {
					SendTo(peer, m);
				}
				lastSent = now;
			}
		}
	}
}

}

// ---------------------------------------------------------------- game modes: the API (lp2.h)

void ModeLoad()
{
	// mode.txt in the game folder: the mode's name (Modes\<name>.lp2mode), then "setting = value" lines
	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	char* slash = strrchr(path, '\\');
	std::string dir(path, slash ? slash + 1 : path);
	std::string name;
	std::vector<std::pair<std::string, float>> overrides;
	if (FILE* f = fopen((dir + "mode.txt").c_str(), "r")) {
		char line[256];
		while (fgets(line, sizeof line, f)) {
			std::string t(line);
			while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) {
				t.pop_back();
			}
			size_t eq = t.find('=');
			if (name.empty() && eq == std::string::npos) {
				name = t;
			}
			else if (eq != std::string::npos) {
				std::string k = t.substr(0, eq);
				while (!k.empty() && k.back() == ' ') {
					k.pop_back();
				}
				for (char& c : k) {
					c = (char)tolower((unsigned char)c);
				}
				overrides.emplace_back(k, (float)atof(t.c_str() + eq + 1));
			}
		}
		fclose(f);
	}
	ScriptsInit();
	EnterCriticalSection(&g_lock);
	delete g_mode;
	g_mode = nullptr;
	g_modeEnd = ModeEnd();
	g_modeLives = 0;
	g_modeManual = false;
	memset(g_modeDeaths, 0, sizeof g_modeDeaths);
	memset(g_modeScore, 0, sizeof g_modeScore);
	memset(g_modeRoundScore, 0, sizeof g_modeRoundScore);
	memset(g_modeTeamScore, 0, sizeof g_modeTeamScore);
	memset(g_modeTeamRoundScore, 0, sizeof g_modeTeamRoundScore);
	LeaveCriticalSection(&g_lock);
	{
		std::lock_guard<std::mutex> lock(g_modeEvLock);
		g_modeEvents.clear();
	}
	if (name.empty()) {
		if (!g_modeName.empty()) {
			XLS_LOG_INFO("mods: mode: none (no mode.txt): the lobby's game type as it is.");
		}
		g_modeName.clear();
		return;
	}
	std::string file = dir + "Modes\\" + name + ".lp2mode", text;
	if (FILE* f = fopen(file.c_str(), "rb")) {
		char buf[65536];
		size_t got;
		while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
			text.append(buf, got);
		}
		fclose(f);
	}
	Json root;
	std::string error;
	if (text.empty() || !Json::Parse(text, root, &error)) {
		XLS_LOG_WARN("mods: mode: can't read %s%s%s.", file.c_str(), error.empty() ? "" : ": ", error.c_str());
		return;
	}
	if (!root["compiled"].IsObject()) {
		XLS_LOG_WARN("mods: mode: %s has no compiled graph: save it in the editor's Game Mode window.", name.c_str());
		return;
	}
	ScriptGraph graph;
	ParseScriptGraph(root["compiled"], std::wstring(name.begin(), name.end()), &graph);
	graph.markers = g_mapMarkers;
	for (const auto& [k, v] : overrides) {
		for (size_t i = 0; i < graph.varNames.size(); i++) {
			if (graph.varNames[i] == k) {
				graph.varInit.emplace_back((int)i, v);
				XLS_LOG_INFO("mods: mode: setting %s = %g (mode.txt).", k.c_str(), v);
			}
		}
	}
	EnterCriticalSection(&g_lock);
	g_mode = NewState(graph);
	g_mode->started = true;   // its Start is Match start (ModeEvent)
	LeaveCriticalSection(&g_lock);
	g_modeName = name;
	XLS_LOG_INFO("mods: mode: %s, %u node(s), %d variable(s).", name.c_str(), (unsigned)graph.nodes.size(), graph.vars);
}

void ModeEvent(int kind, int a, int b)
{
	std::lock_guard<std::mutex> lock(g_modeEvLock);
	if (g_modeEvents.size() < 256) {
		g_modeEvents.push_back({kind, a, b});
	}
}

void ModeFrame()
{
	if (!g_lockReady || g_follow) {   // the host runs the mode; players get its messages
		return;
	}
	std::vector<ModeEv> events;
	{
		std::lock_guard<std::mutex> lock(g_modeEvLock);
		events.swap(g_modeEvents);
	}
	double now = Now();
	EnterCriticalSection(&g_lock);
	State* s = g_mode;
	if (s) {
		for (const ModeEv& e : events) {
			switch (e.kind) {
			case kEvMatchStart:
				ModeDispatch(*s, kModeMatchStart, 0, 0, 0.0f, now);
				break;
			case kEvRoundStart:
				memset(g_modeDeaths, 0, sizeof g_modeDeaths);
				memset(g_modeRoundScore, 0, sizeof g_modeRoundScore);
				memset(g_modeTeamRoundScore, 0, sizeof g_modeTeamRoundScore);
				g_modeEnd = ModeEnd();
				ModeDispatch(*s, kModeRoundStart, e.a, 0, 0.0f, now);
				break;
			case kEvRoundEnd:
				ModeDispatch(*s, kModeRoundEnd, e.a, 0, 0.0f, now);
				break;
			case kEvDeath:
				if (e.a >= 0 && e.a < kSlots) {
					g_modeDeaths[e.a]++;
					if (g_modeManual) {
						ModeHold(e.a, false);
					}
					else if (g_modeLives > 0 && g_modeDeaths[e.a] >= g_modeLives) {
						ModeHold(e.a, true);
						XLS_LOG_INFO("mods: mode: player %d out of lives (%d deaths).", e.a, g_modeDeaths[e.a]);
					}
				}
				ModeDispatch(*s, kModeOnDeath, e.a, 0, 0.0f, now);
				break;
			case kEvKill:
				ModeDispatch(*s, kModeOnKill, e.a, e.b, 0.0f, now);
				break;
			case kEvSpawn:
				ModeDispatch(*s, kModeOnSpawn, e.a, 0, 0.0f, now);
				break;
			}
		}
		float dt = g_modeLastFrame > 0.0 ? (float)std::min(now - g_modeLastFrame, 0.25) : 0.0f;
		g_modeLastFrame = now;
		ModeDispatch(*s, kModeWorldUpdate, 0, 0, dt, now);
		Tick(*s, now);   // timers, delays
	}
	LeaveCriticalSection(&g_lock);
}

bool ModeActive()
{
	return g_mode != nullptr;
}

int ModeLivesLeft(int slot)
{
	// lives left this round (deaths still allowed + 1 while alive); -2: no mode with lives, or a player (the host keeps them)
	if (!g_mode || g_follow || g_modeManual || g_modeLives <= 0 || slot < 0 || slot >= kSlots) {
		return -2;
	}
	return std::max(0, g_modeLives - g_modeDeaths[slot]);
}

bool ModeScores(int id, bool team, int* match, int* round)
{
	*match = *round = 0;
	if (!g_lockReady) {
		return false;
	}
	EnterCriticalSection(&g_lock);
	bool on = g_mode != nullptr;
	if (on && !team && id >= 0 && id < kSlots) {
		*match = g_modeScore[id];
		*round = g_modeRoundScore[id];
	}
	else if (on && team && (id == 0 || id == 1)) { // the team's own score and its players'
		*match = g_modeTeamScore[id];
		*round = g_modeTeamRoundScore[id];
		for (int k = 0; k < kSlots; k++) {
			if (RulesTeamOf(k) == id) {
				*match += g_modeScore[k];
				*round += g_modeRoundScore[k];
			}
		}
	}
	LeaveCriticalSection(&g_lock);
	return on;
}

bool ModeEndPending(int* side, bool* team, bool* match)
{
	if (!g_lockReady) {
		return false;
	}
	EnterCriticalSection(&g_lock);
	ModeEnd e = g_mode ? g_modeEnd : ModeEnd();
	LeaveCriticalSection(&g_lock);
	if (side) {
		*side = e.side;
	}
	if (team) {
		*team = e.team;
	}
	if (match) {
		*match = e.match;
	}
	return e.pending;
}

void ScriptsInit()
{
	if (!g_lockReady) {
		InitializeCriticalSection(&g_lock);
		g_lockReady = true;
		LARGE_INTEGER seed;
		QueryPerformanceCounter(&seed);
		g_random.seed((uint32_t)seed.QuadPart);
		HANDLE t = CreateThread(nullptr, 0, SyncThread, nullptr, 0, nullptr);
		if (t) {
			CloseHandle(t);
		}
	}
}

void ScriptSetMap(const MapPhysics* physics)
{
	ScriptsInit();
	EnterCriticalSection(&g_lock);
	delete g_state;
	g_state = nullptr;
	for (Vitals& v : g_vitals) {
		v = Vitals();
	}
	g_selfSlot = -1;
	g_local.clear();
	g_send.clear();
	g_spawnHere.clear();
	g_spawnSend.clear();
	InterlockedExchange(&g_active, 0);
	InterlockedExchange(&g_key, 0);
	for (int i = 0; i < kMaxVolumes; i++) {
		g_on[i] = 1;
	}
	AcquireSRWLockExclusive(&g_valueLock);
	memset(g_values, 0, sizeof(g_values));
	memset(g_base, 0, sizeof(g_base));
	if (physics) {
		auto base = [](int sid, int setting, float v) {
			if (sid >= 0 && sid < kMaxVolumes) {
				g_base[sid][setting] = v;
			}
		};
		for (const GravityVolume& v : physics->volumes) {
			base(v.sid, kSetGravity, v.gravity);
		}
		for (const DamageVolume& v : physics->damage) {
			base(v.sid, kSetDamagePlayer, v.player);
			base(v.sid, kSetDamageVs, v.vs);
		}
		for (const PostVolume& v : physics->post) {
			base(v.sid, kSetBrightness, v.brightness);
			base(v.sid, kSetSaturation, v.saturation);
			base(v.sid, kSetContrast, v.contrast);
			base(v.sid, kSetTintR, v.tint[0]);
			base(v.sid, kSetTintG, v.tint[1]);
			base(v.sid, kSetTintB, v.tint[2]);
		}
	}
	ReleaseSRWLockExclusive(&g_valueLock);
	g_mapMarkers = physics ? physics->script.markers : std::vector<ScriptMarker>();
	if (g_mode) {
		g_mode->graph.markers = g_mapMarkers;
	}
	if (physics && !physics->script.nodes.empty()) {
		State* s = NewState(physics->script);
		size_t n = s->graph.nodes.size();
		for (int sid : s->graph.startOff) {
			if (sid >= 0 && sid < kMaxVolumes) {
				g_on[sid] = 0;
			}
		}
		g_state = s;
		InterlockedExchange(&g_key, (LONG)Key(s->graph));
		InterlockedExchange(&g_dirty, 1);
		InterlockedExchange(&g_active, 1);
		XLS_LOG_INFO("mods: script: %u node(s), %d volume(s), %u box(es).", (unsigned)n, s->graph.volumes, (unsigned)s->graph.boxes.size());
	}
	LeaveCriticalSection(&g_lock);
}

bool ScriptVolumeOn(int sid)
{
	return sid < 0 || sid >= kMaxVolumes || g_on[sid] != 0;
}

float ScriptValue(int sid, int setting, float base)
{
	if (sid < 0 || sid >= kMaxVolumes || setting < 0 || setting >= kSettings || !g_active) {
		return base;
	}
	AcquireSRWLockShared(&g_valueLock);
	const Value& v = g_values[sid][setting];
	float out = v.set ? Evaluate(v, Now()) : base;
	ReleaseSRWLockShared(&g_valueLock);
	return out;
}

void ScriptCharacter(unsigned char* ch)
{
	if (!g_active) {
		return;
	}
	double now = Now();
	bool follow = g_follow != 0;
	int slot = RulesSlotOf(ch);
	int local = RulesLocalSlot();
	// a player's own machine reports its triggers; in a lobby the host skips the other players' (theirs arrive)
	bool mine = slot >= 0 && slot == local;
	bool remote = slot >= 0 && local >= 0 && slot != local;
	bool skip = follow ? !mine : (remote && SessionPresenceLobby().IsValid());
	EnterCriticalSection(&g_lock);
	State* s = g_state;
	if (slot >= 0 && slot < kSlots) {
		CharacterVitals v;
		bool lobby = SessionPresenceLobby().IsValid();
		if (CharacterStats(ch, &v)) {
			if (mine || !lobby) {
				g_vitals[slot] = {v, now, true};
			}
			else if (!g_vitals[slot].owner || now - g_vitals[slot].at > 2.0) {
				g_vitals[slot] = {v, now, false};   // until (or when no longer) its machine reports
			}
			if (mine) {
				g_self = v;
				g_selfSlot = slot;
			}
		}
		if (!g_local.empty() && (mine || !lobby)) {
			for (auto it = g_local.begin(); it != g_local.end();) {
				if (it->slot == slot) {
					Apply(ch, *it);
					it = g_local.erase(it);
				}
				else {
					++it;
				}
			}
		}
	}
	if (s && !follow && now - s->lastTick >= 1.0 / 120.0) {   // the host's clock: on any character's update
		s->lastTick = now;
		Tick(*s, now);
	}
	while (!g_spawnHere.empty()) {
		Spawn sp = g_spawnHere.front();
		g_spawnHere.pop_front();
		SpawnObject(sp.kind, sp.item, sp.pos, sp.yaw);
	}
	if (s && !skip) {
		s->seen[ch] = now;
		std::vector<uint8_t>& was = s->inside[ch];
		int nv = s->graph.volumes > 0 && s->graph.volumes <= kMaxVolumes ? s->graph.volumes : 0;
		if ((int)was.size() != nv) {
			was.assign(nv, 0);
		}
		std::vector<uint8_t> is(nv, 0);
		const float* pos = (const float*)(ch + 0x30);
		for (const ScriptBox& b : s->graph.boxes) {
			if (b.sid < nv && !is[b.sid] && InBox(b.min, b.max, b.turn, pos)) {
				is[b.sid] = 1;
			}
		}
		int who = ch[0x1f] == 1 ? 1 : ch[0x1f] == 2 ? 2 : 0;   // 1 human, 2 VS (the game's character types)
		for (int v = 0; v < nv; v++) {
			if (is[v] == was[v]) {
				continue;
			}
			was[v] = is[v];
			if (follow) {
				s->reports.push_back({slot, who, v, is[v] != 0});   // to the host
			}
			else {
				Trigger(*s, v, who, slot, is[v] != 0, now);
			}
		}
	}
	LeaveCriticalSection(&g_lock);
}

}
