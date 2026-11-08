// LP2 mods: the lives core. One state per match slot (alive, dead with a respawn coming, out of the match), kept from what
// rules.cpp reports each controller update, and the decisions that follow from it: who an out player watches, and what
// the player is told (drawn on our overlay, ui.cpp, not through the game's battle messages). rules.cpp keeps doing the
// game-side work (holding the death view, the camera, removing bodies). Modes (Survival and Rounds now; Arena and
// scripted modes later) build on these states, so this is where queues, held players and spawn choices go next.
#include "mods/lp2.h"

#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

#include "third-party/imgui/imgui.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <random>

namespace xls::mods {

namespace {

struct Slot {
	LifeState state = LifeState::None;
	int team = -1;         // the team its body fought on while alive (setup team until one is seen)
	int respawnsLeft = -1; // -1: no limit
	DWORD seen = 0;
	char name[32] = {};
};
std::mutex g_mutex;
Slot g_slots[16];
std::atomic<int> g_local{-1}, g_watching{-1};
std::atomic<bool> g_teams{false}, g_survivalOn{false};
std::atomic<DWORD> g_matchSeen{0};

bool Live(const Slot& s)
{
	return s.state == LifeState::Alive && GetTickCount() - s.seen < 3000;
}

const char* StateName(LifeState s)
{
	switch (s) {
	case LifeState::Alive:
		return "alive";
	case LifeState::Dead:
		return "dead";
	case LifeState::Out:
		return "out";
	case LifeState::Held:
		return "held";
	default:
		return "none";
	}
}

// On-screen messages
struct Message {
	std::string text;
	int style;
	uint32_t rgb;
	DWORD until, from;
	int slot;
};
std::vector<Message> g_messages; // under g_mutex

// The Arena test (until modes run: arena-test.txt in the game folder holds how many fight at once, e.g. 2). Everyone
// else waits in line, held; when a fighter dies they go to the back of the line and the first in line comes in, at a
// script marker named arena_spawn_* when the map has them (where the game spawns them otherwise).
int g_arenaFighters = -1; // -1: not read yet, 0: off
std::vector<int> g_queue;   // under g_mutex
bool g_arenaStarted = false;
DWORD g_arenaStartAt = 0;
int g_arenaNextSpot = 0;

int ArenaFighters()
{
	if (g_arenaFighters >= 0) {
		return g_arenaFighters;
	}
	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	if (char* slash = strrchr(path, '\\')) {
		strcpy(slash + 1, "arena-test.txt");
	}
	g_arenaFighters = 0;
	if (FILE* f = fopen(path, "r")) {
		int n = 0;
		if (fscanf(f, "%d", &n) == 1 && n > 0) {
			g_arenaFighters = std::min(n, 15);
		}
		fclose(f);
		XLS_LOG_INFO("mods: lives: Arena test on (%s): %d fighting at once.", path, g_arenaFighters);
	}
	return g_arenaFighters;
}

// The overlay: bottom centre, only for the local player in a Survival match while the battle HUD runs.
void Draw(float width, float height)
{
	int self = g_local;
	if (self < 0 || self >= 16 || GetTickCount() - g_matchSeen > 500) {
		return;
	}
	Slot me, watched;
	int watching = g_watching;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		me = g_slots[self];
		if (watching >= 0 && watching < 16) {
			watched = g_slots[watching];
		}
	}
	ImFont* big = UiFont(true);
	ImFont* small = UiFont(false);
	if (!big || !small) {
		return;
	}
	float scale = height / 720.0f;
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	auto centred = [&](ImFont* f, const char* text, float y, ImU32 colour) {
		ImVec2 size = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, text);
		float x = (width - size.x) / 2;
		dl->AddText(f, f->FontSize, ImVec2(x + 2 * scale, y + 2 * scale), IM_COL32(0, 0, 0, 160), text);
		dl->AddText(f, f->FontSize, ImVec2(x, y), colour, text);
		return size.y;
	};
	// messages
	{
		std::vector<Message> list;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			DWORD now = GetTickCount();
			g_messages.erase(std::remove_if(g_messages.begin(), g_messages.end(), [&](const Message& m) { return (int)(now - m.until) >= 0; }),
			                 g_messages.end());
			list = g_messages;
		}
		float bannerY = height * 0.3f, topY = 70 * scale, hudY = height - 210 * scale;
		for (const Message& m : list) {
			if (m.slot >= 0 && m.slot != self) {
				continue;
			}
			DWORD now = GetTickCount();
			float fade = std::min({1.0f, (now - m.from) / 250.0f, (m.until - now) / 400.0f});
			ImU32 col = IM_COL32((m.rgb >> 16) & 255, (m.rgb >> 8) & 255, m.rgb & 255, (int)(255 * std::max(fade, 0.0f)));
			if (m.style == 0) {
				bannerY += centred(big, m.text.c_str(), bannerY, col) + 6 * scale;
			}
			else if (m.style == 1) {
				topY += centred(small, m.text.c_str(), topY, col) + 4 * scale;
			}
			else {
				hudY += centred(small, m.text.c_str(), hudY, col) + 4 * scale;
			}
		}
	}
	if (me.state == LifeState::Held) {
		int place = 0;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			auto it = std::find(g_queue.begin(), g_queue.end(), self);
			place = it == g_queue.end() ? 0 : (int)(it - g_queue.begin()) + 1;
		}
		float y = height - 150 * scale;
		y += centred(big, "WAITING", y, IM_COL32(120, 200, 255, 255));
		char line[96];
		snprintf(line, sizeof line, place ? "You're number %d in line" : "Waiting to come in", place);
		y += centred(small, line, y + 4 * scale, IM_COL32(235, 248, 255, 255)) + 4 * scale;
		if (watching >= 0 && watched.name[0]) {
			snprintf(line, sizeof line, "Watching %s", watched.name);
			centred(small, line, y + 4 * scale, IM_COL32(150, 210, 230, 220));
		}
	}
	else if (me.state == LifeState::Out) {
		float y = height - 150 * scale;
		y += centred(big, "ELIMINATED", y, IM_COL32(255, 72, 72, 255));
		char line[96];
		if (watching >= 0 && watched.name[0]) {
			snprintf(line, sizeof line, "Spectating %s", watched.name);
		}
		else {
			snprintf(line, sizeof line, g_teams ? "No teammates left to watch" : "Nobody left to watch");
		}
		y += centred(small, line, y + 4 * scale, IM_COL32(235, 248, 255, 255)) + 4 * scale;
		if (watching >= 0) {
			centred(small, "Fire: next player     Zoom: previous player", y + 4 * scale, IM_COL32(150, 210, 230, 220));
		}
	}
	else if (g_survivalOn && me.state == LifeState::Dead && me.respawnsLeft >= 0) {
		char line[64];
		if (me.respawnsLeft == 0) {
			snprintf(line, sizeof line, g_teams ? "No team respawns left" : "No respawns left");
		}
		else {
			snprintf(line, sizeof line, me.respawnsLeft == 1 ? "1 respawn left" : "%d respawns left", me.respawnsLeft);
		}
		centred(small, line, height - 110 * scale, IM_COL32(235, 248, 255, 255));
	}
}

}

void LivesReport(int slot, LifeState state, int team, int respawnsLeft, const char* name)
{
	if (slot < 0 || slot >= 16) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	Slot& s = g_slots[slot];
	if (s.state != state) {
		XLS_LOG_INFO("mods: lives: player %d %s -> %s (team %d, respawns left %d).", slot, StateName(s.state), StateName(state), team, respawnsLeft);
		// the game mode's events: a death (in play -> down), a spawn (back in play)
		if (s.state == LifeState::Alive && (state == LifeState::Dead || state == LifeState::Out)) {
			ModeEvent(kEvDeath, slot);
		}
		else if (state == LifeState::Alive && s.state != LifeState::None) {
			ModeEvent(kEvSpawn, slot);
		}
	}
	s.state = state;
	if (team >= 0) {
		s.team = team;
	}
	s.respawnsLeft = respawnsLeft;
	s.seen = GetTickCount();
	if (name) {
		snprintf(s.name, sizeof s.name, "%s", name);
	}
}

void LivesMatch(int local, bool survival, bool teams)
{
	g_local = local;
	g_survivalOn = survival;
	g_teams = teams;
	g_matchSeen = GetTickCount();
}

void LivesReset()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (Slot& s : g_slots) {
		s = Slot();
	}
	g_watching = -1;
	g_queue.clear();
	g_arenaStarted = false;
	g_arenaStartAt = 0;
	for (int slot = 0; slot < 16; slot++) {
		RulesHold(slot, false);
	}
}

void LivesMessage(const char* text, int style, uint32_t rgb, float seconds, int slot)
{
	if (!text || !*text) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	DWORD now = GetTickCount();
	g_messages.push_back({text, style, rgb, now + (DWORD)(std::max(seconds, 0.5f) * 1000), now, slot});
	XLS_LOG_INFO("mods: lives: message \"%s\" (style %d, %.1f s%s).", text, style, seconds, slot >= 0 ? FormatA(", player %d", slot).c_str() : "");
}

namespace {

// Next arena spot: the map's arena_spawn_* markers in turn (nullptr: none, the game's own spawn)
bool ArenaSpot(float pos[3], float* yaw)
{
	float found[16][4];
	int n = ScriptMarkerFind("arena_spawn_*", found, 16);
	if (n <= 0) {
		return false;
	}
	int k = g_arenaNextSpot++ % n;
	memcpy(pos, found[k], 12);
	*yaw = found[k][3];
	return true;
}

void ArenaBringIn(int slot)
{
	float pos[3], yaw = NAN;
	bool at = ArenaSpot(pos, &yaw);
	RulesRelease(slot, at ? pos : nullptr, yaw);
	char text[80];
	snprintf(text, sizeof text, "%s comes in", g_slots[slot].name[0] ? g_slots[slot].name : "Next player");
	g_messages.push_back({text, 1, 0xffffff, GetTickCount() + 3000, GetTickCount(), -1});
}

}

void LivesTick()
{
	int fighters = ArenaFighters();
	if (fighters <= 0 || GetTickCount() - g_matchSeen > 500) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	std::vector<int> present;
	for (int slot = 0; slot < 16; slot++) {
		if (g_slots[slot].state != LifeState::None && GetTickCount() - g_slots[slot].seen < 3000) {
			present.push_back(slot);
		}
	}
	if (!g_arenaStarted) {
		if (!g_arenaStartAt) {
			g_arenaStartAt = GetTickCount();
		}
		if (present.size() < 2 || GetTickCount() - g_arenaStartAt < 1500) { // let everyone spawn first
			return;
		}
		g_arenaStarted = true;
		std::vector<int> order = present;
		std::shuffle(order.begin(), order.end(), std::mt19937(GetTickCount()));
		g_queue.clear();
		for (size_t i = 0; i < order.size(); i++) {
			if ((int)i < fighters) {
				float pos[3], yaw = NAN;
				if (ArenaSpot(pos, &yaw)) {
					RulesRelease(order[i], pos, yaw); // to the arena spot (already in play: a teleport)
				}
			}
			else {
				g_queue.push_back(order[i]);
				RulesHold(order[i], true);
			}
		}
		XLS_LOG_INFO("mods: lives: Arena test: %d players, %d fight, %d wait.", (int)order.size(), std::min(fighters, (int)order.size()),
		             (int)g_queue.size());
		g_messages.push_back({"ARENA", 0, 0xffd040, GetTickCount() + 3000, GetTickCount(), -1});
		return;
	}
	// a fighter down: to the back of the line, the first in line comes in
	for (int slot : present) {
		Slot& s = g_slots[slot];
		bool queued = std::find(g_queue.begin(), g_queue.end(), slot) != g_queue.end();
		if ((s.state == LifeState::Dead || s.state == LifeState::Out) && !queued && !RulesHeld(slot)) {
			RulesHold(slot, true);
			g_queue.push_back(slot);
			XLS_LOG_INFO("mods: lives: Arena test: player %d down, back of the line (%d waiting).", slot, (int)g_queue.size());
			if (!g_queue.empty() && g_queue.front() != slot) {
				int next = g_queue.front();
				g_queue.erase(g_queue.begin());
				ArenaBringIn(next);
			}
			else if (!g_queue.empty()) { // nobody else waiting: straight back in
				g_queue.erase(g_queue.begin());
				ArenaBringIn(slot);
			}
		}
	}
}

LifeState LivesState(int slot)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return slot >= 0 && slot < 16 ? g_slots[slot].state : LifeState::None;
}

int LivesTeam(int slot)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return slot >= 0 && slot < 16 ? g_slots[slot].team : -1;
}

int LivesPickWatch(int self, int current, int dir)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	int myTeam = self >= 0 && self < 16 ? g_slots[self].team : -1;
	auto ok = [&](int slot) {
		const Slot& s = g_slots[slot];
		return slot != self && Live(s) && (!g_teams || myTeam < 0 || s.team == myTeam);
	};
	if (dir == 0 && current >= 0 && current < 16 && ok(current)) {
		return current;
	}
	int from = current >= 0 && current < 16 ? current : (self >= 0 ? self : 0);
	int step = dir < 0 ? -1 : 1;
	for (int i = 1; i <= 16; i++) {
		int slot = (from + step * i + 32) % 16;
		if (ok(slot)) {
			return slot;
		}
	}
	return -1;
}

void LivesWatching(int slot)
{
	int was = g_watching.exchange(slot);
	if (was != slot) {
		XLS_LOG_INFO("mods: lives: watching player %d (was %d).", slot, was);
	}
}

std::string LivesName(int slot)
{
	if (slot < 0 || slot >= 16) {
		return std::string();
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_slots[slot].name;
}

void LivesInstall()
{
	UiAddLayer("lives", Draw);
}

}
