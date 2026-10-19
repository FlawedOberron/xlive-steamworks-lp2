#pragma once

#include "xlive/xdefs.h"

#include <cstdint>
#include <functional>
#include <string>

namespace xls {

// Per-member key/value data in the session lobbies (Steam lobby member data, keys
// "<lobbyKeyPrefix>m_<key>"). Mods publish what the other players need to know about this player
// (worn custom parts, ...) and listen for theirs. Listeners run on the thread that delivered the
// data (Steam callbacks or the injecting thread) and get the member's Steam id and XUID.
using MemberDataListener = std::function<void(uint64_t steamId, XUID xuid, const std::string& value)>;

// This player's value for key, published to every lobby joined now or later ("" = none).
void MemberDataSet(const std::string& key, const std::string& value);
void MemberDataListen(const std::string& key, MemberDataListener listener);

// Sessions: a lobby was created or joined (publish ours, read everyone's), a member's data
// changed, or the lobby was left.
void MemberDataLobbyEntered(uint64_t lobby);
void MemberDataMemberChanged(uint64_t lobby, uint64_t member);
void MemberDataLobbyLeft(uint64_t lobby);

// Tests without a second client: deliver value as if member steamId had published it.
void MemberDataInject(uint64_t steamId, const std::string& key, const std::string& value);

} // namespace xls
