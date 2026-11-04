#include "core/relay.h"

#include "core/log.h"
#include "core/relay_proto.h"
#include "core/steam.h"
#include "core/utils.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The game side of xlive_relay.exe. Requests that return something wait for the helper's reply (a few hundred
// microseconds); sends don't wait. The helper reads the channels the game reads and pushes their messages here, so
// receiving is local. Its callbacks queue for the game thread's pump; its call results are kept for SteamCall::Poll.

namespace xls::relay {

namespace {

HANDLE g_toHelper = nullptr, g_fromHelper = nullptr, g_process = nullptr;
bool g_active = false;
std::mutex g_writeMutex;

std::mutex g_mutex; // everything below
std::condition_variable g_cv;
uint32_t g_seq = 0;
std::map<uint32_t, std::vector<uint8_t>> g_replies;
std::set<uint32_t> g_waiting;
bool g_helloDone = false, g_helloOk = false;
std::string g_helloError;
std::deque<std::vector<uint8_t>> g_callbacks; // i32 id + struct
struct Result {
	int callback;
	bool failed;
	std::vector<uint8_t> data;
};
std::map<uint64_t, Result> g_results;
std::set<uint64_t> g_calls;                    // ours, not yet polled done
std::map<int, std::deque<SteamNetworkingMessage_t*>> g_inbox;
std::set<int> g_listening;

const DWORD kReplyTimeoutMs = 5000;

bool WriteFrame(uint16_t op, uint32_t seq, const Out& o)
{
	Frame f{(uint32_t)o.b.size(), op, 0, seq};
	std::vector<uint8_t> all(sizeof f + o.b.size());
	memcpy(all.data(), &f, sizeof f);
	if (!o.b.empty()) {
		memcpy(all.data() + sizeof f, o.b.data(), o.b.size());
	}
	std::lock_guard<std::mutex> lock(g_writeMutex);
	DWORD done = 0;
	return WriteFile(g_toHelper, all.data(), (DWORD)all.size(), &done, nullptr) && done == all.size();
}

bool ReadAll(void* p, DWORD n)
{
	while (n) {
		DWORD got = 0;
		if (!ReadFile(g_fromHelper, p, n, &got, nullptr) || !got) {
			return false;
		}
		p = (uint8_t*)p + got;
		n -= got;
	}
	return true;
}

void FreeMessage(SteamNetworkingMessage_t* m)
{
	free(m);
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
		std::lock_guard<std::mutex> lock(g_mutex);
		if (f.op == kHello) {
			In in(body.data(), body.size());
			g_helloOk = in.get<uint8_t>() != 0;
			in.get<uint64_t>();
			g_helloError = in.str();
			g_helloDone = true;
		}
		else if (f.op == kEvMessage) {
			In in(body.data(), body.size());
			int channel = in.get<int32_t>();
			SteamNetworkingIdentity id = in.get<SteamNetworkingIdentity>();
			SteamNetworkingMessage_t* m = (SteamNetworkingMessage_t*)calloc(1, sizeof(SteamNetworkingMessage_t) + in.n);
			memcpy(m + 1, in.p, in.n);
			m->m_pData = m + 1;
			m->m_cbSize = (int)in.n;
			m->m_identityPeer = id;
			m->m_nChannel = channel;
			m->m_usecTimeReceived = (SteamNetworkingMicroseconds)GetTickCount64() * 1000;
			m->m_pfnRelease = &FreeMessage;
			g_inbox[channel].push_back(m);
		}
		else if (f.op == kEvCallback) {
			g_callbacks.push_back(std::move(body));
		}
		else if (f.op == kEvCallResult) {
			In in(body.data(), body.size());
			uint64_t call = in.get<uint64_t>();
			Result r;
			r.callback = in.get<int32_t>();
			r.failed = in.get<uint8_t>() != 0;
			r.data.assign(in.p, in.p + in.n);
			g_results[call] = std::move(r);
		}
		else if (g_waiting.count(f.seq)) {
			g_replies[f.seq] = std::move(body);
		}
		g_cv.notify_all();
	}
	XLS_LOG_ERROR("relay: the helper is gone; lobbies and peers stop working until the game restarts.");
	std::lock_guard<std::mutex> lock(g_mutex);
	g_active = false;
	g_helloDone = true;
	g_cv.notify_all();
}

// A request that returns something: its reply's payload (empty when the helper is gone).
std::vector<uint8_t> Ask(Op op, const Out& o)
{
	uint32_t seq;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_active) {
			return {};
		}
		seq = ++g_seq ? g_seq : ++g_seq;
		g_waiting.insert(seq);
	}
	std::vector<uint8_t> reply;
	if (WriteFrame(op, seq, o)) {
		std::unique_lock<std::mutex> lock(g_mutex);
		if (g_cv.wait_for(lock, std::chrono::milliseconds(kReplyTimeoutMs), [&] { return g_replies.count(seq) || !g_active; })
			&& g_replies.count(seq)) {
			reply = std::move(g_replies[seq]);
			g_replies.erase(seq);
		}
		else if (g_active) {
			XLS_LOG_WARN("relay: no reply to request %u.", (unsigned)op);
		}
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_waiting.erase(seq);
	return reply;
}

void Tell(Op op, const Out& o)
{
	if (g_active) {
		WriteFrame(op, 0, o);
	}
}

template <typename T> T AskValue(Op op, const Out& o)
{
	std::vector<uint8_t> r = Ask(op, o);
	In in(r.data(), r.size());
	return in.get<T>();
}

// Strings handed out as const char*: kept for the next 64 (Steam's own only last until the next call).
const char* Keep(std::string s)
{
	static std::mutex m;
	static std::string ring[64];
	static unsigned next = 0;
	std::lock_guard<std::mutex> lock(m);
	std::string& slot = ring[next++ % 64];
	slot = std::move(s);
	return slot.c_str();
}

const char* AskString(Op op, const Out& o)
{
	std::vector<uint8_t> r = Ask(op, o);
	In in(r.data(), r.size());
	return Keep(in.str());
}

SteamAPICall_t AskCall(Op op, const Out& o)
{
	SteamAPICall_t call = AskValue<uint64_t>(op, o);
	if (call != k_uAPICallInvalid) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_calls.insert(call);
	}
	return call;
}

Out Lobby(CSteamID lobby)
{
	Out o;
	o.put<uint64_t>(lobby.ConvertToUint64());
	return o;
}

class Mm final : public ISteamMatchmaking {
public:
	int GetFavoriteGameCount() override { return 0; }
	bool GetFavoriteGame(int, AppId_t*, uint32*, uint16*, uint16*, uint32*, uint32*) override { return false; }
	int AddFavoriteGame(AppId_t, uint32, uint16, uint16, uint32, uint32) override { return 0; }
	bool RemoveFavoriteGame(AppId_t, uint32, uint16, uint16, uint32) override { return false; }
	SteamAPICall_t RequestLobbyList() override { return AskCall(kRequestLobbyList, Out()); }
	void AddRequestLobbyListStringFilter(const char* k, const char* v, ELobbyComparison c) override
	{
		Out o;
		o.str(k).str(v).put<int32_t>(c);
		Tell(kAddStringFilter, o);
	}
	void AddRequestLobbyListNumericalFilter(const char* k, int v, ELobbyComparison c) override
	{
		Out o;
		o.str(k).put<int32_t>(v).put<int32_t>(c);
		Tell(kAddNumericalFilter, o);
	}
	void AddRequestLobbyListNearValueFilter(const char* k, int v) override
	{
		Out o;
		o.str(k).put<int32_t>(v);
		Tell(kAddNearFilter, o);
	}
	void AddRequestLobbyListFilterSlotsAvailable(int n) override { Tell(kAddSlotsFilter, Out().put<int32_t>(n)); }
	void AddRequestLobbyListDistanceFilter(ELobbyDistanceFilter d) override { Tell(kAddDistanceFilter, Out().put<int32_t>(d)); }
	void AddRequestLobbyListResultCountFilter(int n) override { Tell(kAddResultCountFilter, Out().put<int32_t>(n)); }
	void AddRequestLobbyListCompatibleMembersFilter(CSteamID) override {}
	CSteamID GetLobbyByIndex(int i) override { return CSteamID((uint64)LobbyByIndex(i)); }
	SteamAPICall_t CreateLobby(ELobbyType type, int max) override { return AskCall(kCreateLobby, Out().put<int32_t>(type).put<int32_t>(max)); }
	SteamAPICall_t JoinLobby(CSteamID lobby) override { return AskCall(kJoinLobby, Lobby(lobby)); }
	void LeaveLobby(CSteamID lobby) override { Tell(kLeaveLobby, Lobby(lobby)); }
	bool InviteUserToLobby(CSteamID lobby, CSteamID user) override
	{
		return AskValue<uint8_t>(kInviteUserToLobby, Lobby(lobby).put<uint64_t>(user.ConvertToUint64())) != 0;
	}
	int GetNumLobbyMembers(CSteamID lobby) override { return AskValue<int32_t>(kGetNumLobbyMembers, Lobby(lobby)); }
	CSteamID GetLobbyMemberByIndex(CSteamID lobby, int i) override { return CSteamID((uint64)LobbyMember(lobby.ConvertToUint64(), i)); }
	const char* GetLobbyData(CSteamID lobby, const char* k) override { return AskString(kGetLobbyData, Lobby(lobby).str(k)); }
	bool SetLobbyData(CSteamID lobby, const char* k, const char* v) override
	{
		return AskValue<uint8_t>(kSetLobbyData, Lobby(lobby).str(k).str(v)) != 0;
	}
	int GetLobbyDataCount(CSteamID lobby) override { return AskValue<int32_t>(kGetLobbyDataCount, Lobby(lobby)); }
	bool GetLobbyDataByIndex(CSteamID lobby, int i, char* k, int kn, char* v, int vn) override
	{
		std::vector<uint8_t> r = Ask(kGetLobbyDataByIndex, Lobby(lobby).put<int32_t>(i));
		In in(r.data(), r.size());
		bool ok = in.get<uint8_t>() != 0;
		std::string ks = in.str(), vs = in.str();
		if (kn > 0) {
			snprintf(k, kn, "%s", ks.c_str());
		}
		if (vn > 0) {
			snprintf(v, vn, "%s", vs.c_str());
		}
		return ok;
	}
	bool DeleteLobbyData(CSteamID lobby, const char* k) override { return AskValue<uint8_t>(kDeleteLobbyData, Lobby(lobby).str(k)) != 0; }
	const char* GetLobbyMemberData(CSteamID lobby, CSteamID user, const char* k) override
	{
		return AskString(kGetLobbyMemberData, Lobby(lobby).put<uint64_t>(user.ConvertToUint64()).str(k));
	}
	void SetLobbyMemberData(CSteamID lobby, const char* k, const char* v) override { Tell(kSetLobbyMemberData, Lobby(lobby).str(k).str(v)); }
	bool SendLobbyChatMsg(CSteamID lobby, const void* p, int n) override
	{
		return AskValue<uint8_t>(kSendLobbyChatMsg, Lobby(lobby).raw(p, (size_t)n)) != 0;
	}
	int GetLobbyChatEntry(CSteamID, int, CSteamID*, void*, int, EChatEntryType*) override { return 0; }
	bool RequestLobbyData(CSteamID lobby) override { return AskValue<uint8_t>(kRequestLobbyData, Lobby(lobby)) != 0; }
	void SetLobbyGameServer(CSteamID, uint32, uint16, CSteamID) override {}
	bool GetLobbyGameServer(CSteamID, uint32*, uint16*, CSteamID*) override { return false; }
	bool SetLobbyMemberLimit(CSteamID lobby, int n) override { return AskValue<uint8_t>(kSetLobbyMemberLimit, Lobby(lobby).put<int32_t>(n)) != 0; }
	int GetLobbyMemberLimit(CSteamID lobby) override { return AskValue<int32_t>(kGetLobbyMemberLimit, Lobby(lobby)); }
	bool SetLobbyType(CSteamID lobby, ELobbyType t) override { return AskValue<uint8_t>(kSetLobbyType, Lobby(lobby).put<int32_t>(t)) != 0; }
	bool SetLobbyJoinable(CSteamID lobby, bool j) override { return AskValue<uint8_t>(kSetLobbyJoinable, Lobby(lobby).put<uint8_t>(j)) != 0; }
	CSteamID GetLobbyOwner(CSteamID lobby) override { return CSteamID((uint64)LobbyOwner(lobby.ConvertToUint64())); }
	bool SetLobbyOwner(CSteamID lobby, CSteamID user) override
	{
		return AskValue<uint8_t>(kSetLobbyOwner, Lobby(lobby).put<uint64_t>(user.ConvertToUint64())) != 0;
	}
	bool SetLinkedLobby(CSteamID, CSteamID) override { return false; }
};

Out Peer(const SteamNetworkingIdentity& id)
{
	Out o;
	o.put(id);
	return o;
}

class Msgs final : public ISteamNetworkingMessages {
public:
	EResult SendMessageToUser(const SteamNetworkingIdentity& id, const void* p, uint32 n, int flags, int channel) override
	{
		if (!g_active) {
			return k_EResultNoConnection;
		}
		Out o = Peer(id);
		o.put<int32_t>(flags).put<int32_t>(channel).raw(p, n);
		Tell(kSendMessage, o);
		return k_EResultOK;
	}
	int ReceiveMessagesOnChannel(int channel, SteamNetworkingMessage_t** out, int max) override
	{
		bool listen = false;
		int n = 0;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			listen = g_listening.insert(channel).second;
			auto it = g_inbox.find(channel);
			while (it != g_inbox.end() && n < max && !it->second.empty()) {
				out[n++] = it->second.front();
				it->second.pop_front();
			}
		}
		if (listen) {
			Tell(kListenChannel, Out().put<int32_t>(channel));
		}
		return n;
	}
	bool AcceptSessionWithUser(const SteamNetworkingIdentity& id) override { return AskValue<uint8_t>(kAcceptSession, Peer(id)) != 0; }
	bool CloseSessionWithUser(const SteamNetworkingIdentity& id) override { return AskValue<uint8_t>(kCloseSession, Peer(id)) != 0; }
	bool CloseChannelWithUser(const SteamNetworkingIdentity& id, int channel) override
	{
		return AskValue<uint8_t>(kCloseChannel, Peer(id).put<int32_t>(channel)) != 0;
	}
	ESteamNetworkingConnectionState GetSessionConnectionInfo(const SteamNetworkingIdentity& id, SteamNetConnectionInfo_t* info,
		SteamNetConnectionRealTimeStatus_t* status) override
	{
		std::vector<uint8_t> r = Ask(kSessionInfo, Peer(id));
		In in(r.data(), r.size());
		ESteamNetworkingConnectionState s = (ESteamNetworkingConnectionState)in.get<int32_t>();
		SteamNetConnectionInfo_t i = in.get<SteamNetConnectionInfo_t>();
		SteamNetConnectionRealTimeStatus_t q = in.get<SteamNetConnectionRealTimeStatus_t>();
		if (info) {
			*info = i;
		}
		if (status) {
			*status = q;
		}
		return in.ok ? s : k_ESteamNetworkingConnectionState_None;
	}
};

Mm g_mm;
Msgs g_msgs;

}

bool Start(uint32_t appId)
{
	SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
	HANDLE childRead = nullptr, childWrite = nullptr;
	if (!CreatePipe(&childRead, &g_toHelper, &sa, 1 << 16) || !CreatePipe(&g_fromHelper, &childWrite, &sa, 1 << 16)) {
		XLS_LOG_ERROR("relay: pipes failed (%lu).", GetLastError());
		return false;
	}
	SetHandleInformation(g_toHelper, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(g_fromHelper, HANDLE_FLAG_INHERIT, 0);
	std::wstring dir = ModuleDirectory();
	std::wstring exe = dir + L"xlive_relay.exe";
	std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring((uintptr_t)childRead) + L" " + std::to_wstring((uintptr_t)childWrite) + L" "
		+ std::to_wstring(appId) + L" \"" + dir + L"xlive_relay.log\"";
	STARTUPINFOW si{sizeof si};
	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
	CloseHandle(childRead);
	CloseHandle(childWrite);
	if (!ok) {
		XLS_LOG_ERROR("relay: can't start %ls (%lu); lobbies stay this app's.", exe.c_str(), GetLastError());
		CloseHandle(g_toHelper);
		CloseHandle(g_fromHelper);
		return false;
	}
	CloseHandle(pi.hThread);
	g_process = pi.hProcess;
	g_active = true;
	std::thread(Reader).detach();
	std::unique_lock<std::mutex> lock(g_mutex);
	g_cv.wait_for(lock, std::chrono::seconds(20), [] { return g_helloDone; });
	if (!g_helloOk) {
		XLS_LOG_ERROR("relay: the helper couldn't sign into Steam as app %u (%s); lobbies stay this app's.", appId,
			g_helloDone ? g_helloError.c_str() : "no answer");
		g_active = false;
		TerminateProcess(g_process, 1);
		return false;
	}
	XLS_LOG_INFO("relay: lobbies and peers go through app %u (xlive_relay.exe).", appId);
	return true;
}

bool Active()
{
	return g_active;
}

void Pump()
{
	std::deque<std::vector<uint8_t>> work;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		work.swap(g_callbacks);
	}
	for (const std::vector<uint8_t>& b : work) {
		if (b.size() >= 4) {
			SteamDispatchCallback(*(const int32_t*)b.data(), b.data() + 4, b.size() - 4);
		}
	}
}

ISteamMatchmaking* Matchmaking()
{
	return &g_mm;
}

ISteamNetworkingMessages* Messages()
{
	return &g_msgs;
}

bool IsProxy(const ISteamMatchmaking* mm)
{
	return mm == &g_mm;
}

uint64_t LobbyOwner(uint64_t lobby)
{
	return AskValue<uint64_t>(kGetLobbyOwner, Out().put<uint64_t>(lobby));
}

uint64_t LobbyMember(uint64_t lobby, int i)
{
	return AskValue<uint64_t>(kGetLobbyMemberByIndex, Out().put<uint64_t>(lobby).put<int32_t>(i));
}

uint64_t LobbyByIndex(int i)
{
	return AskValue<uint64_t>(kGetLobbyByIndex, Out().put<int32_t>(i));
}

bool CloudWrite(const char* name, const std::vector<uint8_t>& data)
{
	Out o;
	o.str(name).bytes(data.data(), data.size());
	std::vector<uint8_t> r = Ask(kCloudWrite, o);
	return r.size() == 1 && r[0];
}

bool CloudRead(const char* name, std::vector<uint8_t>* data)
{
	Out o;
	o.str(name);
	std::vector<uint8_t> r = Ask(kCloudRead, o);
	In in(r.data(), r.size());
	bool ok = in.get<uint8_t>() != 0;
	std::string bytes = in.str();
	if (!ok || !in.ok || bytes.empty()) {
		return false;
	}
	data->assign(bytes.begin(), bytes.end());
	return true;
}

bool SetConfigInt32(int value, int32_t v)
{
	return AskValue<uint8_t>(kSetConfigInt32, Out().put<int32_t>(value).put<int32_t>(v)) != 0;
}

int CallPoll(uint64_t call, void* out, int size, int callback, bool* failed)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_results.find(call);
	if (it == g_results.end()) {
		return g_calls.count(call) ? (g_active ? 0 : (*failed = true, 1)) : -1;
	}
	*failed = it->second.failed || it->second.callback != callback || (int)it->second.data.size() < size;
	if (!*failed) {
		memcpy(out, it->second.data.data(), size);
	}
	g_results.erase(it);
	g_calls.erase(call);
	return 1;
}

}
