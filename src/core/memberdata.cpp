#include "core/memberdata.h"

#include "core/config.h"
#include "core/log.h"
#include "core/steam.h"

#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace xls {

namespace {

std::recursive_mutex g_mutex;
std::map<std::string, std::string> g_local;                         // key -> our value
std::map<std::string, std::vector<MemberDataListener>> g_listeners; // key -> listeners
std::set<uint64_t> g_lobbies;                                       // lobbies we're in
std::map<std::pair<uint64_t, std::string>, std::string> g_seen;     // (member, key) -> last value delivered

std::string SteamKey(const std::string& key)
{
	return Cfg().lobbyKeyPrefix + "m_" + key;
}

// Hands value to key's listeners when it differs from what this member last delivered.
void Deliver(uint64_t member, const std::string& key, const std::string& value)
{
	std::vector<MemberDataListener> listeners;
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		auto it = g_listeners.find(key);
		if (it == g_listeners.end()) {
			return;
		}
		std::string& last = g_seen[{member, key}];
		if (last == value) {
			return;
		}
		last = value;
		listeners = it->second;
	}
	XUID xuid = XuidFromSteamId(CSteamID((uint64)member));
	for (const MemberDataListener& listener : listeners) {
		listener(member, xuid, value);
	}
}

void ReadMember(uint64_t lobby, uint64_t member)
{
	if (!SteamReady() || member == SteamLocalId().ConvertToUint64()) {
		return;
	}
	std::vector<std::string> keys;
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		for (const auto& l : g_listeners) {
			keys.push_back(l.first);
		}
	}
	for (const std::string& key : keys) {
		const char* value = SteamMatchmaking()->GetLobbyMemberData(CSteamID((uint64)lobby), CSteamID((uint64)member), SteamKey(key).c_str());
		Deliver(member, key, value ? value : "");
	}
}

} // namespace

void MemberDataSet(const std::string& key, const std::string& value)
{
	std::vector<uint64_t> lobbies;
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		if (g_local[key] == value) {
			return;
		}
		g_local[key] = value;
		lobbies.assign(g_lobbies.begin(), g_lobbies.end());
	}
	if (!SteamReady()) {
		return;
	}
	for (uint64_t lobby : lobbies) {
		SteamMatchmaking()->SetLobbyMemberData(CSteamID((uint64)lobby), SteamKey(key).c_str(), value.c_str());
	}
	XLS_LOG_INFO("member data: %s = %zu byte(s), published to %zu lobby(s).", key.c_str(), value.size(), lobbies.size());
}

void MemberDataListen(const std::string& key, MemberDataListener listener)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_listeners[key].push_back(std::move(listener));
}

void MemberDataLobbyEntered(uint64_t lobby)
{
	std::map<std::string, std::string> local;
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_lobbies.insert(lobby);
		local = g_local;
	}
	if (!SteamReady()) {
		return;
	}
	ISteamMatchmaking* matchmaking = SteamMatchmaking();
	for (const auto& kv : local) {
		matchmaking->SetLobbyMemberData(CSteamID((uint64)lobby), SteamKey(kv.first).c_str(), kv.second.c_str());
	}
	int count = matchmaking->GetNumLobbyMembers(CSteamID((uint64)lobby));
	for (int i = 0; i < count; i++) {
		ReadMember(lobby, LobbyMember(matchmaking, CSteamID((uint64)lobby), i).ConvertToUint64());
	}
	XLS_LOG_INFO("member data: lobby %llu: published %zu key(s), read %d member(s).", (unsigned long long)lobby, local.size(), count);
}

void MemberDataMemberChanged(uint64_t lobby, uint64_t member)
{
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		if (!g_lobbies.count(lobby)) {
			return;
		}
	}
	ReadMember(lobby, member);
}

void MemberDataLobbyLeft(uint64_t lobby)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_lobbies.erase(lobby);
}

void MemberDataInject(uint64_t steamId, const std::string& key, const std::string& value)
{
	XLS_LOG_INFO("member data: injected %s for %llu (%zu byte(s)).", key.c_str(), (unsigned long long)steamId, value.size());
	Deliver(steamId, key, value);
}

} // namespace xls
