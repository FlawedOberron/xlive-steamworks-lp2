// Voice chat rules: who is heard in a match. The game's own voice code (NetworkXboxVoice) sends every player's voice
// to every machine and only knows mute lists, so the rules are applied where a talker's voice is played (api/xhv.cpp
// asks VoiceGain for every incoming frame). Two rules from the host (rules.cpp extras voiceTeam / voiceEnemy; outside
// team games everyone counts as an enemy):
//   Always  heard by everyone
//   Living  the living hear only the living; the dead hear everyone
//   Nearby  as Living, and a living talker only within kNearEnd of the listener (fading out from kNearFull); a dead
//           listener hears from where its camera is
//   None    never heard
// A talker is matched to its match slot by lobby member data "vslot": every machine publishes its own slot from the
// battle HUD. Outside a match (no battle HUD for kFrameStale), and for a talker whose slot isn't known (a player on an
// older build), everyone is heard.
#include "mods/lp2.h"

#include "core/log.h"
#include "core/memberdata.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace xls::mods {

namespace {

enum { kVoiceAlways, kVoiceLiving, kVoiceNearby, kVoiceNone };
const float kNearFull = 1500.0f, kNearEnd = 3000.0f; // cm
const DWORD kFrameStale = 2000;
const uint32_t kBodyPos = 0x30;

std::atomic<int> g_team{0}, g_enemy{0};
std::atomic<DWORD> g_frameAt{0};
std::atomic<float> g_gain[16];
std::atomic<int> g_published{-2};
std::mutex g_mutex;
std::map<uint64_t, int> g_slotOf; // by XUID
bool g_heard[16];                 // game thread: for the log
int g_logged = 0;

const char* ModeName(int mode)
{
	static const char* const names[] = {"Always", "Living", "Nearby", "None"};
	return names[mode & 3];
}

}

void VoiceRulesSet(int team, int enemy)
{
	int t = team & 3, e = enemy & 3;
	if (g_team.exchange(t) != t || g_enemy.exchange(e) != e) {
		XLS_LOG_INFO("mods: voice: team voice %s, enemy voice %s.", ModeName(t), ModeName(e));
	}
}

void VoiceInstall()
{
	for (auto& g : g_gain) {
		g = 1.0f;
	}
	for (bool& h : g_heard) {
		h = true;
	}
	MemberDataListen("vslot", [](uint64_t, XUID xuid, const std::string& value) {
		int slot = value.empty() ? -1 : atoi(value.c_str());
		std::lock_guard<std::mutex> lock(g_mutex);
		if (slot >= 0 && slot < 16) {
			g_slotOf[xuid] = slot;
		}
		else {
			g_slotOf.erase(xuid);
		}
	});
}

void VoiceFrame()
{
	int self = RulesLocalSlot();
	if (g_published.exchange(self) != self) {
		MemberDataSet("vslot", self >= 0 && self < 16 ? std::to_string(self) : std::string());
	}
	if (self < 0 || self >= 16) {
		return;
	}
	bool teams = RulesTeams();
	int myTeam = RulesTeamOf(self), teamMode = g_team, enemyMode = g_enemy;
	bool selfAlive = LivesState(self) == LifeState::Alive;
	// the listener's ear: its body, else (dead, spectating) the camera
	float ear[3] = {}, up[3], target[3], fov = 0;
	uint8_t* body = selfAlive ? RulesBody(self) : nullptr;
	bool haveEar = true;
	if (body) {
		memcpy(ear, body + kBodyPos, sizeof ear);
	}
	else {
		haveEar = RulesCamera(ear, target, up, &fov);
	}
	for (int slot = 0; slot < 16; slot++) {
		float gain = 1.0f;
		int mode = kVoiceAlways;
		bool mate = false;
		if (slot != self && RulesInPlay(slot)) {
			mate = teams && myTeam >= 0 && RulesTeamOf(slot) == myTeam;
			mode = mate ? teamMode : enemyMode;
			bool alive = LivesState(slot) == LifeState::Alive;
			uint8_t* theirs = alive ? RulesBody(slot) : nullptr;
			if (mode == kVoiceNone) {
				gain = 0.0f;
			}
			else if (mode != kVoiceAlways && !alive) {
				gain = selfAlive ? 0.0f : 1.0f; // the dead talk among themselves
			}
			else if (mode == kVoiceNearby && theirs && haveEar) {
				const float* p = (const float*)(theirs + kBodyPos);
				float dx = p[0] - ear[0], dy = p[1] - ear[1], dz = p[2] - ear[2];
				float d = sqrtf(dx * dx + dy * dy + dz * dz);
				gain = d <= kNearFull ? 1.0f : d >= kNearEnd ? 0.0f : (kNearEnd - d) / (kNearEnd - kNearFull);
			}
		}
		g_gain[slot] = gain;
		bool heard = gain > 0.0f;
		if (heard != g_heard[slot]) {
			g_heard[slot] = heard;
			if (g_logged < 200) {
				g_logged++;
				XLS_LOG_INFO("mods: voice: player %d (%s, %s voice) %s here.", slot, mate ? "team mate" : "enemy", ModeName(mode), heard ? "can be heard" : "can't be heard");
			}
		}
	}
	g_frameAt = GetTickCount();
}

float VoiceGain(uint64_t xuid)
{
	if (GetTickCount() - g_frameAt > kFrameStale) {
		return 1.0f; // no match running: the lobby, loading, results
	}
	int slot = -1;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_slotOf.find(xuid);
		slot = it != g_slotOf.end() ? it->second : -1;
	}
	return slot >= 0 ? g_gain[slot].load() : 1.0f;
}

}
