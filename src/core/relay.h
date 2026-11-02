// Lobbies and peer messages as another Steam app (config steam.network_app_id, e.g. Spacewar 480 to play with disc
// copies), done by xlive_relay.exe (relay/main.cpp) over pipes. While it runs, xls::SteamMatchmaking() and
// xls::SteamNetworkingMessages() return proxies that forward to it (core/steam.h); everything else stays this
// process's own Steam.
#pragma once

#include <stdint.h>

class ISteamMatchmaking;
class ISteamNetworkingMessages;

namespace xls::relay {

bool Start(uint32_t appId); // launches the helper; true once it signed in
bool Active();
void Pump();                // the helper's callbacks to the modules (game thread, from SteamPumpForce)

ISteamMatchmaking* Matchmaking();
ISteamNetworkingMessages* Messages();
bool IsProxy(const ISteamMatchmaking* mm);
uint64_t LobbyOwner(uint64_t lobby);
uint64_t LobbyMember(uint64_t lobby, int i);
uint64_t LobbyByIndex(int i);
bool SetConfigInt32(int value, int32_t v); // a networking setting, in the helper (where the traffic is)

// One of the helper's async calls: -1 not the helper's, 0 pending, 1 done (out filled unless failed).
int CallPoll(uint64_t call, void* out, int size, int callback, bool* failed);

}
