// The network relay's wire format (core/relay.cpp, relay/main.cpp). A Steam copy can play in another app's lobbies
// (Spacewar, 480, which disc copies use): xlive_relay.exe signs into Steam as that app and does the lobbies and peer
// messages for the game process, over two anonymous pipes. Frames: Frame, then `size` payload bytes. Requests carry a
// seq; the reply has the same op and seq. Events (op >= kEvent) come unasked.
#pragma once

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

namespace xls::relay {

struct Frame {
	uint32_t size; // payload bytes
	uint16_t op;
	uint16_t flags;
	uint32_t seq;  // 0: no reply wanted
};

enum Op : uint16_t {
	// helper start: payload = app id (u32), then the helper's Steam id (u64) in its reply
	kHello = 1,
	// matchmaking
	kCreateLobby, kJoinLobby, kRequestLobbyList, kAddStringFilter, kAddNumericalFilter, kAddNearFilter,
	kAddSlotsFilter, kAddDistanceFilter, kAddResultCountFilter, kGetLobbyByIndex, kGetLobbyData, kSetLobbyData,
	kDeleteLobbyData, kGetLobbyDataCount, kGetLobbyDataByIndex, kGetLobbyMemberData, kSetLobbyMemberData,
	kGetNumLobbyMembers, kGetLobbyMemberByIndex, kGetLobbyOwner, kSetLobbyOwner, kLeaveLobby, kInviteUserToLobby,
	kRequestLobbyData, kSetLobbyJoinable, kSetLobbyType, kSetLobbyMemberLimit, kGetLobbyMemberLimit,
	kSendLobbyChatMsg,
	// peer messages
	kSendMessage, kListenChannel, kAcceptSession, kCloseSession, kCloseChannel, kSessionInfo,
	// networking utils
	kSetConfigInt32, kInitRelayNetwork,
	// events
	kEvent = 0x8000,
	kEvCallback,   // i32 callback id, then its struct
	kEvCallResult, // u64 call, i32 callback id, u8 failed, then its struct
	kEvMessage,    // i32 channel, SteamNetworkingIdentity, data
};

// Payload writer / reader: little endian, strings as u32 length + bytes.
struct Out {
	std::vector<uint8_t> b;
	Out& raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); return *this; }
	template <typename T> Out& put(T v) { return raw(&v, sizeof v); }
	Out& str(const char* s) { uint32_t n = s ? (uint32_t)strlen(s) : 0; put(n); return raw(s, n); }
};

struct In {
	const uint8_t* p;
	size_t n;
	bool ok = true;
	In(const void* data, size_t size) : p((const uint8_t*)data), n(size) {}
	bool raw(void* out, size_t k)
	{
		if (k > n) { ok = false; memset(out, 0, k); return false; }
		memcpy(out, p, k); p += k; n -= k;
		return true;
	}
	template <typename T> T get() { T v{}; raw(&v, sizeof v); return v; }
	std::string str()
	{
		uint32_t k = get<uint32_t>();
		if (k > n) { ok = false; return {}; }
		std::string s((const char*)p, k); p += k; n -= k;
		return s;
	}
};

}
