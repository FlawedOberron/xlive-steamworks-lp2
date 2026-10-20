#include "core/content.h"

#include "core/log.h"
#include "core/memberdata.h"
#include "core/steam.h"
#include "core/transfer.h"
#include "core/utils.h"

#include <stdio.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace xls {

namespace {

std::mutex g_mutex;
std::vector<std::function<int()>> g_pending;
std::map<uint64_t, std::string> g_values; // member -> their "content"
uint64_t g_waitLobby = 0;
bool g_localReady = true;
int g_localPercent = 100;
int g_batchFiles = 0; // downloads in this batch (since nothing was pending), 0 = none
int g_batchDone = 0;  // of which finished
DWORD g_settleUntil = 0; // GetTickCount() until which lobbies count as not ready (0 = none)
const DWORD kSettleMs = 2500;

// "dl <file>/<files> <percent>" (or an older build's "dl <percent>"); false for "ready".
bool ParseProgress(const std::string& value, int* file, int* files, int* percent)
{
	if (value.compare(0, 3, "dl ") != 0) {
		return false;
	}
	if (sscanf(value.c_str() + 3, "%d/%d %d", file, files, percent) != 3) {
		*file = *files = 1;
		*percent = atoi(value.c_str() + 3);
	}
	return true;
}

int PercentOf(const std::string& value)
{
	int file, files, percent;
	return ParseProgress(value, &file, &files, &percent) ? percent : 100;
}

void Update()
{
	std::vector<std::function<int()>> checks;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		checks = g_pending;
	}
	int count = 0;
	for (const auto& check : checks) {
		count += std::max(0, check());
	}
	uint64_t received, total;
	int active = TransferProgress(&received, &total);
	count = std::max(count, active);
	bool pending = count > 0;
	int percent = !pending ? 100 : total ? (int)std::min<uint64_t>(received * 100 / total, 99) : 0;
	std::string value;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		// the batch: grows as downloads join it, done = what left the pending count
		int files = pending ? std::max(g_batchFiles, g_batchDone + count) : 0;
		int done = pending ? files - count : 0;
		bool same = pending == !g_localReady && percent == g_localPercent && files == g_batchFiles && done == g_batchDone;
		g_batchFiles = files;
		g_batchDone = done;
		if (same) {
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
		value = pending ? FormatA("dl %d/%d %d", done + 1, files, percent) : "ready";
	}
	MemberDataSet("content", value);
}

DWORD WINAPI Worker(LPVOID)
{
	for (;;) {
		Update();
		Sleep(500);
	}
}

}

void ContentAddPending(std::function<int()> pending)
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

bool ContentLocalProgress(int* file, int* files, int* percent)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_localReady) {
		return false;
	}
	*file = g_batchDone + 1;
	*files = g_batchFiles;
	*percent = g_localPercent;
	return true;
}

bool ContentMemberReady(uint64_t steamId)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_values.find(steamId);
	return it != g_values.end() && it->second == "ready";
}

bool ContentMemberProgress(uint64_t steamId, int* file, int* files, int* percent)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_values.find(steamId);
	return it != g_values.end() && ParseProgress(it->second, file, files, percent);
}

std::string ContentProgressText(int file, int files, int percent)
{
	return FormatA("Downloading %d/%d [%d%%]", file, files, percent);
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
			return ContentProgressText(g_batchDone + 1, g_batchFiles, g_localPercent);
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
