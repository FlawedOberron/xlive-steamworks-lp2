#include "core/content.h"

#include "core/log.h"
#include "core/memberdata.h"
#include "core/steam.h"
#include "core/transfer.h"
#include "core/utils.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace xls {

namespace {

std::mutex g_mutex;
std::vector<std::function<bool()>> g_pending;
std::map<uint64_t, std::string> g_values; // member -> their "content"
uint64_t g_waitLobby = 0;
bool g_localReady = true;
int g_localPercent = 100;
DWORD g_settleUntil = 0; // GetTickCount() until which lobbies count as not ready (0 = none)
const DWORD kSettleMs = 2500;

// "dl <percent>" -> percent; "ready" (or anything else) -> 100.
int PercentOf(const std::string& value)
{
	return value.compare(0, 3, "dl ") == 0 ? atoi(value.c_str() + 3) : 100;
}

void Update()
{
	std::vector<std::function<bool()>> checks;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		checks = g_pending;
	}
	bool pending = false;
	for (const auto& check : checks) {
		pending = check() || pending;
	}
	uint64_t received, total;
	int active = TransferProgress(&received, &total);
	pending = pending || active > 0;
	int percent = !pending ? 100 : total ? (int)std::min<uint64_t>(received * 100 / total, 99) : 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (pending == !g_localReady && percent == g_localPercent) {
			return;
		}
		if (pending && g_localReady) {
			XLS_LOG_INFO("content: downloading; match starts wait for it.");
		}
		else if (!pending) {
			XLS_LOG_INFO("content: everything here.");
		}
		g_localReady = !pending;
		g_localPercent = percent;
	}
	MemberDataSet("content", pending ? FormatA("dl %d", percent) : "ready");
}

DWORD WINAPI Worker(LPVOID)
{
	for (;;) {
		Update();
		Sleep(500);
	}
}

}

void ContentAddPending(std::function<bool()> pending)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_pending.push_back(std::move(pending));
}

void ContentStart()
{
	static std::once_flag once;
	std::call_once(once, [] {
		MemberDataListen("content", [](uint64_t steamId, XUID, const std::string& value) {
			std::lock_guard<std::mutex> lock(g_mutex);
			g_values[steamId] = value;
		});
		MemberDataSet("content", "ready");
		HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
		if (t) {
			CloseHandle(t);
		}
	});
}

bool ContentLocalReady()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_localReady;
}

bool ContentLobbyReady(uint64_t lobby, int* waiting, int* percent)
{
	std::vector<uint64_t> members;
	uint64_t self = 0;
	if (lobby && SteamReady() && SteamMatchmaking()) {
		ISteamMatchmaking* mm = SteamMatchmaking();
		int count = mm->GetNumLobbyMembers(CSteamID((uint64)lobby));
		for (int i = 0; i < count; i++) {
			members.push_back(LobbyMember(mm, CSteamID((uint64)lobby), i).ConvertToUint64());
		}
		self = SteamLocalId().ConvertToUint64();
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	*waiting = 0;
	*percent = 100;
	if (g_settleUntil && (int32_t)(GetTickCount() - g_settleUntil) < 0) {
		++*waiting; // a member just arrived: their content isn't known yet
		*percent = 0;
	}
	if (!g_localReady) {
		++*waiting;
		*percent = g_localPercent;
	}
	for (uint64_t member : members) {
		auto it = g_values.find(member);
		if (member == self || it == g_values.end() || PercentOf(it->second) >= 100) {
			continue;
		}
		++*waiting;
		*percent = std::min(*percent, PercentOf(it->second));
	}
	return *waiting == 0;
}

void ContentSettle()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_settleUntil = (GetTickCount() + kSettleMs) | 1;
}

void ContentStartWaiting(uint64_t lobby)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_waitLobby = lobby;
}

std::string ContentStatusText()
{
	uint64_t lobby;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_localReady) {
			return FormatA("Downloading content (%d%%)", g_localPercent);
		}
		lobby = g_waitLobby;
	}
	int waiting, percent;
	if (!lobby || ContentLobbyReady(lobby, &waiting, &percent)) {
		return std::string();
	}
	return FormatA("Waiting for %d player%s to download (%d%%)", waiting, waiting == 1 ? "" : "s", percent);
}

}
