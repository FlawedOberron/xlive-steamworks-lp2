// xlive_relay.exe: Steam as another app for the game process (core/relay.cpp starts it). A Steam copy's process is
// Lost Planet 2 (45750); disc copies sign in as Spacewar (480). This helper signs in as the network app and does the
// game's lobbies and peer messages there, so both kinds of copy meet in one lobby list.
//   xlive_relay.exe <read handle> <write handle> <app id> <log path>
// It exits when the game closes its pipe. Callbacks: SteamAPI_RunCallbacks with MSVC-layout callback objects (as
// core/steam.cpp; Steam's manual dispatch delivered nothing under Proton); call results are polled. The
// CSteamID-returning methods go through the flat API.
#include "core/relay_proto.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>

#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <steam/steam_api.h>
#include <steam/steam_api_flat.h>
#include <thread>
#include <vector>

using namespace xls::relay;

namespace {

HANDLE g_in = INVALID_HANDLE_VALUE, g_out = INVALID_HANDLE_VALUE;
FILE* g_log = nullptr;
std::mutex g_queueMutex;
std::vector<std::pair<Frame, std::vector<uint8_t>>> g_queue;
HANDLE g_wake = nullptr;
volatile bool g_quit = false;
std::set<int> g_channels; // channels the game reads
struct Pending {
	int callback, size;
};
std::map<SteamAPICall_t, Pending> g_pending; // the game's async calls

void Log(const char* fmt, ...)
{
	if (!g_log) {
		return;
	}
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list a;
	va_start(a, fmt);
	vfprintf(g_log, fmt, a);
	va_end(a);
	fputc('\n', g_log);
	fflush(g_log);
}

bool ReadAll(void* p, DWORD n)
{
	while (n) {
		DWORD got = 0;
		if (!ReadFile(g_in, p, n, &got, nullptr) || !got) {
			return false;
		}
		p = (uint8_t*)p + got;
		n -= got;
	}
	return true;
}

void Send(uint16_t op, uint32_t seq, const Out& o)
{
	Frame f{(uint32_t)o.b.size(), op, 0, seq};
	std::vector<uint8_t> all(sizeof f + o.b.size());
	memcpy(all.data(), &f, sizeof f);
	if (!o.b.empty()) {
		memcpy(all.data() + sizeof f, o.b.data(), o.b.size());
	}
	DWORD done = 0;
	if (!WriteFile(g_out, all.data(), (DWORD)all.size(), &done, nullptr)) {
		g_quit = true;
	}
}

void Reader()
{
	for (;;) {
		Frame f;
		if (!ReadAll(&f, sizeof f)) {
			break;
		}
		std::vector<uint8_t> body(f.size);
		if (f.size && !ReadAll(body.data(), f.size)) {
			break;
		}
		{
			std::lock_guard<std::mutex> lock(g_queueMutex);
			g_queue.emplace_back(f, std::move(body));
		}
		SetEvent(g_wake);
	}
	g_quit = true;
	SetEvent(g_wake);
}

ISteamMatchmaking* Mm() { return SteamMatchmaking(); }

template <typename T> SteamAPICall_t Track(SteamAPICall_t call)
{
	if (call != k_uAPICallInvalid) {
		g_pending[call] = {T::k_iCallback, (int)sizeof(T)};
	}
	return call;
}

// steam_api.dll is MSVC: its CCallbackBase vtable is { Run(param, ioFailure, call), Run(param), size } (see
// core/steam.cpp). Each forwards its struct to the game.
class MsvcCallbackBase {
public:
	virtual void RunCallResult(void* param, bool, SteamAPICall_t) { Run(param); }
	virtual void Run(void* param) = 0;
	virtual int GetCallbackSizeBytes() = 0;

protected:
	uint8 m_nCallbackFlags = 0;
	int m_iCallback = 0;
};

template <class P> class Forward final : public MsvcCallbackBase {
public:
	Forward()
	{
		m_iCallback = P::k_iCallback;
		SteamAPI_RegisterCallback(reinterpret_cast<CCallbackBase*>(this), P::k_iCallback);
	}
	void Run(void* param) override
	{
		Out o;
		o.put<int32_t>(P::k_iCallback).raw(param, sizeof(P));
		Send(kEvCallback, 0, o);
	}
	int GetCallbackSizeBytes() override { return (int)sizeof(P); }
};

void Register()
{
	new Forward<LobbyChatUpdate_t>();
	new Forward<LobbyDataUpdate_t>();
	new Forward<LobbyKicked_t>();
	new Forward<LobbyInvite_t>();
	new Forward<GameLobbyJoinRequested_t>();
	new Forward<SteamNetworkingMessagesSessionRequest_t>();
	new Forward<SteamNetworkingMessagesSessionFailed_t>();
}

struct Waiting {
	SteamNetworkingIdentity id;
	int flags, channel;
	std::vector<uint8_t> data;
};
std::deque<Waiting> g_waiting; // reliable sends that found Steam's send buffer full (main thread only)

void SendWaiting()
{
	while (!g_waiting.empty()) {
		Waiting& w = g_waiting.front();
		EResult r = SteamNetworkingMessages()->SendMessageToUser(w.id, w.data.data(), (uint32)w.data.size(), w.flags, w.channel);
		if (r == k_EResultLimitExceeded) {
			return;
		}
		if (r != k_EResultOK) {
			Log("send to %llu on %d: EResult %d", (unsigned long long)w.id.GetSteamID64(), w.channel, (int)r);
		}
		g_waiting.pop_front();
	}
}

void Handle(const Frame& f, In& in)
{
	Out o;
	switch (f.op) {
	case kCreateLobby: {
		ELobbyType type = (ELobbyType)in.get<int32_t>();
		int max = in.get<int32_t>();
		o.put<uint64_t>(Track<LobbyCreated_t>(Mm()->CreateLobby(type, max)));
		break;
	}
	case kJoinLobby: o.put<uint64_t>(Track<LobbyEnter_t>(Mm()->JoinLobby(in.get<uint64_t>()))); break;
	case kRequestLobbyList: o.put<uint64_t>(Track<LobbyMatchList_t>(Mm()->RequestLobbyList())); break;
	case kAddStringFilter: {
		std::string k = in.str(), v = in.str();
		Mm()->AddRequestLobbyListStringFilter(k.c_str(), v.c_str(), (ELobbyComparison)in.get<int32_t>());
		break;
	}
	case kAddNumericalFilter: {
		std::string k = in.str();
		int v = in.get<int32_t>();
		Mm()->AddRequestLobbyListNumericalFilter(k.c_str(), v, (ELobbyComparison)in.get<int32_t>());
		break;
	}
	case kAddNearFilter: {
		std::string k = in.str();
		Mm()->AddRequestLobbyListNearValueFilter(k.c_str(), in.get<int32_t>());
		break;
	}
	case kAddSlotsFilter: Mm()->AddRequestLobbyListFilterSlotsAvailable(in.get<int32_t>()); break;
	case kAddDistanceFilter: Mm()->AddRequestLobbyListDistanceFilter((ELobbyDistanceFilter)in.get<int32_t>()); break;
	case kAddResultCountFilter: Mm()->AddRequestLobbyListResultCountFilter(in.get<int32_t>()); break;
	case kGetLobbyByIndex: o.put<uint64_t>(SteamAPI_ISteamMatchmaking_GetLobbyByIndex(Mm(), in.get<int32_t>())); break;
	case kGetLobbyData: {
		uint64_t lobby = in.get<uint64_t>();
		std::string k = in.str();
		o.str(Mm()->GetLobbyData(lobby, k.c_str()));
		break;
	}
	case kSetLobbyData: {
		uint64_t lobby = in.get<uint64_t>();
		std::string k = in.str(), v = in.str();
		o.put<uint8_t>(Mm()->SetLobbyData(lobby, k.c_str(), v.c_str()));
		break;
	}
	case kDeleteLobbyData: {
		uint64_t lobby = in.get<uint64_t>();
		std::string k = in.str();
		o.put<uint8_t>(Mm()->DeleteLobbyData(lobby, k.c_str()));
		break;
	}
	case kGetLobbyDataCount: o.put<int32_t>(Mm()->GetLobbyDataCount(in.get<uint64_t>())); break;
	case kGetLobbyDataByIndex: {
		uint64_t lobby = in.get<uint64_t>();
		int i = in.get<int32_t>();
		char k[256] = {}, v[8192] = {};
		bool ok = Mm()->GetLobbyDataByIndex(lobby, i, k, sizeof k, v, sizeof v);
		o.put<uint8_t>(ok).str(k).str(v);
		break;
	}
	case kGetLobbyMemberData: {
		uint64_t lobby = in.get<uint64_t>(), user = in.get<uint64_t>();
		std::string k = in.str();
		o.str(Mm()->GetLobbyMemberData(lobby, user, k.c_str()));
		break;
	}
	case kSetLobbyMemberData: {
		uint64_t lobby = in.get<uint64_t>();
		std::string k = in.str(), v = in.str();
		Mm()->SetLobbyMemberData(lobby, k.c_str(), v.c_str());
		break;
	}
	case kGetNumLobbyMembers: o.put<int32_t>(Mm()->GetNumLobbyMembers(in.get<uint64_t>())); break;
	case kGetLobbyMemberByIndex: {
		uint64_t lobby = in.get<uint64_t>();
		o.put<uint64_t>(SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(Mm(), lobby, in.get<int32_t>()));
		break;
	}
	case kGetLobbyOwner: o.put<uint64_t>(SteamAPI_ISteamMatchmaking_GetLobbyOwner(Mm(), in.get<uint64_t>())); break;
	case kSetLobbyOwner: {
		uint64_t lobby = in.get<uint64_t>();
		o.put<uint8_t>(Mm()->SetLobbyOwner(lobby, in.get<uint64_t>()));
		break;
	}
	case kLeaveLobby: Mm()->LeaveLobby(in.get<uint64_t>()); break;
	case kInviteUserToLobby: {
		uint64_t lobby = in.get<uint64_t>();
		o.put<uint8_t>(Mm()->InviteUserToLobby(lobby, in.get<uint64_t>()));
		break;
	}
	case kRequestLobbyData: o.put<uint8_t>(Mm()->RequestLobbyData(in.get<uint64_t>())); break;
	case kSetLobbyJoinable: {
		uint64_t lobby = in.get<uint64_t>();
		o.put<uint8_t>(Mm()->SetLobbyJoinable(lobby, in.get<uint8_t>() != 0));
		break;
	}
	case kSetLobbyType: {
		uint64_t lobby = in.get<uint64_t>();
		o.put<uint8_t>(Mm()->SetLobbyType(lobby, (ELobbyType)in.get<int32_t>()));
		break;
	}
	case kSetLobbyMemberLimit: {
		uint64_t lobby = in.get<uint64_t>();
		o.put<uint8_t>(Mm()->SetLobbyMemberLimit(lobby, in.get<int32_t>()));
		break;
	}
	case kGetLobbyMemberLimit: o.put<int32_t>(Mm()->GetLobbyMemberLimit(in.get<uint64_t>())); break;
	case kSendLobbyChatMsg: {
		uint64_t lobby = in.get<uint64_t>();
		o.put<uint8_t>(Mm()->SendLobbyChatMsg(lobby, in.p, (int)in.n));
		break;
	}
	case kSendMessage: {
		SteamNetworkingIdentity id = in.get<SteamNetworkingIdentity>();
		int flags = in.get<int32_t>(), channel = in.get<int32_t>();
		// the game side doesn't wait for the answer, so a full send buffer can't be retried there: reliable
		// messages wait here, in order, behind any already waiting
		if (g_waiting.empty() || !(flags & k_nSteamNetworkingSend_Reliable)) {
			EResult r = SteamNetworkingMessages()->SendMessageToUser(id, in.p, (uint32)in.n, flags, channel);
			if (r == k_EResultOK) {
				break;
			}
			if (r != k_EResultLimitExceeded || !(flags & k_nSteamNetworkingSend_Reliable)) {
				Log("send to %llu on %d: EResult %d", (unsigned long long)id.GetSteamID64(), channel, (int)r);
				break;
			}
		}
		g_waiting.push_back({id, flags, channel, std::vector<uint8_t>(in.p, in.p + in.n)});
		break;
	}
	case kListenChannel: g_channels.insert(in.get<int32_t>()); break;
	case kAcceptSession: o.put<uint8_t>(SteamNetworkingMessages()->AcceptSessionWithUser(in.get<SteamNetworkingIdentity>())); break;
	case kCloseSession: o.put<uint8_t>(SteamNetworkingMessages()->CloseSessionWithUser(in.get<SteamNetworkingIdentity>())); break;
	case kCloseChannel: {
		SteamNetworkingIdentity id = in.get<SteamNetworkingIdentity>();
		o.put<uint8_t>(SteamNetworkingMessages()->CloseChannelWithUser(id, in.get<int32_t>()));
		break;
	}
	case kSessionInfo: {
		SteamNetConnectionInfo_t info = {};
		SteamNetConnectionRealTimeStatus_t status = {};
		ESteamNetworkingConnectionState s = SteamNetworkingMessages()->GetSessionConnectionInfo(in.get<SteamNetworkingIdentity>(), &info, &status);
		o.put<int32_t>(s).put(info).put(status);
		break;
	}
	case kSetConfigInt32: {
		ESteamNetworkingConfigValue v = (ESteamNetworkingConfigValue)in.get<int32_t>();
		o.put<uint8_t>(SteamNetworkingUtils()->SetGlobalConfigValueInt32(v, in.get<int32_t>()));
		break;
	}
	case kInitRelayNetwork: SteamNetworkingUtils()->InitRelayNetworkAccess(); break;
	case kCloudWrite: {
		std::string name = in.str(), data = in.str();
		ISteamRemoteStorage* rs = SteamRemoteStorage();
		o.put<uint8_t>(rs && !name.empty() && rs->FileWrite(name.c_str(), data.data(), (int32)data.size()));
		break;
	}
	case kCloudRead: {
		std::string name = in.str();
		ISteamRemoteStorage* rs = SteamRemoteStorage();
		int32 size = rs && !name.empty() && rs->FileExists(name.c_str()) ? rs->GetFileSize(name.c_str()) : 0;
		std::string data(size > 0 ? (size_t)size : 0, '\0');
		bool ok = size > 0 && rs->FileRead(name.c_str(), &data[0], size) == size;
		o.put<uint8_t>(ok);
		o.bytes(data.data(), ok ? data.size() : 0);
		break;
	}
	default: Log("unknown op %u", f.op); break;
	}
	if (f.seq) {
		Send(f.op, f.seq, o);
	}
}

void Results()
{
	for (auto it = g_pending.begin(); it != g_pending.end();) {
		bool failed = false;
		if (!SteamUtils()->IsAPICallCompleted(it->first, &failed)) {
			++it;
			continue;
		}
		std::vector<uint8_t> result(it->second.size);
		bool got = !failed && SteamUtils()->GetAPICallResult(it->first, result.data(), it->second.size, it->second.callback, &failed);
		Out o;
		o.put<uint64_t>(it->first).put<int32_t>(it->second.callback).put<uint8_t>(failed || !got).raw(result.data(), result.size());
		Send(kEvCallResult, 0, o);
		it = g_pending.erase(it);
	}
}

void Messages()
{
	ISteamNetworkingMessages* m = SteamNetworkingMessages();
	for (int channel : g_channels) {
		SteamNetworkingMessage_t* msgs[64];
		int n = m->ReceiveMessagesOnChannel(channel, msgs, 64);
		for (int i = 0; i < n; i++) {
			Out o;
			o.put<int32_t>(channel).put(msgs[i]->m_identityPeer).raw(msgs[i]->m_pData, (size_t)msgs[i]->m_cbSize);
			Send(kEvMessage, 0, o);
			msgs[i]->Release();
		}
	}
}

}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argc < 4) {
		return 2;
	}
	g_in = (HANDLE)(uintptr_t)_wcstoui64(argv[1], nullptr, 10);
	g_out = (HANDLE)(uintptr_t)_wcstoui64(argv[2], nullptr, 10);
	unsigned app = (unsigned)wcstoul(argv[3], nullptr, 10);
	if (argc > 4) {
		g_log = _wfopen(argv[4], L"w");
	}
	char appText[16];
	snprintf(appText, sizeof appText, "%u", app);
	for (const char* k : {"SteamAppId", "SteamGameId", "SteamOverlayGameId"}) {
		SetEnvironmentVariableA(k, appText);
	}
	SteamErrMsg err = {};
	ESteamAPIInitResult r = SteamAPI_InitEx(&err);
	Out hello;
	hello.put<uint8_t>(r == k_ESteamAPIInitResult_OK);
	if (r != k_ESteamAPIInitResult_OK) {
		Log("Steam as app %u failed (%d): %s", app, (int)r, err);
		hello.put<uint64_t>(0).str(err);
		Send(kHello, 0, hello);
		return 1;
	}
	uint64_t me = SteamAPI_ISteamUser_GetSteamID(SteamUser());
	Log("signed in as app %u, user %llu", SteamUtils()->GetAppID(), (unsigned long long)me);
	hello.put<uint64_t>(me).str("");
	Send(kHello, 0, hello);
	SteamNetworkingUtils()->InitRelayNetworkAccess();
	Register();
	g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	std::thread(Reader).detach();
	while (!g_quit) {
		WaitForSingleObject(g_wake, 1);
		std::vector<std::pair<Frame, std::vector<uint8_t>>> work;
		{
			std::lock_guard<std::mutex> lock(g_queueMutex);
			work.swap(g_queue);
		}
		for (auto& w : work) {
			In in(w.second.data(), w.second.size());
			Handle(w.first, in);
		}
		SendWaiting();
		SteamAPI_RunCallbacks();
		Results();
		Messages();
	}
	Log("the game closed the pipe; leaving");
	// Steam's networking thread may still be closing sessions: ExitProcess ends it first (as the DLL does).
	ExitProcess(0);
}
