// LP2 mods: the lives core. One state per match slot (alive, dead with a respawn coming, out of the match), kept from what
// rules.cpp reports each controller update, and the decisions that follow from it: who an out player watches, and what
// the player is told (drawn on our overlay, ui.cpp, not through the game's battle messages). rules.cpp keeps doing the
// game-side work (holding the death view, the camera, removing bodies). Modes (Survival and Rounds now; Arena and
// scripted modes later) build on these states, so this is where queues, held players and spawn choices go next.
#include "mods/lp2.h"

#include "core/log.h"

#include <windows.h>

#include "third-party/imgui/imgui.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

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
	default:
		return "none";
	}
}

// The overlay: bottom centre, only for the local player in a Survival match while the battle HUD runs.
void Draw(float width, float height)
{
	int self = g_local;
	if (!g_survivalOn || self < 0 || self >= 16 || GetTickCount() - g_matchSeen > 500) {
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
	if (me.state == LifeState::Out) {
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
	else if (me.state == LifeState::Dead && me.respawnsLeft >= 0) {
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

void LivesInstall()
{
	UiAddLayer("lives", Draw);
}

}
